/**
 * @file   validation.js
 * @brief  对外 API 入参校验，防命令注入 / 路径穿越。
 * @author NetNexus Team
 * @date   2026-04-19
 *
 * 原则：任何会被拼到 docker 命令、文件路径、容器名里的字段必须先过一道正则。
 */

'use strict';

/** 实例 id / 链路 id：以字母数字开头，只允许字母数字和 `._-`，长度 3~64 */
const ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{2,63}$/;

/** 端口名，当前只支持 GE-N（N=1..99） */
const PORT_RE = /^GE-[1-9]\d?$/;

/** docker 镜像名基本形态：repo[:tag][@sha256:digest] */
const IMAGE_RE = /^[a-z0-9][a-z0-9._\-/]{0,199}(:[A-Za-z0-9._-]{1,128})?(@sha256:[a-f0-9]{64})?$/;

function isValidId(s)    { return typeof s === 'string' && ID_RE.test(s); }
function isValidPort(s)  { return typeof s === 'string' && (s === '' || PORT_RE.test(s)); }

/**
 * 镜像名校验：
 *   1) 基本字符集合法；
 *   2) 若给了白名单，名字必须匹配任意一个白名单前缀。
 */
function isValidImage(s, allowlist)
{
    if (typeof s !== 'string' || s.length === 0 || s.length > 256) return false;
    if (!IMAGE_RE.test(s)) return false;
    if (!allowlist || allowlist.length === 0) return true;
    return allowlist.some(prefix => s.startsWith(prefix));
}

/** base64 字符串解码后大小（字节）。不 decode 实体，省内存 */
function base64DecodedSize(s)
{
    if (typeof s !== 'string' || s.length === 0) return 0;
    const padding = s.endsWith('==') ? 2 : s.endsWith('=') ? 1 : 0;
    return Math.floor((s.length * 3) / 4) - padding;
}

module.exports = {
    ID_RE, PORT_RE, IMAGE_RE,
    isValidId, isValidPort, isValidImage,
    base64DecodedSize
};
