/**
 * @file   server.js
 * @brief  NetNexus 拓扑编排后端：管理 Docker 容器并桥接浏览器到 telnet 3788
 * @author NetNexus Team
 * @date   2026-04-17
 *
 * 关键设计（与 scripts/ci/top_runner.py 对齐）:
 *   1) 容器以 `sleep infinity` 启动，而不是跑镜像默认 CMD。
 *   2) if_map.conf.gns3 不再 bind mount,直接用 image 自带的固定映射
 *      (GE-1..GE-8=eth1..eth8),保证 dev swap-image 能正常替换该文件。
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
 *   POST   /api/links                 { id, from, fromPort, to, toPort } 新增链路（支持运行时热接线）
 *   DELETE /api/links/:id             删除链路
 *   GET    /api/links/:id/capture     查看某条链路最近一次抓包状态
 *   POST   /api/links/:id/capture/start  启动链路抓包
 *   POST   /api/links/:id/capture/stop   停止链路抓包
 *   GET    /api/captures/:id/download 下载抓包 pcap
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

/** netnexus 在容器里的 sqlite db 位置，用来做"导出/恢复配置" */
const NN_DB_PATH_IN_CONTAINER = '/opt/netnexus/data/netnexus.db';
/** 宿主机临时目录：做 docker cp 中转 */
const DB_TMP_HOST_DIR = path.join(os.tmpdir(), 'nn-topo-db');
try { fs.mkdirSync(DB_TMP_HOST_DIR, { recursive: true }); } catch (_) { /* ignore */ }

/** 宿主机临时目录：保存链路抓包 pcap */
const CAPTURE_TMP_HOST_DIR = path.join(os.tmpdir(), 'nn-topo-captures');
try { fs.mkdirSync(CAPTURE_TMP_HOST_DIR, { recursive: true }); } catch (_) { /* ignore */ }

