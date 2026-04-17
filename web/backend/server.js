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

const PORT = parseInt(process.env.PORT || '5174', 10);
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
app.use(cors());
// db 恢复时 dbBase64 可能很大（几百 KB ~ 几 MB），把默认 100KB 上限抬高，
// 否则 Express 会以非 JSON 响应回 413，前端 r.json() 直接抛
// "The string did not match the expected pattern."
app.use(express.json({ limit: '64mb' }));

/** 内存实例表，重启服务即丢失 */
const instances = new Map(); // id -> { id, image, containerName, hostPort, status, createdAt, ifMapFile, linkNets }

/** 内存链路表：每根线对应一个 docker bridge network */
const links = new Map(); // id -> { id, from, to, fromPort, toPort, networkName, wired }

let nextHostPort = 13788;

/** 每台设备固定对外呈现 4 个 GE 口，没用到的 GE 槽位挂一个该设备私有的 stub 网络，
 *  这样容器里一定有 eth1..eth4，if_map 可以稳定写 GE-1=eth1 .. GE-4=eth4。 */
const GE_PORT_COUNT = 4;

function dockerArgs(args) { return USE_SUDO ? ['docker', ...args] : args; }
function dockerBin() { return USE_SUDO ? 'sudo' : DOCKER; }

function runDocker(args, timeoutMs = 60000)
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

// -------- 工具 --------

function sanitizeId(s) { return String(s).replace(/[^a-zA-Z0-9_.-]/g, '_'); }
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
    const { id, image, dbBase64 } = req.body || {};
    if (!id || !image)
    {
        return res.status(400).json({ error: 'id and image required' });
    }

    const containerName = `nn-topo-${id}`;

    // 幂等：如果已经有容器且在跑，直接返回现有实例，不做任何破坏性操作。
    // 这样用户即便误点"启动"也不会丢容器内数据。
    if (instances.has(id))
    {
        const existing = instances.get(id);
        const stillThere = await dockerContainerRunning(existing.containerName);
        if (stillThere)
        {
            return res.json({ instance: existing, reused: true });
        }
        // 容器自己挂了（比如 docker 被 kill），清理掉旧登记，允许重建
        try { await runDocker(['rm', '-f', existing.containerName]); } catch (_) { /* ignore */ }
        instances.delete(id);
    }
    // 没登记但宿主机上已有同名容器（比如前端缓存掉了但后端还知道），尝试继承而不是删掉
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

    const hostPort = nextHostPort++;

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

        // 标记已接通的 link：两端都在运行
        for (const item of netsInOrder)
        {
            if (!item.slot.link) continue;
            const link = item.slot.link;
            const peer = link.from === id ? link.to : link.from;
            if (instances.has(peer)) { link.wired = true; }
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

        // 给 netnexus 一点时间去 listen 3788，再返回
        await sleep(800);

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
    const inst = instances.get(req.params.id);
    if (!inst)
    {
        return res.status(404).json({ error: 'instance not found' });
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

app.delete('/api/instances/:id', async (req, res) =>
{
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

    const runningEnds = [instances.get(from), instances.get(to)].filter(Boolean).length;
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

async function adoptExistingOnBoot()
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
        console.warn('[backend] adopt: list containers failed:', e.stderr || e.message);
        return;
    }

    if (!rows || rows.length === 0)
    {
        console.log('[backend] adopt: no existing nn-topo-* containers');
        return;
    }

    console.log(`[backend] adopting ${rows.length} existing container(s)`);
    for (const line of rows)
    {
        const [name, image] = line.split('\t');
        if (!name) continue;
        const inst = await tryAdoptOne(name, null, image);
        if (inst)
        {
            console.log(`  adopted ${name}  image=${inst.image}  port=${inst.hostPort}  linkNets=${inst.linkNets.length}  stubNets=${inst.stubNets.length}`);
        }
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
        socket.destroy();
        return;
    }
    const inst = instances.get(query.id);
    if (!inst)
    {
        socket.destroy();
        return;
    }
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
                setTimeout(tryConnect, RETRY_INTERVAL_MS);
                return;
            }
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

server.listen(PORT, async () =>
{
    console.log(`[backend] listening on http://0.0.0.0:${PORT}`);
    console.log(`[backend] docker bin = ${dockerBin()} ${USE_SUDO ? '(sudo)' : ''}`);
    await adoptExistingOnBoot();
    await cleanupOrphanNetworksOnBoot();
});
