/**
 * @file   server.js
 * @brief  NetNexus 拓扑编排后端：管理 Docker 容器并桥接浏览器到 telnet 3788
 * @author NetNexus Team
 * @date   2026-04-17
 *
 * 关键设计（与 scripts/ci/top_runner.py 对齐）:
 *   1) 容器以 `sleep infinity` 启动，而不是跑镜像默认 CMD。
 *   2) 启动前把 if_map.conf.gns3 挂进容器，把 GE-1..GE-N 映射到 eth1..ethN。
 *   3) 按 GE 端口序号从小到大 `docker network connect` 链路网络，保证 ethX
 *      的命名顺序与 GE-X 一致。
 *   4) 接口都就位后，`docker exec -d` 在容器里拉起 /opt/netnexus/bin/netnexus。
 *   5) 宿主机上暴露一个端口映射到容器 3788，供 WebSocket 终端桥接。
 *
 * 接口:
 *   GET    /api/images                列出本地可用的 netnexus 镜像
 *   GET    /api/instances             列出当前由本服务管理的实例
 *   POST   /api/instances             { id, image }  启动一个实例
 *   DELETE /api/instances/:id         停止并移除实例
 *   POST   /api/instances/cleanup     清理所有 nn-topo-* 容器
 *   GET    /api/links                 列出当前链路
 *   POST   /api/links                 { id, from, fromPort, to, toPort } 登记链路
 *   DELETE /api/links/:id             删除链路
 *   WS     /ws/terminal?id=<instId>   浏览器 <-> 容器 telnet 3788 双向桥接
 */

const express = require('express');
const cors = require('cors');
const { WebSocketServer } = require('ws');
const { execFile, spawn } = require('child_process');
const http = require('http');
const net = require('net');
const url = require('url');
const fs = require('fs');
const os = require('os');
const path = require('path');

const config = require('./config');
const { isValidId, isValidPort, isValidImage, base64DecodedSize } = require('./validation');

config.validateOrExit();
config.dump();

const PORT = config.PORT;
const DOCKER = process.env.DOCKER_BIN || 'docker';
const USE_SUDO = process.env.USE_SUDO === '1';

/** 容器内 if_map 的固定路径，与 CI 使用的一致 */
const IF_MAP_PATH_IN_CONTAINER = '/opt/netnexus/resources/if/if_map.conf.gns3';
/** 宿主机临时目录，用来放每个容器的 if_map 文件 */
const IF_MAP_HOST_DIR = path.join(os.tmpdir(), 'nn-topo-ifmap');
try { fs.mkdirSync(IF_MAP_HOST_DIR, { recursive: true }); } catch (_) { /* ignore */ }

/** netnexus 在容器里的 sqlite db 位置，用来做"导出/恢复配置" */
const NN_DB_PATH_IN_CONTAINER = '/opt/netnexus/data/netnexus.db';
/** 宿主机临时目录：做 docker cp 中转 */
const DB_TMP_HOST_DIR = path.join(os.tmpdir(), 'nn-topo-db');
try { fs.mkdirSync(DB_TMP_HOST_DIR, { recursive: true }); } catch (_) { /* ignore */ }

/** 容器内启动 netnexus 的 bash 片段（与 CI 基本一致） */
const NN_START_SH = [
    'mkdir -p /opt/netnexus/log /opt/netnexus/log/asan /opt/netnexus/data',
    'export NN_WORK_DIR=/opt/netnexus',
    'export LD_LIBRARY_PATH=/opt/netnexus/lib:${LD_LIBRARY_PATH}',
    'export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=0:abort_on_error=0:log_path=/opt/netnexus/log/asan/asan}"',
    'exec /opt/netnexus/bin/netnexus > /tmp/netnexus.log 2>&1'
].join(' && ');

const app = express();
app.set('trust proxy', true); // nginx 反代场景下取真实客户端 IP 做限流

// CORS：prod 严格白名单，dev 放开
const corsOptions = config.IS_PROD
    ? {
        origin(origin, cb)
        {
            // 同源 / 服务端到服务端（无 Origin）直接放行
            if (!origin) return cb(null, true);
            if (config.ALLOWED_ORIGINS.length === 0) return cb(null, true);
            if (config.ALLOWED_ORIGINS.includes(origin)) return cb(null, true);
            return cb(new Error(`CORS: origin ${origin} not allowed`));
        },
        credentials: true
    }
    : { origin: true, credentials: true };
app.use(cors(corsOptions));

// body 大小上限，防止超大 dbBase64 / 恶意请求打爆内存
app.use(express.json({ limit: config.BODY_LIMIT }));

// 基础安全响应头（替代 helmet，省一个依赖）
app.use((_req, res, next) =>
{
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.setHeader('X-Frame-Options', 'DENY');
    res.setHeader('Referrer-Policy', 'no-referrer');
    next();
});

// 鉴权：生产模式 + 设了 NN_AUTH_TOKEN 时校验 X-NN-Token
function requireAuth(req, res, next)
{
    if (!config.IS_PROD || !config.AUTH_TOKEN) return next();
    const got = req.get('X-NN-Token') || req.query.token;
    if (got !== config.AUTH_TOKEN)
    {
        return res.status(401).json({ error: 'unauthorized' });
    }
    next();
}

// 每 IP 滑动窗口限流（仅对写操作生效）
const rateBuckets = new Map(); // ip -> [ts, ts, ...]
function rateLimitMutations(req, res, next)
{
    if (req.method === 'GET' || req.method === 'HEAD' || req.method === 'OPTIONS') return next();
    const ip = req.ip || req.socket.remoteAddress || 'unknown';
    const now = Date.now();
    const winStart = now - config.RATE_LIMIT_WINDOW_MS;
    const bucket = (rateBuckets.get(ip) || []).filter(t => t >= winStart);
    if (bucket.length >= config.RATE_LIMIT_MAX)
    {
        console.warn(`[rate-limit] ${ip} exceeded ${config.RATE_LIMIT_MAX}/${config.RATE_LIMIT_WINDOW_MS}ms`);
        return res.status(429).json({ error: 'rate limit exceeded' });
    }
    bucket.push(now);
    rateBuckets.set(ip, bucket);
    // 懒回收，每 200 个不同 IP 扫一次
    if (rateBuckets.size > 200)
    {
        for (const [k, v] of rateBuckets)
        {
            const kept = v.filter(t => t >= winStart);
            if (kept.length === 0) rateBuckets.delete(k);
            else rateBuckets.set(k, kept);
        }
    }
    next();
}

// 简单访问日志（审计 + 排错）
app.use((req, _res, next) =>
{
    if (req.path.startsWith('/api/'))
    {
        const ip = req.ip || req.socket.remoteAddress || '-';
        console.log(`[http] ${ip} ${req.method} ${req.originalUrl}`);
    }
    next();
});

app.use('/api', rateLimitMutations);
app.use('/api', requireAuth);