/** 容器内启动 netnexus 的 bash 片段（与 CI 基本一致） */
const NN_START_SH = [
    'mkdir -p /opt/netnexus/log /opt/netnexus/log/asan /opt/netnexus/data',
    'export NN_WORK_DIR=/opt/netnexus',
    'export LD_LIBRARY_PATH=/opt/netnexus/lib:${LD_LIBRARY_PATH}',
    // 某些 x86 线上环境默认把容器内 IPv6 关掉（disable_ipv6=1），会导致 if_addr_apply 返回 Permission denied。
    // 这里在进程启动前统一尝试打开 all/default/已存在接口的 IPv6 开关。
    'if [ -d /proc/sys/net/ipv6/conf ]; then for f in /proc/sys/net/ipv6/conf/*/disable_ipv6; do [ -f "$f" ] && echo 0 > "$f" 2>/dev/null || true; done; fi',
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
const instances = new Map(); // id -> { id, image, containerName, hostPort, status, createdAt, linkNets }

/** 内存链路表：每根线对应一个 docker bridge network */
const links = new Map(); // id -> { id, from, to, fromPort, toPort, networkName, wired }

/** 抓包会话：保留最近一次链路抓包状态与下载文件 */
const captureSessions = new Map(); // id -> session
const captureByLink = new Map();   // linkId -> captureId

/** 停机时把容器里 /opt/netnexus/data 整个 tar.base64 存这里，下次 start 再灌回。
 *  单独存避免挂在 inst 对象上让 GET /api/instances 列表返回巨大 payload。 */
const stoppedDbs = new Map(); // id -> base64 string

let nextHostPort = 13788;

/** 每台设备固定对外呈现 8 个 GE 口，没用到的 GE 槽位挂一个该设备私有的 stub 网络，
 *  保证容器里一定有 eth1..eth8,与 image 自带的 if_map.conf.gns3
 *  (GE-1..GE-8=eth1..eth8) 对齐。 */
const GE_PORT_COUNT = 8;

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

/** 等宿主机映射端口真正可连；WebSocket 终端走的就是这个路径。 */
async function waitForHostTcpReady(host, port, maxMs = 5000)
{
    if (!port || port <= 0) return false;
    const deadline = Date.now() + maxMs;
    while (Date.now() < deadline)
    {
        const ok = await new Promise(resolve =>
        {
            const sock = net.createConnection({ host, port });
            let done = false;
            function finish(v)
            {
                if (done) return;
                done = true;
                try { sock.destroy(); } catch (_) { /* ignore */ }
                resolve(v);
            }
            sock.setTimeout(1000);
            sock.on('connect', () => finish(true));
            sock.on('timeout', () => finish(false));
            sock.on('error', () => finish(false));
        });
        if (ok) return true;
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

function captureHelperContainerName(sessionId, role)
{
    return `nn-cap-${role}-${sanitizeId(sessionId).slice(0, 40)}`;
}

function captureFileName(linkId, startedAt)
{
    const ts = new Date(startedAt).toISOString().replace(/[:.]/g, '-');
    return `netnexus-capture-${sanitizeId(linkId)}-${ts}.pcap`;
}

function dockerSpawn(args)
{
    return spawn(dockerBin(), dockerArgs(args), { stdio: ['ignore', 'pipe', 'pipe'] });
}

async function dockerImageExists(name)
{
    try
    {
        await runDocker(['image', 'inspect', name]);
        return true;
    }
    catch (_) { return false; }
}

async function dockerImageHasTcpdump(name)
{
    try
    {
        await runDocker(['run', '--rm', '--entrypoint', '/bin/sh', name, '-lc', 'command -v tcpdump >/dev/null 2>&1']);
        return true;
    }
    catch (_) { return false; }
}

function getActiveCaptureSessions()
{
    return Array.from(captureSessions.values()).filter(s => s.status === 'running' || s.status === 'starting' || s.status === 'stopping');
}

function appendCaptureLine(session, line)
{
    const text = String(line || '').replace(/\r/g, '').trimEnd();
    if (!text) return;
    session.lines.push(text);
    if (session.lines.length > config.CAPTURE_LINE_LIMIT)
    {
        session.lines.splice(0, session.lines.length - config.CAPTURE_LINE_LIMIT);
    }
}

function pushCaptureText(session, key, chunk, prefix = '')
{
    const incoming = Buffer.isBuffer(chunk) ? chunk.toString('utf8') : String(chunk || '');
    session.textBuffers[key] = (session.textBuffers[key] || '') + incoming;
    const parts = session.textBuffers[key].split('\n');
    session.textBuffers[key] = parts.pop() || '';
    for (const line of parts)
    {
        appendCaptureLine(session, `${prefix}${line}`);
    }
}

function flushCaptureText(session, key, prefix = '')
{
    if (!session.textBuffers[key]) return;
    appendCaptureLine(session, `${prefix}${session.textBuffers[key]}`);
    session.textBuffers[key] = '';
}

function serializeCapture(session, { includeLines = true } = {})
{
    if (!session) return null;
    const payload = {
        id: session.id,
        linkId: session.linkId,
        status: session.status,
        startedAt: session.startedAt,
        stoppedAt: session.stoppedAt || null,
        stopReason: session.stopReason || null,
        error: session.error || null,
        bytes: session.bytes || 0,
        bridgeName: session.bridgeName,
        networkName: session.networkName,
        helperImage: session.helperImage,
        downloadName: session.downloadName,
        downloadUrl: (session.bytes || 0) > 0 ? `/api/captures/${encodeURIComponent(session.id)}/download` : null
    };
    if (includeLines) payload.lines = [...session.lines];
    return payload;
}

async function resolveCaptureHelperImage()
{
    for (const image of config.CAPTURE_HELPER_IMAGES)
    {
        if (!(await dockerImageExists(image))) continue;
        if (await dockerImageHasTcpdump(image)) return image;
    }
    return null;
}

async function inspectLinkBridge(link)
{
    const networkName = link.networkName || linkNetworkName(link.id);
    if (!(await dockerNetworkExists(networkName)))
    {
        throw new Error(`链路 ${link.id} 尚未接通，docker 网络 ${networkName} 不存在`);
    }
    const { stdout } = await runDocker(['network', 'inspect', networkName, '--format', '{{json .}}']);
    const info = JSON.parse(stdout.trim() || '{}');
    const networkId = String(info.Id || '').trim();
    if (!networkId)
    {
        throw new Error(`无法读取链路 ${networkName} 的网络 ID`);
    }
    const driver = String(info.Driver || '').trim();
    if (driver && driver !== 'bridge')
    {
        throw new Error(`当前仅支持 bridge 网络抓包，实际为 ${driver}`);
    }
    const bridgeName = info.Options?.['com.docker.network.bridge.name'] || `br-${networkId.slice(0, 12)}`;
    return { networkName, networkId, bridgeName };
}

function maybeFinalizeCapture(session)
{
    if (!session || session.finalized) return;
    if (!session.recorderClosed || !session.viewerClosed) return;

    session.finalized = true;
    flushCaptureText(session, 'viewer');
    flushCaptureText(session, 'pcap', '[pcap] ');
    try { session.fileStream?.end(); } catch (_) { /* ignore */ }
    session.fileStream = null;
    session.stoppedAt = session.stoppedAt || Date.now();

    if (session.finalStatus)
    {
        session.status = session.finalStatus;
    }
    else if (session.stopRequested)
    {
        session.status = 'stopped';
    }
    else if (session.error)
    {
        session.status = 'error';
    }
    else
    {
        session.status = 'stopped';
    }
}

async function stopCaptureSession(session, reason = '用户停止抓包', finalStatus = 'stopped')
{
    if (!session) return null;
    if (session.stopPromise) return session.stopPromise;

    session.stopRequested = true;
    session.stopReason = session.stopReason || reason;
    session.finalStatus = session.finalStatus || finalStatus;
    if (session.status === 'running' || session.status === 'starting') session.status = 'stopping';
    appendCaptureLine(session, `[capture] ${reason}`);

    session.stopPromise = (async () =>
    {
        const names = [session.viewerName, session.recorderName].filter(Boolean);
        for (const name of names)
        {
            await runDocker(['rm', '-f', name]).catch(() => {});
        }
        try { session.viewerProc?.kill('SIGTERM'); } catch (_) { /* ignore */ }
        try { session.recorderProc?.kill('SIGTERM'); } catch (_) { /* ignore */ }

        const deadline = Date.now() + 5000;
        while ((!session.recorderClosed || !session.viewerClosed) && Date.now() < deadline)
        {
            await sleep(100);
        }
        if (!session.recorderClosed) session.recorderClosed = true;
        if (!session.viewerClosed) session.viewerClosed = true;

        maybeFinalizeCapture(session);
        return session;
    })();

    return session.stopPromise;
}

async function stopCaptureByLink(linkId, reason)
{
    const captureId = captureByLink.get(linkId);
    if (!captureId) return null;
    const session = captureSessions.get(captureId);
    if (!session || (session.status !== 'running' && session.status !== 'starting' && session.status !== 'stopping'))
    {
        return session || null;
    }
    return stopCaptureSession(session, reason);
}

async function stopCapturesForInstance(instanceId, reason)
{
    for (const link of links.values())
    {
        if (link.from === instanceId || link.to === instanceId)
        {
            await stopCaptureByLink(link.id, reason);
        }
    }
}

async function startCaptureForLink(link)
{
    const existingId = captureByLink.get(link.id);
    if (existingId)
    {
        const existing = captureSessions.get(existingId);
        if (existing && (existing.status === 'running' || existing.status === 'starting' || existing.status === 'stopping'))
        {
            return existing;
        }
    }

    if (getActiveCaptureSessions().length >= config.MAX_ACTIVE_CAPTURES)
    {
        const active = getActiveCaptureSessions()[0];
        const err = new Error(`当前仅允许 ${config.MAX_ACTIVE_CAPTURES} 个活动抓包，请先停止 ${active?.linkId || '现有会话'}`);
        err.statusCode = 409;
        err.activeCapture = active ? serializeCapture(active, { includeLines: false }) : null;
        throw err;
    }

    const helperImage = await resolveCaptureHelperImage();
    if (!helperImage)
    {
        throw new Error(`抓包需要重建本地 netnexus 镜像并带入 tcpdump，例如重新构建 netnexus:latest；已检查 ${config.CAPTURE_HELPER_IMAGES.join(' / ')}`);
    }

    const bridge = await inspectLinkBridge(link);
    const startedAt = Date.now();
    const sessionId = `cap-${startedAt.toString(36)}${Math.random().toString(36).slice(2, 6)}`;
    const pcapPath = path.join(CAPTURE_TMP_HOST_DIR, `${sessionId}.pcap`);
    const session = {
        id: sessionId,
        linkId: link.id,
        networkName: bridge.networkName,
        bridgeName: bridge.bridgeName,
        helperImage,
        status: 'starting',
        startedAt,
        stoppedAt: null,
        stopReason: null,
        error: null,
        bytes: 0,
        lines: [],
        textBuffers: { viewer: '', pcap: '' },
        downloadName: captureFileName(link.id, startedAt),
        pcapPath,
        recorderName: captureHelperContainerName(sessionId, 'rec'),
        viewerName: captureHelperContainerName(sessionId, 'tap'),
        recorderClosed: false,
        viewerClosed: false,
        finalized: false,
        stopRequested: false,
        finalStatus: null,
        stopPromise: null,
        fileStream: fs.createWriteStream(pcapPath)
    };

    captureSessions.set(session.id, session);
    captureByLink.set(link.id, session.id);

    session.fileStream.on('error', err =>
    {
        session.error = `抓包文件写入失败: ${err.message}`;
        session.finalStatus = 'error';
        stopCaptureSession(session, session.error, 'error').catch(() => {});
    });

    appendCaptureLine(
        session,
        `[capture] ${link.id} 开始抓包，bridge=${bridge.bridgeName} helper=${helperImage}，文件上限 ${config.CAPTURE_MAX_BYTES} 字节`
    );

    const commonArgs = ['run', '--rm', '--name', '', '--network', 'host', '--cap-add', 'NET_ADMIN', '--cap-add', 'NET_RAW', helperImage, 'tcpdump'];
    const recorderArgs = [...commonArgs];
    recorderArgs[3] = session.recorderName;
    recorderArgs.push('-i', bridge.bridgeName, '-s', '0', '-U', '-nn', '-w', '-');

    const viewerArgs = [...commonArgs];
    viewerArgs[3] = session.viewerName;
    viewerArgs.push('-i', bridge.bridgeName, '-s', '0', '-l', '-nn', '-tttt', '-e', '-vv');

    session.recorderProc = dockerSpawn(recorderArgs);
    session.viewerProc = dockerSpawn(viewerArgs);

    session.recorderProc.stdout.on('data', chunk =>
    {
        session.bytes += chunk.length;
        session.fileStream.write(chunk);
        if (!session.stopRequested && session.bytes >= config.CAPTURE_MAX_BYTES)
        {
            stopCaptureSession(session, `抓包达到上限 ${config.CAPTURE_MAX_BYTES} 字节，已自动停止`).catch(() => {});
        }
    });
    session.recorderProc.stderr.on('data', chunk => pushCaptureText(session, 'pcap', chunk, '[pcap] '));
    session.viewerProc.stdout.on('data', chunk => pushCaptureText(session, 'viewer', chunk));
    session.viewerProc.stderr.on('data', chunk => pushCaptureText(session, 'viewer', chunk, '[tcpdump] '));

    session.recorderProc.on('error', err =>
    {
        session.error = `抓包写文件进程启动失败: ${err.message}`;
        session.finalStatus = 'error';
        stopCaptureSession(session, session.error, 'error').catch(() => {});
    });
    session.viewerProc.on('error', err =>
    {
        session.error = `抓包文本进程启动失败: ${err.message}`;
        session.finalStatus = 'error';
        stopCaptureSession(session, session.error, 'error').catch(() => {});
    });

    session.recorderProc.on('close', (code, signal) =>
    {
        session.recorderClosed = true;
        if (!session.stopRequested && code !== 0)
        {
            session.error = `抓包写文件进程异常退出(code=${code ?? 'null'}, signal=${signal || '-'})`;
            session.finalStatus = 'error';
            stopCaptureSession(session, session.error, 'error').catch(() => {});
        }
        maybeFinalizeCapture(session);
    });
    session.viewerProc.on('close', (code, signal) =>
    {
        session.viewerClosed = true;
        flushCaptureText(session, 'viewer');
        if (!session.stopRequested && code !== 0)
        {
            session.error = `抓包文本进程异常退出(code=${code ?? 'null'}, signal=${signal || '-'})`;
            session.finalStatus = 'error';
            stopCaptureSession(session, session.error, 'error').catch(() => {});
        }
        maybeFinalizeCapture(session);
    });

    await sleep(350);
    if (!session.error && !session.stopRequested)
    {
        session.status = 'running';
    }
    maybeFinalizeCapture(session);
    return session;
}

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
 * 为实例规划 8 个 GE 口的 docker 网络：
 *   - 用到的 GE-N 指向 link 对应的 docker 网络
 *   - 未用到的 GE-N 指向一个仅此设备可见的 stub 网络
 * 返回数组长度固定 GE_PORT_COUNT，按 GE-1..GE-8 顺序。
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
const SUBNET_ALLOC_MAX_RETRIES = 64;

function ipv4ToU32(ip)
{
    const parts = ip.split('.').map(x => Number(x));
    if (parts.length !== 4 || parts.some(x => !Number.isInteger(x) || x < 0 || x > 255)) return null;
    return (((parts[0] << 24) >>> 0)
        | ((parts[1] << 16) >>> 0)
        | ((parts[2] << 8) >>> 0)
        | (parts[3] >>> 0)) >>> 0;
}

function parseIpv4CidrRange(cidr)
{
    const [ip, prefixText] = String(cidr || '').trim().split('/');
    if (!ip || !prefixText) return null;
    if (ip.includes(':')) return null;
    const prefix = Number(prefixText);
    if (!Number.isInteger(prefix) || prefix < 0 || prefix > 32) return null;
    const ipU32 = ipv4ToU32(ip);
    if (ipU32 === null) return null;
    const mask = prefix === 0 ? 0 : (0xffffffff << (32 - prefix)) >>> 0;
    const start = (ipU32 & mask) >>> 0;
    const end = (start | (~mask >>> 0)) >>> 0;
    return { cidr: `${ip}/${prefix}`, start, end };
}

function cidrRangesOverlap(a, b)
{
    return a.start <= b.end && b.start <= a.end;
}

function isSubnetOverlapErr(detail)
{
    return /Pool overlaps with other one on this address space/i.test(detail || '');
}

async function listUsedSubnets()
{
    const used = [];
    let stdout;
    try
    {
        ({ stdout } = await runDocker(['network', 'ls', '--format', '{{.Name}}']));
    }
    catch (_) { return used; }
    const names = stdout.split('\n').map(s => s.trim()).filter(Boolean);
    for (const n of names)
    {
        try
        {
            const { stdout: sub } = await runDocker(['inspect', '-f', '{{range .IPAM.Config}}{{println .Subnet}}{{end}}', n]);
            const cidrs = sub.split(/\s+/).map(s => s.trim()).filter(Boolean);
            for (const cidr of cidrs)
            {
                const r = parseIpv4CidrRange(cidr);
                if (r) used.push(r);
            }
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
        const r = parseIpv4CidrRange(candidate);
        if (!r) continue;
        const overlapped = used.some(x => cidrRangesOverlap(x, r));
        if (!overlapped) return candidate;
    }
    throw new Error('no free /29 subnet in 10.200.0.0/16 pool');
}

async function createBridgeNetwork(name)
{
    let lastOverlap = null;
    for (let attempt = 1; attempt <= SUBNET_ALLOC_MAX_RETRIES; attempt++)
    {
        const subnet = await pickFreeSubnet29();
        try
        {
            // 与 CI topology runtime 对齐：链路/占位网络都开启 IPv6，避免双栈恢复时地址下发失败
            await runDocker(['network', 'create', '--ipv6', '--driver', 'bridge', '--internal', '--subnet', subnet, name]);
            return;
        }
        catch (e)
        {
            const detail = String(e.stderr || e.message || '').trim();
            if (!isSubnetOverlapErr(detail)) throw e;
            lastOverlap = detail || 'overlap';
            console.warn(
                `[backend] create network ${name} subnet ${subnet} overlap, retry ${attempt}/${SUBNET_ALLOC_MAX_RETRIES}`
            );
        }
    }
    throw new Error(
        `failed to allocate subnet for ${name}: overlap retries exhausted (${SUBNET_ALLOC_MAX_RETRIES}), last=${lastOverlap || 'unknown'}`
    );
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
 *   1) 收集 links,规划 GE 槽位
 *   2) docker run --cap-add NET_ADMIN/NET_RAW -p host:3788 image sleep infinity
 *      (if_map.conf.gns3 由 image 自带,不再 bind mount)
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
                const ready = await waitForNetnexusReady(existing.containerName, 15000);
                const hostReady = ready && await waitForHostTcpReady('127.0.0.1', existing.hostPort, 5000);
                if (!ready || !hostReady)
                {
                    const tail = await tailNetnexusLog(existing.containerName);
                    console.warn(`[backend] ${existing.containerName} terminal not ready (container=${ready}, host=${hostReady}), log tail:\n${tail}`);
                    return res.status(500).json({ error: 'netnexus terminal did not become ready', detail: tail || 'no log' });
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

    try
    {
        // 1) 规划 4 个 GE 口：有 link 的用 link 网络，没 link 的用 stub 网络
        //    if_map.conf.gns3 由 image 自带的 GE-1..GE-8=eth1..eth8 提供,不再 bind mount
        //    (避免阻碍 dev swap-image 替换该文件)
        const slots = planInterfacesOf(id);

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

        // 3) 以 sleep infinity 拉起容器
        //    eth0 是 docker 默认 bridge，承载 -p 端口映射和 CLI 3788；eth1..eth8 后面 connect
        await runDocker([
            'run', '-d',
            '--name', containerName,
            '--hostname', id,
            '--cap-add', 'NET_ADMIN',
            '--cap-add', 'NET_RAW',
            '--cap-add', 'SYS_PTRACE',
            '--sysctl', 'net.ipv6.conf.all.disable_ipv6=0',
            '--sysctl', 'net.ipv6.conf.default.disable_ipv6=0',
            '--security-opt', 'seccomp=unconfined',
            ...containerHardeningArgs(),
            '-e', 'NN_WORK_DIR=/opt/netnexus',
            '-e', 'LD_LIBRARY_PATH=/opt/netnexus/lib',
            '-v', '/var/run/docker.sock:/var/run/docker.sock',
            '-p', `${hostPort}:3788`,
            image,
            'sleep', 'infinity'
        ]);

        // 4) 严格按 GE-1..GE-8 顺序 attach，确保 ethN 命名对齐
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
            status: 'starting',
            createdAt: Date.now(),
            linkNets: netsInOrder.map(x => x.net),
            stubNets: netsInOrder.filter(x => !x.slot.link).map(x => x.net)
        };
        instances.set(id, inst);

        // 等 netnexus 真的 listen 3788，且宿主机映射端口可连后再返回。
        // 页面终端走 127.0.0.1:hostPort；只检查容器内监听会让 UI 显示 running 但终端连不上。
        const t0 = Date.now();
        const ready = await waitForNetnexusReady(containerName, 20000);
        const hostReady = ready && await waitForHostTcpReady('127.0.0.1', hostPort, 5000);
        const t1 = Date.now();
        if (ready && hostReady)
        {
            inst.status = 'running';
            console.log(`[backend] ${containerName} netnexus terminal ready in ${t1 - t0}ms`);
        }
        else
        {
            const tail = await tailNetnexusLog(containerName);
            const err = new Error(`netnexus terminal did not become ready (container=${ready}, host=${hostReady})`);
            err.stderr = tail || err.message;
            console.warn(`[backend] ${containerName} ${err.message} after ${t1 - t0}ms, log tail:\n${tail}`);
            throw err;
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
        // 失败清理容器
        await runDocker(['rm', '-f', containerName]).catch(() => {});
        instances.delete(id);
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
 * 会产生 eth9+，打破 GE-N 的 1..8 固定布局，ping 不通）。
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
        await stopCapturesForInstance(inst.id, `设备 ${inst.id} 停止，抓包同步结束`);

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

        // 6) 保留 inst 记录（id/image/hostPort 给 start 复用），db 单独存
        if (savedDb) stoppedDbs.set(inst.id, savedDb);
        else stoppedDbs.delete(inst.id);
        inst.status = 'stopped';
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
        await stopCapturesForInstance(req.params.id, `设备 ${req.params.id} 删除，抓包同步结束`);

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
        for (const session of getActiveCaptureSessions())
        {
            await stopCaptureSession(session, '执行一键清理，抓包同步结束');
        }

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
 * 新增（或登记）一条链路。
 *   - 两端都未启动：仅登记，等设备启动时按 GE 槽位自动接通。
 *   - 一端/两端已启动：对运行中的端点做 "disconnect stub -> connect link" 原位换接口，
 *     保持容器内 GE-N <-> ethN 对齐，实现热加线。
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

    const runningEndpoints = [
        { id: link.from, port: link.fromPort },
        { id: link.to, port: link.toPort }
    ].filter(ep => {
        const inst = instances.get(ep.id);
        return inst && inst.status === 'running';
    });

    let hotOkEnds = 0;
    let hotFailedEnds = 0;
    if (runningEndpoints.length > 0)
    {
        try
        {
            await ensureLinkNetwork(link);
        }
        catch (e)
        {
            hotFailedEnds = runningEndpoints.length;
            console.warn(`[backend] POST link ${id} ensure network failed: ${e.stderr || e.message}`);
        }
    }

    if (runningEndpoints.length > 0 && hotFailedEnds === 0)
    {
        for (const ep of runningEndpoints)
        {
            const inst = instances.get(ep.id);
            const geIdx = parsePortIndex(ep.port);
            if (!inst || geIdx < 1 || geIdx > GE_PORT_COUNT)
            {
                hotFailedEnds++;
                continue;
            }
            const stubName = stubNetworkName(inst.id, geIdx);
            try
            {
                if (await containerOnNetwork(inst.containerName, stubName))
                {
                    await runDocker(['network', 'disconnect', stubName, inst.containerName]).catch(() => {});
                }
                await runDocker(['network', 'rm', stubName]).catch(() => {});

                if (!(await containerOnNetwork(inst.containerName, link.networkName)))
                {
                    await runDocker(['network', 'connect', link.networkName, inst.containerName]);
                }

                inst.stubNets = (inst.stubNets || []).filter(n => n !== stubName);
                if (!(inst.linkNets || []).includes(link.networkName))
                {
                    inst.linkNets = inst.linkNets || [];
                    inst.linkNets.push(link.networkName);
                }
                hotOkEnds++;
            }
            catch (e)
            {
                hotFailedEnds++;
                console.warn(
                    `[backend] POST link ${id} hot-swap failed on ${inst.containerName} ${ep.port}: ${e.stderr || e.message}`
                );
            }
        }
    }

    const runningEnds = runningEndpoints.length;
    link.wired = (runningEnds === 2 && hotFailedEnds === 0 && hotOkEnds === 2);

    let note;
    if (runningEnds === 0)
    {
        note = `已登记，启动两端设备时会自动接通（${link.networkName}）`;
    }
    else if (hotFailedEnds > 0)
    {
        note = `链路已登记，热接线部分失败（成功 ${hotOkEnds}/${runningEnds} 端），建议重启失败端设备`;
    }
    else if (runningEnds === 2)
    {
        note = `链路已热接通（${link.networkName}）`;
    }
    else
    {
        note = `链路已热接入 1 端（${link.networkName}），另一端启动后会自动接通`;
    }

    res.json({
        ok: true,
        link,
        wired: link.wired,
        hotApplied: hotOkEnds > 0,
        needRestart: hotFailedEnds > 0,
        runningEnds,
        note
    });
});

app.get('/api/links', (_req, res) =>
{
    res.json({ links: Array.from(links.values()) });
});

app.get('/api/links/:id/capture', (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    if (!links.has(req.params.id))
    {
        return res.status(404).json({ error: 'link not found' });
    }
    const captureId = captureByLink.get(req.params.id);
    const session = captureId ? captureSessions.get(captureId) : null;
    res.json({ capture: serializeCapture(session) });
});

app.post('/api/links/:id/capture/start', async (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    const link = links.get(req.params.id);
    if (!link)
    {
        return res.status(404).json({ error: 'link not found' });
    }
    try
    {
        const priorId = captureByLink.get(link.id);
        const prior = priorId ? captureSessions.get(priorId) : null;
        const reused = !!(prior && (prior.status === 'running' || prior.status === 'starting' || prior.status === 'stopping'));
        const session = await startCaptureForLink(link);
        if (session.status === 'error')
        {
            return res.status(500).json({
                error: 'capture start failed',
                detail: session.error || 'unknown capture error',
                capture: serializeCapture(session)
            });
        }
        res.json({ ok: true, capture: serializeCapture(session), reused });
    }
    catch (e)
    {
        const status = e.statusCode || 500;
        res.status(status).json({
            error: 'capture start failed',
            detail: String(e.message || e),
            activeCapture: e.activeCapture || null
        });
    }
});

app.post('/api/links/:id/capture/stop', async (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    if (!links.has(req.params.id))
    {
        return res.status(404).json({ error: 'link not found' });
    }
    const captureId = captureByLink.get(req.params.id);
    const session = captureId ? captureSessions.get(captureId) : null;
    if (!session)
    {
        return res.status(404).json({ error: 'capture not found' });
    }
    await stopCaptureSession(session, '用户手动停止抓包');
    res.json({ ok: true, capture: serializeCapture(session) });
});

app.get('/api/captures/:id/download', (req, res) =>
{
    if (!isValidId(req.params.id)) return res.status(400).json({ error: 'invalid id' });
    const session = captureSessions.get(req.params.id);
    if (!session)
    {
        return res.status(404).json({ error: 'capture not found' });
    }
    if (!session.pcapPath || !fs.existsSync(session.pcapPath))
    {
        return res.status(404).json({ error: 'capture file not found' });
    }
    res.download(session.pcapPath, session.downloadName);
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

    await stopCaptureByLink(link.id, `链路 ${link.id} 删除，抓包同步结束`);

    // 对每个运行中的端点做 "disconnect link → connect stub" 原位换接口，
    // 让容器内 ethN 槽位继续存在，避免后续加线被 docker 分到 eth5+。
    // 结果：GE-N 保持 UP（stub 提供 carrier），直连路由仍在，但对端不可达。
    const runningEndpoints = [
        { id: link.from, port: link.fromPort },
        { id: link.to, port: link.toPort }
    ].filter(ep => {
        const inst = instances.get(ep.id);
        return inst && inst.status === 'running';
    });

    try
    {
        for (const ep of runningEndpoints)
        {
            const inst = instances.get(ep.id);
            const geIdx = parsePortIndex(ep.port);
            if (geIdx < 1 || geIdx > GE_PORT_COUNT) continue;

            if (await containerOnNetwork(inst.containerName, link.networkName))
            {
                await runDocker(['network', 'disconnect', link.networkName, inst.containerName]).catch(() => {});
            }
            const stubName = await ensureStubNetwork(inst.id, geIdx);
            if (!(await containerOnNetwork(inst.containerName, stubName)))
            {
                await runDocker(['network', 'connect', stubName, inst.containerName]);
            }
            inst.linkNets = (inst.linkNets || []).filter(n => n !== link.networkName);
            if (!(inst.stubNets || []).includes(stubName))
            {
                inst.stubNets = inst.stubNets || [];
                inst.stubNets.push(stubName);
            }
        }
    }
    catch (e)
    {
        console.warn(`[backend] DELETE link ${req.params.id} hot-swap failed: ${e.stderr || e.message}`);
        // 兜底：继续走 unwireLink 把 link 网络彻底清掉，保证一致性
    }

    // 此时 link 网络上已没有 running 容器；unwireLink 会清干净残余并 rm 网络
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

async function cleanupCaptureHelpersOnBoot()
{
    let names;
    try
    {
        const { stdout } = await runDocker(['ps', '-a', '--filter', 'name=nn-cap-', '--format', '{{.Names}}']);
        names = stdout.split('\n').map(s => s.trim()).filter(Boolean);
    }
    catch (_) { return; }

    let removed = 0;
    for (const name of names)
    {
        await runDocker(['rm', '-f', name]).catch(() => {});
        removed++;
    }
    if (removed > 0)
    {
        console.log(`[backend] removed ${removed} stale capture helper container(s)`);
    }
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
    let retryTimer = null;
    let retries = 0;
    let connected = false;
    const MAX_RETRIES = 60;
    const RETRY_INTERVAL_MS = 500;

    function sendSafe(data)
    {
        try { ws.send(data); } catch (_) { /* ignore */ }
    }

    function closeWs()
    {
        alive = false;
        if (retryTimer)
        {
            clearTimeout(retryTimer);
            retryTimer = null;
        }
        if (tcp)
        {
            try { tcp.destroy(); } catch (_) { /* ignore */ }
            tcp = null;
        }
        try { ws.close(); } catch (_) { /* ignore */ }
    }

    function scheduleRetry(reason)
    {
        if (!alive || connected || retryTimer) return;
        if (retries >= MAX_RETRIES)
        {
            console.warn(`[ws] TCP not ready ${inst.containerName} 127.0.0.1:${inst.hostPort}: ${reason}; retries exhausted`);
            sendSafe(`\r\n*** TCP error: device terminal not ready after ${(MAX_RETRIES * RETRY_INTERVAL_MS / 1000).toFixed(0)}s ***\r\n`);
            closeWs();
            return;
        }
        retries++;
        if (retries === 1)
        {
            sendSafe('\r\n*** Waiting for device terminal ... ***\r\n');
        }
        console.log(`[ws] TCP not ready ${inst.containerName} 127.0.0.1:${inst.hostPort}: ${reason}, retry ${retries}/${MAX_RETRIES}`);
        retryTimer = setTimeout(() =>
        {
            retryTimer = null;
            tryConnect();
        }, RETRY_INTERVAL_MS);
    }

    function tryConnect()
    {
        if (!alive || connected) return;
        if (tcp)
        {
            try { tcp.destroy(); } catch (_) { /* ignore */ }
            tcp = null;
        }

        const sock = net.createConnection({ host: '127.0.0.1', port: inst.hostPort }, () =>
        {
            connected = true;
            console.log(`[ws] TCP connected ${inst.containerName} 127.0.0.1:${inst.hostPort} (retries=${retries})`);
            sendSafe(`\r\n*** Connected to ${inst.containerName} (${inst.image}) at 127.0.0.1:${inst.hostPort} ***\r\n`);
        });
        tcp = sock;
        let retryHandled = false;

        sock.on('data', (buf) =>
        {
            if (!alive) return;
            sendSafe(buf);
        });
        sock.on('error', (err) =>
        {
            if (!alive) return;
            if (!connected && ['ECONNREFUSED', 'ECONNRESET', 'ETIMEDOUT', 'EHOSTUNREACH'].includes(err.code))
            {
                retryHandled = true;
                scheduleRetry(err.code || err.message);
                return;
            }
            console.warn(`[ws] TCP error ${inst.containerName} 127.0.0.1:${inst.hostPort}: ${err.code || ''} ${err.message}`);
            sendSafe(`\r\n*** TCP error: ${err.message} ***\r\n`);
            closeWs();
        });
        sock.on('close', () =>
        {
            if (!alive) return;
            if (tcp === sock) tcp = null;
            if (!connected)
            {
                if (!retryHandled) scheduleRetry('closed before connect');
                return;
            }
            closeWs();
        });
    }

    ws.on('message', (data) =>
    {
        if (!alive || !tcp || !connected) return;
        const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
        try { tcp.write(buf); } catch (_) { /* ignore */ }
    });
    ws.on('close', () =>
    {
        closeWs();
    });

    tryConnect();
}

server.listen(PORT, config.BIND_HOST, async () =>
{
    console.log(`[backend] listening on http://${config.BIND_HOST}:${PORT}  env=${config.ENV}`);
    console.log(`[backend] docker bin = ${dockerBin()} ${USE_SUDO ? '(sudo)' : ''}`);
    await cleanupCaptureHelpersOnBoot();
    await registerExistingOnBoot();
    await cleanupOrphanNetworksOnBoot();
});
