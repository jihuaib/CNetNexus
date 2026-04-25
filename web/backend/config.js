/**
 * @file   config.js
 * @brief  后端统一配置。所有环境相关开关都集中在这里，业务代码只读 config.*
 * @author NetNexus Team
 * @date   2026-04-19
 *
 * 环境：NN_ENV=production|development（也兼容 NODE_ENV）。
 * 生产默认更严：有实例/链路/速率/资源上限，要求 AUTH_TOKEN 或 CORS 白名单二选一。
 */

'use strict';

const ENV = (process.env.NN_ENV || process.env.NODE_ENV || 'development').toLowerCase();
const IS_PROD = ENV === 'production' || ENV === 'prod';

function intEnv(name, fallback)
{
    const v = parseInt(process.env[name], 10);
    return Number.isFinite(v) && v >= 0 ? v : fallback;
}

function listEnv(name, fallback)
{
    const raw = process.env[name];
    if (raw === undefined || raw === '') return fallback;
    return raw.split(',').map(s => s.trim()).filter(Boolean);
}

const config = {
    ENV,
    IS_PROD,

    // --- 监听 ---
    PORT:      intEnv('PORT', 5174),
    // prod 默认只绑 loopback，由前面 nginx 反代；dev 放开便于局域网联调
    BIND_HOST: process.env.BIND_HOST || (IS_PROD ? '127.0.0.1' : '0.0.0.0'),

    // --- 鉴权 ---
    // 设置后，/api/* 需要 X-NN-Token 头（WebSocket 通过 ?token= 查询参数）
    AUTH_TOKEN: process.env.NN_AUTH_TOKEN || '',

    // --- CORS / Origin ---
    // 逗号分隔，例："https://lab.example.com,https://lab.internal"
    ALLOWED_ORIGINS: listEnv('ALLOWED_ORIGINS', []),

    // --- 资源上限（反 DoS） ---
    MAX_INSTANCES: intEnv('MAX_INSTANCES', IS_PROD ? 10   : 1000),
    MAX_LINKS:     intEnv('MAX_LINKS',     IS_PROD ? 30   : 10000),

    // HTTP body 上限（dbBase64 导入会比较大，但不应无限）
    BODY_LIMIT:    process.env.BODY_LIMIT || (IS_PROD ? '8mb' : '64mb'),
    // 单个容器 db 解码后大小上限，防爆内存 / 爆磁盘
    MAX_DB_BYTES:  intEnv('MAX_DB_BYTES',  8 * 1024 * 1024),
    // 单次抓包文件上限，避免一直抓到打满磁盘
    CAPTURE_MAX_BYTES: intEnv('CAPTURE_MAX_BYTES', IS_PROD ? 32 * 1024 * 1024 : 256 * 1024 * 1024),
    // 前端保留的抓包文本行数
    CAPTURE_LINE_LIMIT: intEnv('CAPTURE_LINE_LIMIT', IS_PROD ? 400 : 1000),
    // 当前后端默认只允许一个活动抓包，避免多会话叠加占资源
    MAX_ACTIVE_CAPTURES: intEnv('MAX_ACTIVE_CAPTURES', 1),
    // 用于 host-network 抓 bridge 的辅助镜像，需本地已存在且内含 tcpdump
    CAPTURE_HELPER_IMAGES: listEnv('CAPTURE_HELPER_IMAGES', ['netnexus:latest', 'netnexus:debug', 'netnexus:dev']),

    // --- 镜像白名单（防止拉任意镜像 DoS / 逃逸） ---
    // 前缀匹配。只要镜像名以白名单中任意一项开头即允许。
    IMAGE_ALLOWLIST: listEnv('IMAGE_ALLOWLIST', ['netnexus']),

    // --- 容器资源 cgroups 限制 ---
    CONTAINER_MEMORY: process.env.CONTAINER_MEMORY || (IS_PROD ? '256m' : ''),
    CONTAINER_CPUS:   process.env.CONTAINER_CPUS   || (IS_PROD ? '0.5'  : ''),
    CONTAINER_PIDS:   intEnv('CONTAINER_PIDS',       IS_PROD ? 256      : 0),
    CONTAINER_NOFILE: intEnv('CONTAINER_NOFILE',     IS_PROD ? 1024     : 0),

    // --- Docker 命令超时 ---
    DOCKER_TIMEOUT_MS: intEnv('DOCKER_TIMEOUT_MS', 60000),

    // --- 速率限制（每 IP 滑动窗口） ---
    RATE_LIMIT_WINDOW_MS: intEnv('RATE_LIMIT_WINDOW_MS', 60000),
    RATE_LIMIT_MAX:       intEnv('RATE_LIMIT_MAX', IS_PROD ? 30 : 600)
};

// 生产模式下启动时做门禁：Token 或 Origin 白名单至少有一样，否则认为配置不安全。
// 若部署在 nginx 后仅监听 127.0.0.1，也视为安全（外面访问进不来）。
function validateOrExit()
{
    if (!IS_PROD) return;
    const boundToLoopback = config.BIND_HOST === '127.0.0.1' || config.BIND_HOST === 'localhost';
    const hasToken  = !!config.AUTH_TOKEN;
    const hasOrigin = config.ALLOWED_ORIGINS.length > 0;
    if (!boundToLoopback && !hasToken && !hasOrigin)
    {
        console.error('[config] FATAL: production mode without AUTH_TOKEN / ALLOWED_ORIGINS / loopback bind.');
        console.error('         Set NN_AUTH_TOKEN or ALLOWED_ORIGINS, or keep BIND_HOST=127.0.0.1 behind a trusted reverse proxy.');
        process.exit(1);
    }
}

function dump()
{
    const masked = { ...config, AUTH_TOKEN: config.AUTH_TOKEN ? '***set***' : '(none)' };
    console.log('[config] effective:', JSON.stringify(masked, null, 2));
}

module.exports = { ...config, validateOrExit, dump };