/** 内存实例表，重启服务即丢失 */
const instances = new Map(); // id -> { id, image, containerName, hostPort, status, createdAt, ifMapFile, linkNets }

/** 内存链路表：每根线对应一个 docker bridge network */
const links = new Map(); // id -> { id, from, to, fromPort, toPort, networkName, wired }

/** 停机时把容器里 /opt/netnexus/data 整个 tar.base64 存这里，下次 start 再灌回。
 *  单独存避免挂在 inst 对象上让 GET /api/instances 列表返回巨大 payload。 */
const stoppedDbs = new Map(); // id -> base64 string

let nextHostPort = 13788;

/** 每台设备固定对外呈现 4 个 GE 口，没用到的 GE 槽位挂一个该设备私有的 stub 网络，
 *  这样容器里一定有 eth1..eth4，if_map 可以稳定写 GE-1=eth1 .. GE-4=eth4。 */
const GE_PORT_COUNT = 4;

function dockerArgs(args) { return USE_SUDO ? ['docker', ...args] : args; }
function dockerBin() { return USE_SUDO ? 'sudo' : DOCKER; }

function runDocker(args, timeoutMs = config.DOCKER_TIMEOUT_MS)
{
    return new Promise((resolve, reject) =>
    {
        execFile(dockerBin(), dockerArgs(args), { timeout: timeoutMs, maxBuffer: 64 * 1024 * 1024 }, (err, stdout, stderr) =>
        {
            if (err)
            {
                err.stderr = stderr;
                err.stdout = stdout;
                err.dockerArgs = args;
                return reject(err);
            }
            resolve({ stdout, stderr });
        });
    });
}

/** 像 runDocker 但 stdout 以 Buffer 形式返回，用于二进制数据（tar/db etc.） */
function runDockerBuffer(args, timeoutMs = 120000)
{
    return new Promise((resolve, reject) =>
    {
        execFile(dockerBin(), dockerArgs(args),
            { timeout: timeoutMs, maxBuffer: 256 * 1024 * 1024, encoding: 'buffer' },
            (err, stdout, stderr) =>
            {
                if (err)
                {
                    err.stderr = stderr ? stderr.toString() : '';
                    err.stdout = stdout;
                    err.dockerArgs = args;
                    return reject(err);
                }
                resolve({ stdout, stderr });
            });
    });
}

/** 用 spawn 把 stdin 喂给 docker exec -i ...，用于 tar 解压回灌等场景 */
function runDockerStdin(args, inputBuffer, timeoutMs = 120000)
{
    return new Promise((resolve, reject) =>
    {
        const p = spawn(dockerBin(), dockerArgs(args), { stdio: ['pipe', 'pipe', 'pipe'] });
        let stderr = '';
        let stdout = '';
        const t = setTimeout(() => { try { p.kill('SIGKILL'); } catch (_) { /* ignore */ } }, timeoutMs);
        p.stderr.on('data', d => { stderr += d.toString(); });
        p.stdout.on('data', d => { stdout += d.toString(); });
        p.on('error', err => { clearTimeout(t); reject(err); });
        p.on('close', code =>
        {
            clearTimeout(t);
            if (code === 0) resolve({ stdout, stderr });
            else
            {
                const err = new Error(`docker ${args.join(' ')} exited ${code}: ${stderr.trim()}`);
                err.stderr = stderr;
                err.dockerArgs = args;
                reject(err);
            }
        });
        p.stdin.on('error', () => { /* 可能 docker 已经退出，吞掉避免 EPIPE 抛 */ });
        p.stdin.end(inputBuffer);
    });
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

/** 同步确保容器里 netnexus 进程被彻底清掉，避免 restart 出现幽灵进程 */
async function killNetnexusInContainer(containerName)
{
    await runDocker([
        'exec', containerName, '/bin/sh', '-c',
        'pkill -x netnexus 2>/dev/null; for i in 1 2 3 4 5; do pgrep -x netnexus >/dev/null || exit 0; sleep 0.2; done; pkill -9 -x netnexus 2>/dev/null; sleep 0.2; true'
    ]).catch(() => { /* 没进程 pkill 会非零，不是错 */ });
}

/**
 * 等 netnexus 进程起来 + 3788 端口进入 LISTEN。
 * 为兼容各类镜像（可能没装 ss/netstat/awk），用多种手段串联：
 *   1) `pgrep -x netnexus` 先确认进程活着
 *   2) 再通过 `/proc/net/tcp(6)` 的 grep 判断 0xECC(=3788) 在 0A(=LISTEN)
 */
async function waitForNetnexusReady(containerName, maxMs = 5000)
{
    const deadline = Date.now() + maxMs;
    while (Date.now() < deadline)
    {
        try
        {
            await runDocker(['exec', containerName, 'pgrep', '-x', 'netnexus']);
            // /proc/net/tcp 每行形如：
            //   sl local_address rem_address st ...
            // 列 2 = "<hex-ip>:<hex-port>"（port 4 位大写 hex），列 4 = 2 位 hex state。
            // 我们要匹配 port 0ECC 且 state 0A。用 grep 的字段约束（端口后跟空格 + remote）：
            const { stdout } = await runDocker([
                'exec', containerName, '/bin/sh', '-c',
                "grep -E ':0ECC [0-9A-F]+:[0-9A-F]+ 0A ' /proc/net/tcp /proc/net/tcp6 2>/dev/null | head -n1"
            ]);
            if (stdout.trim().length > 0) return true;
        }
        catch (_) { /* 继续重试 */ }
        await sleep(200);
    }
    return false;
}

/** 读 netnexus 模块日志（`$NN_WORK_DIR/log/*.log`）的尾巴，排错用 */
async function tailNetnexusLog(containerName, lines = 30)
{
    try
    {
        const { stdout } = await runDocker([
            'exec', containerName, '/bin/sh', '-c',
            `for f in /opt/netnexus/log/main.log /opt/netnexus/log/cfg.log /tmp/netnexus.log; do ` +
            `if [ -s "$f" ]; then echo "==> $f <=="; tail -n ${lines} "$f"; fi; done`
        ]);
        return stdout;
    }
    catch (_) { return ''; }
}

// -------- 工具 --------

function sanitizeId(s) { return String(s).replace(/[^a-zA-Z0-9_.-]/g, '_'); }

/**
 * 容器级 hardening：cgroups 资源上限 + no-new-privileges。
 * 只在 config 里配了对应值才注入，方便 dev 环境完全不限制。
 */
function containerHardeningArgs()
{
    const a = ['--security-opt', 'no-new-privileges:true'];
    if (config.CONTAINER_MEMORY)
    {
        a.push('--memory', config.CONTAINER_MEMORY);
        a.push('--memory-swap', config.CONTAINER_MEMORY);
    }
    if (config.CONTAINER_CPUS)       a.push('--cpus', String(config.CONTAINER_CPUS));
    if (config.CONTAINER_PIDS > 0)   a.push('--pids-limit', String(config.CONTAINER_PIDS));
    if (config.CONTAINER_NOFILE > 0) a.push('--ulimit', `nofile=${config.CONTAINER_NOFILE}:${config.CONTAINER_NOFILE}`);
    return a;
}
function linkNetworkName(linkId) { return `nn-link-${sanitizeId(linkId).slice(0, 40)}`; }
function stubNetworkName(instanceId, geIdx) { return `nn-stub-${sanitizeId(instanceId).slice(0, 32)}-${geIdx}`; }

/** 把 'GE-3' 这种端口名解析成整数 3，失败返回 0 */
function parsePortIndex(portName)
{
    const m = /^GE-(\d+)$/.exec(String(portName || ''));
    if (!m) return 0;
    return parseInt(m[1], 10);
}

/**
 * 为实例规划 4 个 GE 口的 docker 网络：
 *   - 用到的 GE-N 指向 link 对应的 docker 网络
 *   - 未用到的 GE-N 指向一个仅此设备可见的 stub 网络
 * 返回数组长度固定 4，按 GE-1..GE-4 顺序。
 */
function planInterfacesOf(instanceId)
{
    /** @type {{ port: string, geIdx: number, link: any | null }[]} */
    const slots = [];
    for (let i = 1; i <= GE_PORT_COUNT; i++)
    {
        slots.push({ port: `GE-${i}`, geIdx: i, link: null });
    }
    for (const link of links.values())
    {
        if (link.from === instanceId)
        {
            const idx = parsePortIndex(link.fromPort);
            if (idx >= 1 && idx <= GE_PORT_COUNT) slots[idx - 1].link = link;
        }
        if (link.to === instanceId)
        {
            const idx = parsePortIndex(link.toPort);
            if (idx >= 1 && idx <= GE_PORT_COUNT) slots[idx - 1].link = link;
        }
    }
    return slots;
}

/**
 * 写 if_map 文件：固定写 GE-1..GE-4 = eth1..eth4，与 connect 顺序严格对齐。
 *
 * 文件名带时间戳，每次启动都用新名字。这样能避开两个坑：
 *   1) docker `-v src:dst` 当 src 不存在时会偷偷把 src 建成 root 拥有的空目录，
 *      下次 writeFileSync 直接 EISDIR；
 *   2) 即便宿主机 /tmp/nn-topo-ifmap 里残留了上述 root-owned 孤儿目录，
 *      新文件名和它撞不上，新一轮 docker run 也能挂上正确的文件。
 */
function writeIfMapFile(instanceId)
{
    const stamp = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;
    const file = path.join(IF_MAP_HOST_DIR, `${sanitizeId(instanceId)}-${stamp}-if_map.conf.gns3`);
    // 极端情况下时间戳撞了名字，把存量先清掉
    try
    {
        const st = fs.lstatSync(file);
        if (st.isDirectory()) fs.rmSync(file, { recursive: true, force: true });
        else fs.unlinkSync(file);
    }
    catch (e)
    {
        if (e && e.code !== 'ENOENT') throw e;
    }
    const lines = ['# Auto-generated by nn-topo web backend\n'];
    for (let i = 1; i <= GE_PORT_COUNT; i++)
    {
        lines.push(`GE-${i} = eth${i}\n`);
    }
    fs.writeFileSync(file, lines.join(''), 'utf8');
    return file;
}

async function dockerNetworkExists(name)
{
    try
    {
        const { stdout } = await runDocker(['network', 'ls', '--filter', `name=^${name}$`, '--format', '{{.Name}}']);
        return stdout.trim() === name;
    }
    catch (_) { return false; }
}

async function dockerContainerExists(name)
{
    try
    {
        const { stdout } = await runDocker(['ps', '-a', '--filter', `name=^${name}$`, '--format', '{{.Names}}']);
        return stdout.split('\n').map(s => s.trim()).includes(name);
    }
    catch (_) { return false; }
}

async function dockerContainerRunning(name)
{
    try
    {
        const { stdout } = await runDocker(['inspect', '-f', '{{.State.Running}}', name]);
        return stdout.trim() === 'true';
    }
    catch (_) { return false; }
}

/**
 * 为 nn-link-* / nn-stub-* 自己管子网。
 * docker 默认每个 bridge 网络要吃一整个 /16，地址池(`172.16.0.0/12` + `192.168.0.0/16`)
 * 顶多塞 30 多个网络就用光了。我们一台设备 4 个口、动辄 8 个网络，很容易触顶
 * 表现就是新设备 docker run / network create 卡住直到超时。
 *
 * 自己用 /29（每段 6 个可用 IP，对一个 link 上那 2 端足够），从 10.200.0.0/16 里发，
 * 理论上可以吃到 8192 个网络，对 web UI 拓扑场景绰绰有余。
 */
const SUBNET_POOL_PREFIX = '10.200';

async function listUsedSubnets()
{
    const used = new Set();
    let stdout;
    try
    {
        ({ stdout } = await runDocker(['network', 'ls', '--format', '{{.Name}}']));
    }
    catch (_) { return used; }
    const names = stdout.split('\n').map(s => s.trim()).filter(n => n.startsWith('nn-link-') || n.startsWith('nn-stub-'));
    for (const n of names)
    {
        try
        {
            const { stdout: sub } = await runDocker(['inspect', '-f', '{{range .IPAM.Config}}{{.Subnet}}{{end}}', n]);
            const s = sub.trim();
            if (s) used.add(s);
        }
        catch (_) { /* ignore */ }
    }
    return used;
}

async function pickFreeSubnet29()
{
    const used = await listUsedSubnets();
    // /29 步长 = 8，每个 /24 能塞 32 个 /29
    for (let i = 0; i < 8192; i++)
    {
        const block  = (i >> 5) & 0xff;
        const offset = (i & 31) * 8;
        const candidate = `${SUBNET_POOL_PREFIX}.${block}.${offset}/29`;
        if (!used.has(candidate)) return candidate;
    }
    throw new Error('no free /29 subnet in 10.200.0.0/16 pool');
}

async function createBridgeNetwork(name)
{
    const subnet = await pickFreeSubnet29();
    await runDocker(['network', 'create', '--driver', 'bridge', '--internal', '--subnet', subnet, name]);
}

async function ensureLinkNetwork(link)
{
    const name = link.networkName || linkNetworkName(link.id);
    link.networkName = name;
    if (!(await dockerNetworkExists(name)))
    {
        await createBridgeNetwork(name);
    }
    return name;
}

async function ensureStubNetwork(instanceId, geIdx)
{
    const name = stubNetworkName(instanceId, geIdx);
    if (!(await dockerNetworkExists(name)))
    {
        await createBridgeNetwork(name);
    }
    return name;
}

async function containerOnNetwork(containerName, networkName)
{
    try
    {
        const { stdout } = await runDocker(['inspect', '-f', '{{json .NetworkSettings.Networks}}', containerName]);
        const map = JSON.parse(stdout.trim() || '{}');
        return Object.prototype.hasOwnProperty.call(map, networkName);
    }
    catch (_) { return false; }
}

async function unwireLink(link)
{
    if (!link.networkName) return;
    const { stdout } = await runDocker(
        ['network', 'inspect', '-f', '{{range .Containers}}{{.Name}}\n{{end}}', link.networkName]
    ).catch(() => ({ stdout: '' }));
    const names = stdout.split('\n').map(s => s.trim()).filter(Boolean);
    for (const n of names)
    {
        await runDocker(['network', 'disconnect', '-f', link.networkName, n]).catch(() => {});
    }
    await runDocker(['network', 'rm', link.networkName]).catch(() => {});
    link.wired = false;
}

// -------- Images --------

app.get('/api/images', async (_req, res) =>
{
    try
    {
        const { stdout } = await runDocker(['images', '--format', '{{.Repository}}:{{.Tag}}\t{{.ID}}\t{{.Size}}']);
        const images = stdout.trim().split('\n')
            .filter(Boolean)
            .map(line =>
            {
                const [repoTag, id, size] = line.split('\t');
                return { name: repoTag, id, size };
            })
            .filter(img => /netnexus/i.test(img.name));

        if (images.length === 0)
        {
            images.push({ name: 'netnexus:latest', id: '-', size: '(not built)' });
            images.push({ name: 'netnexus:debug', id: '-', size: '(not built)' });
        }
        res.json({ images });
    }
    catch (e)
    {
        res.status(500).json({ error: 'docker images failed', detail: String(e.stderr || e.message) });
    }
});

// -------- Instances --------

app.get('/api/instances', (_req, res) =>
{
    res.json({ instances: Array.from(instances.values()) });
});

/**
 * 启动一个 NetNexus 容器。流程完全按 CI 的顺序：
 *   1) 收集 links，写 if_map
 *   2) docker run --cap-add NET_ADMIN/NET_RAW -p host:3788 -v ifmap image sleep infinity
 *   3) 按 GE 序号依次 docker network connect
 *   4) docker exec -d 启动 netnexus
 */
app.post('/api/instances', async (req, res) =>
{
    const { id, image } = req.body || {};
    let dbBase64 = (req.body || {}).dbBase64 || null;
    if (!id || !image)
    {
        return res.status(400).json({ error: 'id and image required' });
    }
    if (!isValidId(id))
    {
        return res.status(400).json({ error: 'invalid id', detail: '仅允许字母数字和 ._- ，以字母数字开头，长度 3~64' });
    }
    if (!isValidImage(image, config.IMAGE_ALLOWLIST))
    {
        return res.status(400).json({ error: 'invalid image', detail: `镜像不在白名单内（允许前缀：${config.IMAGE_ALLOWLIST.join(', ') || '*'}）` });
    }
    if (dbBase64)
    {
        const decoded = base64DecodedSize(dbBase64);
        if (decoded > config.MAX_DB_BYTES)
        {
            return res.status(413).json({ error: 'dbBase64 too large', detail: `解码后 ${decoded} 字节，上限 ${config.MAX_DB_BYTES}` });
        }
    }
    // 实例数上限（幂等复用已有实例不计入配额）
    if (!instances.has(id) && instances.size >= config.MAX_INSTANCES)
    {
        return res.status(429).json({ error: 'instance quota exceeded', detail: `当前已有 ${instances.size} 个，上限 ${config.MAX_INSTANCES}` });
    }

    const containerName = `nn-topo-${id}`;

    // 复用上次停机时的 hostPort（让前端 terminal URL 保持稳定）
    let reservedHostPort = null;

    // 幂等：如果已经有容器且在跑，直接返回现有实例，不做任何破坏性操作。
    // 这样用户即便误点"启动"也不会丢容器内数据。
    if (instances.has(id))
    {
        const existing = instances.get(id);
        const containerRunning = existing.containerName && await dockerContainerRunning(existing.containerName);

        if (containerRunning)
        {
            // 容器还在跑（用户重复点启动、或刚 pkill 没等到 stop 真销毁）
            try
            {
                await killNetnexusInContainer(existing.containerName);
                await runDocker([
                    'exec', '-d', existing.containerName, '/bin/bash', '-lc', NN_START_SH
                ]);
                const ready = await waitForNetnexusReady(existing.containerName, 5000);
                if (!ready)
                {
                    const tail = await tailNetnexusLog(existing.containerName);
                    console.warn(`[backend] ${existing.containerName} netnexus not ready in 5s, log tail:\n${tail}`);
                    return res.status(500).json({ error: 'netnexus did not become ready', detail: tail || 'no log' });
                }
                existing.status = 'running';
                return res.json({ instance: existing, reused: true });
            }
            catch (e)
            {
                console.warn(`[backend] exec netnexus in ${existing.containerName} failed: ${e.stderr || e.message}`);
                return res.status(500).json({ error: 'exec netnexus failed', detail: String(e.stderr || e.message) });
            }
        }

        // 走到这里：status=stopped，或者容器被外力（docker daemon 重启等）搞没了。
        // 按当前 links 重建：复用 hostPort + 回灌 stoppedDbs 的 db。
        reservedHostPort = existing.hostPort || null;

        // 迁移/降级场景：后端 restart 时 registerExistingOnBoot 把旧 container 置为 stopped
        // 但容器还在，db 没导出到 stoppedDbs —— 这里补救，rm 前先导一次
        if (!stoppedDbs.has(id) && await dockerContainerExists(containerName))
        {
            try
            {
                const savedDb = await readDbFromContainer(containerName);
                if (savedDb) stoppedDbs.set(id, savedDb);
            }
            catch (e)
            {
                console.warn(`[backend] migrate-export db for ${id} failed: ${e.stderr || e.message}`);
            }
        }
        if (!dbBase64 && stoppedDbs.has(id)) dbBase64 = stoppedDbs.get(id);

        // 保险起见清一下残留的同名容器（/stop 正常路径已 rm 过）
        await runDocker(['rm', '-f', containerName]).catch(() => {});
        instances.delete(id);
    }
    // 没登记但宿主机上已有同名容器（前端缓存掉了但后端还知道），尝试继承而不是删掉
    else if (await dockerContainerExists(containerName))
    {
        if (await dockerContainerRunning(containerName))
        {
            const adopted = await tryAdoptOne(containerName, id, image);
            if (adopted) return res.json({ instance: adopted, reused: true });
        }
        // 存在但没跑：用户可能想重建，但为了避免误删，这里返回冲突让用户显式删除
        return res.status(409).json({
            error: 'container exists',
            detail: `容器 ${containerName} 已存在但未运行。如需重建请先显式删除该设备`
        });
    }

    const hostPort = reservedHostPort || nextHostPort++;
    if (hostPort >= nextHostPort) nextHostPort = hostPort + 1;

    let ifMapFile;
    try
    {
        // 1) 规划 4 个 GE 口：有 link 的用 link 网络，没 link 的用 stub 网络；写固定 if_map
        const slots = planInterfacesOf(id);
        ifMapFile = writeIfMapFile(id);

        // 到这里已经确认没有同名容器（或是用户允许的情况），不再 rm

        // 2) 为每个 GE 槽位确保 docker 网络存在（link 或 stub）
        const netsInOrder = [];
        for (const slot of slots)
        {
            const net = slot.link
                ? await ensureLinkNetwork(slot.link)
                : await ensureStubNetwork(id, slot.geIdx);
            netsInOrder.push({ net, slot });
        }

        // 3) 以 sleep infinity 拉起容器，挂 if_map
        //    eth0 是 docker 默认 bridge，承载 -p 端口映射和 CLI 3788；eth1..eth4 后面 connect
        await runDocker([
            'run', '-d',
            '--name', containerName,
            '--hostname', id,
            '--cap-add', 'NET_ADMIN',
            '--cap-add', 'NET_RAW',
            '--cap-add', 'SYS_PTRACE',
            '--security-opt', 'seccomp=unconfined',
            ...containerHardeningArgs(),
            '-e', 'NN_WORK_DIR=/opt/netnexus',
            '-e', 'LD_LIBRARY_PATH=/opt/netnexus/lib',
            '-v', `${ifMapFile}:${IF_MAP_PATH_IN_CONTAINER}:ro`,
            '-p', `${hostPort}:3788`,
            image,
            'sleep', 'infinity'
        ]);

        // 4) 严格按 GE-1..GE-4 顺序 attach，确保 ethN 命名对齐
        for (const item of netsInOrder)
        {
            await runDocker(['network', 'connect', item.net, containerName]);
        }

        // 4.5) 如果调用方带了 dbBase64（比如从导出文件恢复），在 netnexus 启动前把 db 灌进容器
        if (dbBase64)
        {
            try
            {
                await writeDbIntoContainer(containerName, dbBase64);
            }
            catch (e)
            {
                console.warn(`[backend] restore db for ${id} failed: ${e.stderr || e.message}`);
            }
        }

        // 5) exec 启动 netnexus
        await runDocker(['exec', '-d', containerName, '/bin/bash', '-lc', NN_START_SH]);
        console.log(`[backend] ${containerName} created, netnexus exec'd; hostPort=${hostPort}`);

        // 标记已接通的 link：两端都在跑（status=running）才算真的 wired
        for (const item of netsInOrder)
        {
            if (!item.slot.link) continue;
            const link = item.slot.link;
            const peer = link.from === id ? link.to : link.from;
            const peerInst = instances.get(peer);
            if (peerInst && peerInst.status === 'running') { link.wired = true; }
        }

        const inst = {
            id,
            image,
            containerName,
            hostPort,
            status: 'running',
            createdAt: Date.now(),
            ifMapFile,
            linkNets: netsInOrder.map(x => x.net),
            stubNets: netsInOrder.filter(x => !x.slot.link).map(x => x.net)
        };
        instances.set(id, inst);

        // 等 netnexus 真的 listen 3788 再返回（首次启动要加载全部模块，可能 >1s）。
        // 探测失败不阻塞成功返回：前端 bridgeTerminal 还有 6s 的 TCP 重试兜底。
        const t0 = Date.now();
        const ready = await waitForNetnexusReady(containerName, 10000);
        const t1 = Date.now();
        if (ready)
        {
            console.log(`[backend] ${containerName} netnexus ready in ${t1 - t0}ms`);
        }
        else
        {
            const tail = await tailNetnexusLog(containerName);
            console.warn(`[backend] ${containerName} netnexus NOT ready in ${t1 - t0}ms, returning anyway. log tail:\n${tail}`);
        }

        // 成功起来了，清掉上次停机保存的 db（已经灌回容器里）
        stoppedDbs.delete(id);

        res.json({ instance: inst });
    }
    catch (e)
    {
        const detail = String(e.stderr || e.message || 'unknown').trim();
        console.error(`[backend] POST /api/instances ${id} failed:`);
        console.error(`  args: docker ${(e.dockerArgs || []).join(' ')}`);
        console.error(`  err : ${detail}`);
        // 失败清理容器、if_map
        await runDocker(['rm', '-f', containerName]).catch(() => {});
        try { fs.unlinkSync(ifMapFile); } catch (_) { /* ignore */ }
        res.status(500).json({ error: 'docker run failed', detail });
    }
});

/**
 * 导出：把容器内 netnexus.db 的字节用 base64 返回。
 * 说明：netnexus 运行时 sqlite 没启 WAL，直接 docker cp 主库文件对配置类数据足够稳。
 * 如果对端此时没落盘（刚启动或 db 还没生成），返回 dbBase64: null，前端当"没 db 可导"处理。
 */
app.get('/api/instances/:id/db', async (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    const inst = instances.get(req.params.id);
    if (!inst)
    {
        return res.status(404).json({ error: 'instance not found' });
    }
    // 停机状态：容器已 rm，直接返回 /stop 时保存的快照
    if (inst.status === 'stopped')
    {
        const b64 = stoppedDbs.get(req.params.id) || null;
        return res.json({ id: req.params.id, dbBase64: b64, size: b64 ? Buffer.from(b64, 'base64').length : 0 });
    }
    try
    {
        const b64 = await readDbFromContainer(inst.containerName);
        res.json({ id: req.params.id, dbBase64: b64, size: b64 ? Buffer.from(b64, 'base64').length : 0 });
    }
    catch (e)
    {
        res.json({ id: req.params.id, dbBase64: null, error: String(e.stderr || e.message) });
    }
});

/**
 * netnexus 用 SQLite WAL 模式（netnexus.db / netnexus.db-wal / netnexus.db-shm 三件套）。
 * 只拷贝 .db 主文件等于只拿到几乎空的壳，配置全在 -wal 里。
 * 这里用容器内 tar 把整个 /opt/netnexus/data 目录打包成一个 stream，base64 后返回。
 */
async function readDbFromContainer(containerName)
{
    try
    {
        await runDocker(['exec', containerName, 'test', '-d', '/opt/netnexus/data']);
    }
    catch (_)
    {
        return null;
    }
    const { stdout } = await runDockerBuffer(
        ['exec', containerName, 'tar', '-cf', '-', '-C', '/opt/netnexus', 'data']
    );
    if (!stdout || stdout.length === 0) return null;
    return stdout.toString('base64');
}

/**
 * 反向写入：拿到 tar 字节流，把容器里 /opt/netnexus/data 整个覆盖。
 * 必须在 netnexus 进程启动之前调用（否则 sqlite 已经把空 db 打开占住了）。
 */
async function writeDbIntoContainer(containerName, tarBase64)
{
    const buf = Buffer.from(tarBase64, 'base64');
    if (buf.length === 0) return;
    // 兜底：清掉容器里的旧 data 目录（fresh 容器其实没有，但保险）
    await runDocker(['exec', containerName, 'rm', '-rf', '/opt/netnexus/data']).catch(() => {});
    await runDocker(['exec', containerName, 'mkdir', '-p', '/opt/netnexus']);
    await runDockerStdin(
        ['exec', '-i', containerName, 'tar', '-xf', '-', '-C', '/opt/netnexus'],
        buf
    );
}

/**
 * "停止" = 导出 db、docker rm -f 销毁容器、回收 stub 网络、link 置 unwired。
 *
 * 之所以真销毁：只有 fresh create 才能按当前 links 重新规划 GE-N → ethN 映射。
 * 如果仅 pkill netnexus、容器保活，用户停机时新加的 link 永远接不进来（新 link
 * 会产生 eth5+，打破 GE-N 的 1..4 固定布局，ping 不通）。
 *
 * docker stop + docker start 不能用的原因：Docker 重新 attach 网络的顺序不保证，
 * ethN 可能错位。所以走完整的 rm → run 路径，顺序由我们自己控制。
 *
 * 代价：停机/启动从秒级 pkill+exec 变成几秒级的 docker rm + run + connect。
 * inst 内配置通过 readDbFromContainer 保存到 stoppedDbs，下次 start 灌回。
 */
app.post('/api/instances/:id/stop', async (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    const inst = instances.get(req.params.id);
    if (!inst)
    {
        return res.status(404).json({ error: 'not found' });
    }
    if (inst.status === 'stopped')
    {
        return res.json({ ok: true, instance: inst, reused: true });
    }
    try
    {
        // 1) 导出容器里的 /opt/netnexus/data（含 sqlite db-wal）到内存
        let savedDb = null;
        try
        {
            savedDb = await readDbFromContainer(inst.containerName);
        }
        catch (e)
        {
            console.warn(`[backend] stop ${inst.containerName}: export db failed: ${e.stderr || e.message}`);
        }

        // 2) 先 pkill netnexus，防止 docker rm 时还在写盘
        await killNetnexusInContainer(inst.containerName).catch(() => {});

        // 3) 真销毁容器
        await runDocker(['rm', '-f', inst.containerName]).catch(() => {});

        // 4) 回收 stub 网络（这些本来就是该设备私有）
        for (const n of inst.stubNets || [])
        {
            await runDocker(['network', 'rm', n]).catch(() => {});
        }

        // 5) 标记相关 link unwired；对端也不在跑就顺手把 link 网络回收
        for (const link of links.values())
        {
            if (link.from === inst.id || link.to === inst.id)
            {
                link.wired = false;
                const otherId = link.from === inst.id ? link.to : link.from;
                const otherInst = instances.get(otherId);
                if (!otherInst || otherInst.status !== 'running')
                {
                    await unwireLink(link);
                }
            }
        }

        // 6) if_map 清掉（下次 start 按当前 links 重写）
        if (inst.ifMapFile)
        {
            try { fs.unlinkSync(inst.ifMapFile); } catch (_) { /* ignore */ }
        }

        // 7) 保留 inst 记录（id/image/hostPort 给 start 复用），db 单独存
        if (savedDb) stoppedDbs.set(inst.id, savedDb);
        else stoppedDbs.delete(inst.id);
        inst.status = 'stopped';
        inst.ifMapFile = null;
        inst.linkNets = [];
        inst.stubNets = [];

        res.json({ ok: true, instance: inst, dbSaved: !!savedDb });
    }
    catch (e)
    {
        res.status(500).json({ error: 'stop failed', detail: String(e.stderr || e.message) });
    }
});

app.delete('/api/instances/:id', async (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    const inst = instances.get(req.params.id);
    if (!inst)
    {
        const fallbackName = `nn-topo-${req.params.id}`;
        await runDocker(['rm', '-f', fallbackName]).catch(() => {});
        return res.status(404).json({ error: 'not found' });
    }
    try
    {
        // 标记链路 unwired（如果对端也没了就把网络回收）
        for (const link of links.values())
        {
            if (link.from === req.params.id || link.to === req.params.id)
            {
                link.wired = false;
                const otherId = link.from === req.params.id ? link.to : link.from;
                if (!instances.has(otherId) || otherId === req.params.id)
                {
                    await unwireLink(link);
                }
            }
        }
        await runDocker(['rm', '-f', inst.containerName]).catch(() => {});
        // 回收 stub 网络（这些网络本来就是这台设备私有的）
        for (const n of inst.stubNets || [])
        {
            await runDocker(['network', 'rm', n]).catch(() => {});
        }
        if (inst.ifMapFile) { try { fs.unlinkSync(inst.ifMapFile); } catch (_) { /* ignore */ } }
        instances.delete(req.params.id);
        stoppedDbs.delete(req.params.id);
        res.json({ ok: true });
    }
    catch (e)
    {
        res.status(500).json({ error: 'docker rm failed', detail: String(e.stderr || e.message) });
    }
});

/** 一键清理所有 nn-topo-* 容器（管理端按钮可调） */
app.post('/api/instances/cleanup', async (_req, res) =>
{
    let removed = 0;
    try
    {
        const { stdout } = await runDocker(['ps', '-a', '--filter', 'name=nn-topo-', '--format', '{{.Names}}']);
        const names = stdout.split('\n').map(s => s.trim()).filter(Boolean);
        for (const name of names)
        {
            await runDocker(['rm', '-f', name]).catch(() => {});
            removed++;
        }
        instances.clear();
        stoppedDbs.clear();
        res.json({ ok: true, removed });
    }
    catch (e)
    {
        res.status(500).json({ error: 'cleanup failed', detail: String(e.stderr || e.message) });
    }
});

// -------- Links --------

/**
 * 登记一条链路。
 *   - 两端都未启动：仅登记，等用户启动设备时会把网络 wire 上。
 *   - 一端/两端已启动：仍然只登记，不动已运行容器 —— 因为 if_map 已在容器创建时烧进去，
 *     之后再临时 network connect 会产生 eth5+ 的接口，CLI 的 GE-N 映射不到，ping 不通。
 *     这种情况下提示用户需要重启相关设备。
 */
app.post('/api/links', async (req, res) =>
{
    const { id, from, fromPort, to, toPort } = req.body || {};
    if (!id || !from || !to)
    {
        return res.status(400).json({ error: 'id, from, to required' });
    }
    if (!isValidId(id) || !isValidId(from) || !isValidId(to))
    {
        return res.status(400).json({ error: 'invalid id/from/to' });
    }
    if (!isValidPort(fromPort || '') || !isValidPort(toPort || ''))
    {
        return res.status(400).json({ error: 'invalid port', detail: '端口名需为 GE-N（N=1..99）' });
    }
    if (!links.has(id) && links.size >= config.MAX_LINKS)
    {
        return res.status(429).json({ error: 'link quota exceeded', detail: `当前已有 ${links.size} 条链路，上限 ${config.MAX_LINKS}` });
    }

    const existing = links.get(id);
    const link = existing || {
        id,
        from,
        fromPort: fromPort || '',
        to,
        toPort: toPort || '',
        networkName: linkNetworkName(id),
        wired: false
    };
    if (!existing) links.set(id, link);

    // 只有真在跑的容器才叫 "running end"（停机状态的 inst 记录也在 Map 里但 status=stopped）
    const runningEnds = [instances.get(from), instances.get(to)]
        .filter(x => x && x.status === 'running').length;
    const note = runningEnds === 0
        ? `已登记，启动两端设备时会自动接通（${link.networkName}）`
        : `已登记。检测到 ${runningEnds} 端在运行，需要重启对应设备使链路生效`;

    res.json({ ok: true, link, wired: false, needRestart: runningEnds > 0, note });
});

app.get('/api/links', (_req, res) =>
{
    res.json({ links: Array.from(links.values()) });
});

app.delete('/api/links/:id', async (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    const link = links.get(req.params.id);
    if (!link)
    {
        const name = linkNetworkName(req.params.id);
        await runDocker(['network', 'rm', name]).catch(() => {});
        return res.status(404).json({ error: 'not found' });
    }
    await unwireLink(link);
    links.delete(req.params.id);
    res.json({ ok: true });
});

// -------- 启动时继承已有容器 --------

/**
 * 不再强制清理 nn-topo-* / nn-link-* / nn-stub-*。
 *
 * 后端进程重启时，把本机上所有 nn-topo-* 容器继承回来：
 *   - 停着的容器先 docker start；
 *   - 启动后如果容器里没有 netnexus 进程，帮它 exec 起一次（netnexus 会从自己的 db 恢复）；
 *   - 读 hostPort / 已连的 docker 网络，重建内存里的 instances 记录；
 *   - nextHostPort 避让已经占用的端口号。
 *
 * 用户的配置始终保留在容器内部（/opt/netnexus/data 等），不会因为后端重启被清掉。
 * 只有用户在 UI 里显式"删除 / 清空"时才会走 DELETE /api/instances/:id 真正移除容器。
 */
/**
 * 继承单个已有容器。start + 重新 exec netnexus（幂等），读端口 / 挂载 / 网络，
 * 写入 instances Map。imageHint 可从 docker ps 来，否则自己 inspect。
 */
async function tryAdoptOne(containerName, idOverride = null, imageHint = null)
{
    const id = idOverride || containerName.replace(/^nn-topo-/, '');
    let image = imageHint;
    try
    {
        if (!image)
        {
            const { stdout } = await runDocker(['inspect', '-f', '{{.Config.Image}}', containerName]);
            image = stdout.trim();
        }
    }
    catch (_) { /* ignore */ }

    try
    {
        if (!(await dockerContainerRunning(containerName)))
        {
            await runDocker(['start', containerName]);
        }
    }
    catch (e)
    {
        console.warn(`  [skip] ${containerName}: start failed: ${e.stderr || e.message}`);
        return null;
    }

    let hostPort = 0;
    try
    {
        const { stdout: po } = await runDocker(['port', containerName, '3788/tcp']);
        const m = /:(\d+)\s*$/.exec((po.trim().split('\n')[0] || '').trim());
        if (m) hostPort = parseInt(m[1], 10);
    }
    catch (_) { /* ignore */ }

    // netnexus 进程可能因为 docker stop 丢掉了，幂等地重新 exec 一次
    await runDocker([
        'exec', '-d', containerName, '/bin/bash', '-lc',
        `pgrep -x netnexus >/dev/null || (${NN_START_SH})`
    ]).catch(() => { /* ignore */ });

    let ifMapFile = null;
    try
    {
        const { stdout: ms } = await runDocker(['inspect', '-f', '{{json .Mounts}}', containerName]);
        const mounts = JSON.parse(ms || '[]');
        const m = mounts.find(x => x && x.Destination === IF_MAP_PATH_IN_CONTAINER);
        if (m) ifMapFile = m.Source;
    }
    catch (_) { /* ignore */ }

    const linkNets = [];
    const stubNets = [];
    try
    {
        const { stdout: ns } = await runDocker(['inspect', '-f', '{{json .NetworkSettings.Networks}}', containerName]);
        const obj = JSON.parse(ns || '{}');
        for (const k of Object.keys(obj))
        {
            if (k.startsWith('nn-link-'))      linkNets.push(k);
            else if (k.startsWith('nn-stub-')) stubNets.push(k);
        }
    }
    catch (_) { /* ignore */ }

    if (hostPort >= nextHostPort) nextHostPort = hostPort + 1;

    const inst = {
        id,
        image: image || 'unknown',
        containerName,
        hostPort,
        status: 'running',
        createdAt: Date.now(),
        ifMapFile,
        linkNets,
        stubNets,
        adopted: true
    };
    instances.set(id, inst);
    return inst;
}

/**
 * 删掉所有"没容器挂着"的 nn-link-* / nn-stub-* 网络。
 * 旧的实现 + adopt 模式可能在 docker 里留一堆孤儿网络，每个吃一段地址池，
 * 留多了会让新的 docker network create 失败甚至 docker run 卡超时。
 */
async function cleanupOrphanNetworksOnBoot()
{
    let names;
    try
    {
        const { stdout } = await runDocker(['network', 'ls', '--format', '{{.Name}}']);
        names = stdout.split('\n').map(s => s.trim())
            .filter(n => n.startsWith('nn-link-') || n.startsWith('nn-stub-'));
    }
    catch (_) { return; }

    let removed = 0;
    for (const name of names)
    {
        try
        {
            const { stdout: cs } = await runDocker(['network', 'inspect', '-f', '{{len .Containers}}', name]);
            const count = parseInt(cs.trim(), 10);
            if (count === 0)
            {
                await runDocker(['network', 'rm', name]).catch(() => {});
                removed++;
            }
        }
        catch (_) { /* ignore */ }
    }
    if (removed > 0) console.log(`[backend] removed ${removed} orphan nn-* network(s)`);
}

/**
 * 只登记已有容器的状态到 instances 映射，不做 docker start，也不 exec netnexus。
 * 之所以要登记：用户在前端点击"启动"时，POST /api/instances 会通过 instances.has(id)
 * 找到这条记录，然后用 docker start 原地复用容器（保留 db 与配置）。
 */
async function registerExistingOnBoot()
{
    let rows;
    try
    {
        const { stdout } = await runDocker(
            ['ps', '-a', '--filter', 'name=nn-topo-', '--format', '{{.Names}}\t{{.Image}}']
        );
        rows = stdout.split('\n').map(s => s.trim()).filter(Boolean);
    }
    catch (e)
    {
        console.warn('[backend] register: list containers failed:', e.stderr || e.message);
        return;
    }

    if (!rows || rows.length === 0)
    {
        console.log('[backend] register: no existing nn-topo-* containers');
        return;
    }

    console.log(`[backend] registering ${rows.length} existing container(s) (no auto-start)`);
    for (const line of rows)
    {
        const [name, image] = line.split('\t');
        if (!name) continue;
        const id = name.replace(/^nn-topo-/, '');

        const containerRunning = await dockerContainerRunning(name);
        // 后端重启时把容器里残留的 netnexus 全部 kill 掉，统一置为 stopped，
        // 容器本身保留（避免 docker stop/start 导致网络重连序错位）。
        if (containerRunning)
        {
            await killNetnexusInContainer(name);
        }

        let hostPort = 0;
        try
        {
            const { stdout: po } = await runDocker(['port', name, '3788/tcp']);
            const m = /:(\d+)\s*$/.exec((po.trim().split('\n')[0] || '').trim());
            if (m) hostPort = parseInt(m[1], 10);
        }
        catch (_) { /* ignore */ }

        let ifMapFile = null;
        try
        {
            const { stdout: ms } = await runDocker(['inspect', '-f', '{{json .Mounts}}', name]);
            const mounts = JSON.parse(ms || '[]');
            const m = mounts.find(x => x && x.Destination === IF_MAP_PATH_IN_CONTAINER);
            if (m) ifMapFile = m.Source;
        }
        catch (_) { /* ignore */ }

        const linkNets = [];
        const stubNets = [];
        try
        {
            const { stdout: ns } = await runDocker(['inspect', '-f', '{{json .NetworkSettings.Networks}}', name]);
            const obj = JSON.parse(ns || '{}');
            for (const k of Object.keys(obj))
            {
                if (k.startsWith('nn-link-'))      linkNets.push(k);
                else if (k.startsWith('nn-stub-')) stubNets.push(k);
            }
        }
        catch (_) { /* ignore */ }

        if (hostPort >= nextHostPort) nextHostPort = hostPort + 1;

        const inst = {
            id,
            image: image || 'unknown',
            containerName: name,
            hostPort,
            status: 'stopped',
            createdAt: Date.now(),
            ifMapFile,
            linkNets,
            stubNets,
            adopted: true
        };
        instances.set(id, inst);
        console.log(`  registered ${name}  image=${inst.image}  status=${inst.status}  port=${inst.hostPort}  linkNets=${linkNets.length}  stubNets=${stubNets.length}`);
    }
}

// -------- WebSocket 终端桥接 --------

const server = http.createServer(app);
const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (req, socket, head) =>
{
    const { pathname, query } = url.parse(req.url, true);
    if (pathname !== '/ws/terminal')
    {
        console.warn(`[ws] reject upgrade: bad path ${pathname}`);
        socket.destroy();
        return;
    }
    // Origin 校验（prod 且配置了 ALLOWED_ORIGINS）
    if (config.IS_PROD && config.ALLOWED_ORIGINS.length > 0)
    {
        const origin = req.headers.origin || '';
        if (origin && !config.ALLOWED_ORIGINS.includes(origin))
        {
            console.warn(`[ws] reject upgrade: origin ${origin} not allowed`);
            socket.destroy();
            return;
        }
    }
    // Token 校验（browser 的 WebSocket 不能加自定义头，走 ?token=）
    if (config.IS_PROD && config.AUTH_TOKEN)
    {
        if (query.token !== config.AUTH_TOKEN)
        {
            console.warn('[ws] reject upgrade: bad token');
            socket.destroy();
            return;
        }
    }
    if (!isValidId(query.id || ''))
    {
        console.warn(`[ws] reject upgrade: invalid id=${query.id}`);
        socket.destroy();
        return;
    }
    const inst = instances.get(query.id);
    if (!inst)
    {
        console.warn(`[ws] reject upgrade: no instance for id=${query.id} (known: ${Array.from(instances.keys()).join(',') || '-'})`);
        socket.destroy();
        return;
    }
    if (inst.status !== 'running')
    {
        console.warn(`[ws] reject upgrade: instance ${query.id} status=${inst.status}`);
        socket.destroy();
        return;
    }
    console.log(`[ws] upgrade id=${query.id} → ${inst.containerName} 127.0.0.1:${inst.hostPort}`);
    wss.handleUpgrade(req, socket, head, (ws) =>
    {
        bridgeTerminal(ws, inst);
    });
});

/** 带重试的 TCP 桥接，因为 netnexus 进程起来后需要一点时间开始 listen 3788 */
function bridgeTerminal(ws, inst)
{
    let alive = true;
    let tcp = null;
    let retries = 0;
    const MAX_RETRIES = 15;
    const RETRY_INTERVAL_MS = 400;

    function tryConnect()
    {
        if (!alive) return;
        tcp = net.createConnection({ host: '127.0.0.1', port: inst.hostPort }, () =>
        {
            console.log(`[ws] TCP connected ${inst.containerName} 127.0.0.1:${inst.hostPort} (retries=${retries})`);
            try { ws.send(`\r\n*** Connected to ${inst.containerName} (${inst.image}) at 127.0.0.1:${inst.hostPort} ***\r\n`); } catch (_) {}
        });

        tcp.on('data', (buf) =>
        {
            if (!alive) return;
            try { ws.send(buf); } catch (_) { /* ignore */ }
        });
        tcp.on('error', (err) =>
        {
            if (!alive) return;
            if (err.code === 'ECONNREFUSED' && retries < MAX_RETRIES)
            {
                retries++;
                console.log(`[ws] TCP ECONNREFUSED ${inst.containerName} 127.0.0.1:${inst.hostPort}, retry ${retries}/${MAX_RETRIES}`);
                setTimeout(tryConnect, RETRY_INTERVAL_MS);
                return;
            }
            console.warn(`[ws] TCP error ${inst.containerName} 127.0.0.1:${inst.hostPort}: ${err.code || ''} ${err.message}`);
            try { ws.send(`\r\n*** TCP error: ${err.message} ***\r\n`); } catch (_) {}
            try { ws.close(); } catch (_) {}
        });
        tcp.on('close', () =>
        {
            if (!alive) return;
            if (retries > 0 && retries < MAX_RETRIES)
            {
                retries++;
                setTimeout(tryConnect, RETRY_INTERVAL_MS);
                return;
            }
            alive = false;
            try { ws.close(); } catch (_) {}
        });
    }

    ws.on('message', (data) =>
    {
        if (!alive || !tcp || tcp.pending) return;
        const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
        try { tcp.write(buf); } catch (_) { /* ignore */ }
    });
    ws.on('close', () =>
    {
        alive = false;
        if (tcp) { try { tcp.destroy(); } catch (_) {} }
    });

    tryConnect();
}

server.listen(PORT, config.BIND_HOST, async () =>
{
    console.log(`[backend] listening on http://${config.BIND_HOST}:${PORT}  env=${config.ENV}`);
    console.log(`[backend] docker bin = ${dockerBin()} ${USE_SUDO ? '(sudo)' : ''}`);
    await registerExistingOnBoot();
    await cleanupOrphanNetworksOnBoot();
});
