# NetNexus 拓扑后端部署指南（HTTP 版）

本文档描述把 `web/backend` 部署到一台 **x86_64 Linux** 服务器，前端已经放在
`/var/www/netnexus/`，站点域名 `www.netnexus.com.cn`，使用纯 HTTP（80 端口）。

> 开发机可以是 ARM64（M 系列 Mac、aarch64 Linux）。`package.sh` 打包产物不含
> `node_modules`，跨架构部署不受影响，见 §2。

---

## 0. 目标拓扑

```
浏览器 → http://www.netnexus.com.cn
          ├─ 静态前端：/var/www/netnexus/             （nginx 直接送）
          ├─ /api/*  → 127.0.0.1:5174  （nginx 注 X-NN-Token）
          └─ /ws/*   → 127.0.0.1:5174  （nginx 追加 ?token=）
                       │
                       └─ node server.js
                            └─ docker 守护进程：nn-topo-* 容器、nn-link-* / nn-stub-* 网络
```

后端**只绑 `127.0.0.1`**，外网流量全部经 nginx；token 存在 nginx 里，浏览器永远看不到。

---

## 1. 目标机前置依赖

### 1.1 Docker

```bash
curl -fsSL https://get.docker.com | sh
sudo systemctl enable --now docker
docker --version
```

### 1.2 Node.js 20 LTS

**Ubuntu / Debian：**

```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
node -v   # v20.x
npm  -v
```

**CentOS / RHEL / Rocky / Alma：**

```bash
curl -fsSL https://rpm.nodesource.com/setup_20.x | sudo -E bash -
sudo yum install -y nodejs
```

**离线机**（开发机下好 tarball 再拷过去）：

```bash
# 开发机
wget https://nodejs.org/dist/v20.19.0/node-v20.19.0-linux-x64.tar.xz
scp node-v20.19.0-linux-x64.tar.xz user@target:/tmp/

# 目标机
sudo tar -C /opt -xJf /tmp/node-v20.19.0-linux-x64.tar.xz
sudo mv /opt/node-v20.19.0-linux-x64 /opt/node
sudo ln -sf /opt/node/bin/node /usr/local/bin/node
sudo ln -sf /opt/node/bin/npm  /usr/local/bin/npm
```

> 服务器是 x86_64 就装 x86_64 的 node，不要从 ARM64 本机拷过去。

### 1.3 其它工具

```bash
sudo apt install -y rsync openssl nginx        # Debian 系
# 或 sudo yum install -y rsync openssl nginx   # RHEL 系
```

---

## 2. 在开发机上打包

```bash
cd /path/to/CNetNexus/web/backend
./package.sh
# 或 npm run package
```

产物：`web/backend/dist/netnexus-backend-<date>-<sha>.tar.gz`（约 30 KB）。

包含的文件：`server.js / config.js / validation.js / package.json /
package-lock.json / DEPLOY.md / install.sh`。**不含 `node_modules`**，
所以在 ARM64 开发机打包、上传到 x86_64 服务器没有任何问题 —— 依赖由目标机自己装。

---

## 3. 上传并安装

```bash
# 开发机
scp dist/netnexus-backend-*.tar.gz user@www.netnexus.com.cn:/tmp/

# 目标机
cd /tmp
tar xzf netnexus-backend-*.tar.gz
cd netnexus-backend-*/
sudo ./install.sh
```

`install.sh` 会依次：

1. 创建系统用户 `nn-backend` 并加入 `docker` 组；
2. `rsync` 代码到 `/opt/nn-backend/app/`；
3. 以 `nn-backend` 身份跑 `npm ci --omit=dev`（需要目标机能访问 npm registry）；
4. 若 `/etc/nn-backend.env` 不存在就生成默认配置，**token 自动 `openssl rand -hex 32`**；
5. 写 `/etc/systemd/system/nn-backend.service` 并 `systemctl enable --now`；
6. 打印 systemd 状态。

安装完看到 `[backend] listening on http://127.0.0.1:5174  env=production` 即成功。

### 3.1 npm 镜像加速（可选）

如果目标机连 `registry.npmjs.org` 慢：

```bash
sudo -u nn-backend npm config set registry https://registry.npmmirror.com
# 然后重新 npm ci
cd /opt/nn-backend/app
sudo -u nn-backend npm ci --omit=dev
```

---

## 4. 查看并修改 `/etc/nn-backend.env`

```bash
sudo cat /etc/nn-backend.env
```

默认长这样：

```bash
NN_ENV=production
PORT=5174
BIND_HOST=127.0.0.1
NN_AUTH_TOKEN=<install.sh 生成的 64 位 hex>
ALLOWED_ORIGINS=http://www.netnexus.com.cn
MAX_INSTANCES=10
MAX_LINKS=30
IMAGE_ALLOWLIST=netnexus
CONTAINER_MEMORY=256m
CONTAINER_CPUS=0.5
```

**记下 `NN_AUTH_TOKEN`，下面 nginx 要用同一个值。**

完整变量表：

| 变量 | 说明 | 默认 |
|---|---|---|
| `NN_ENV` | `production` / `development` | 必填 |
| `PORT` | 监听端口 | `5174` |
| `BIND_HOST` | 监听地址 | `127.0.0.1` |
| `NN_AUTH_TOKEN` | `X-NN-Token` / `?token=` 校验值 | 随机生成 |
| `ALLOWED_ORIGINS` | 允许的 Origin，逗号分隔 | `http://www.netnexus.com.cn` |
| `MAX_INSTANCES` | 最大同时实例数 | `10` |
| `MAX_LINKS` | 最大链路数 | `30` |
| `BODY_LIMIT` | HTTP body 上限 | `8mb` |
| `MAX_DB_BYTES` | dbBase64 解码后上限 | `8388608`（8 MB） |
| `IMAGE_ALLOWLIST` | 允许的镜像前缀 | `netnexus` |
| `CONTAINER_MEMORY` | 每容器内存上限 | `256m` |
| `CONTAINER_CPUS` | 每容器 CPU 上限 | `0.5` |
| `CONTAINER_PIDS` | 每容器进程数上限 | `256` |
| `CONTAINER_NOFILE` | 每容器 nofile 上限 | `1024` |
| `DOCKER_TIMEOUT_MS` | 单条 docker 命令超时 | `60000` |
| `RATE_LIMIT_WINDOW_MS` | 限流窗口 | `60000` |
| `RATE_LIMIT_MAX` | 每 IP 每窗口最多写请求数 | `30` |

改完记得：

```bash
sudo systemctl restart nn-backend
sudo journalctl -u nn-backend -n 30
```

---

## 5. 前端静态资源

前端打包输出已经在 `/var/www/netnexus/`。每次前端更新：

```bash
# 开发机
cd /path/to/CNetNexus/web/frontend
npm ci
npm run build

# 同步到服务器
rsync -a --delete dist/ user@www.netnexus.com.cn:/var/www/netnexus/
```

前端代码调 `fetch('/api/...')` 和 `new WebSocket(...${location.host}/ws/...)`，
都是**相对路径**，走 nginx 同源，不用改任何配置。

---

## 6. nginx 配置（HTTP-only）

新建 `/etc/nginx/sites-available/netnexus`（或者直接改你原有的站点文件，把下面两个 `location` 加进去）：

```nginx
server {
    listen 80;
    server_name www.netnexus.com.cn netnexus.com.cn;

    # ===== 前端静态资源 =====
    root /var/www/netnexus;
    index index.html;

    location / {
        try_files $uri $uri/ /index.html;   # Vue history 路由兜底
    }

    # ===== /api 反代到后端，注入 token =====
    location /api/ {
        proxy_pass http://127.0.0.1:5174;
        proxy_set_header Host              $host;
        proxy_set_header X-Real-IP         $remote_addr;
        proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header X-NN-Token        "<TOKEN>";        # 同 NN_AUTH_TOKEN
        proxy_read_timeout 120s;
        proxy_send_timeout 120s;
        client_max_body_size 16m;
    }

    # ===== /ws 反代到后端（WebSocket），追加 token =====
    location /ws/ {
        if ($args = '')  { set $args "token=<TOKEN>"; }
        if ($args != '') { set $args "$args&token=<TOKEN>"; }

        proxy_pass http://127.0.0.1:5174;
        proxy_http_version 1.1;
        proxy_set_header Upgrade    $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host       $host;
        proxy_set_header X-Real-IP  $remote_addr;
        proxy_read_timeout 1h;      # 终端长连接
    }
}
```

启用 + reload：

```bash
sudo ln -sf /etc/nginx/sites-available/netnexus /etc/nginx/sites-enabled/netnexus
sudo nginx -t && sudo systemctl reload nginx
```

把**三处 `<TOKEN>`** 都换成 `/etc/nn-backend.env` 里 `NN_AUTH_TOKEN` 的值。

---

## 7. 防火墙

```bash
sudo ufw allow 80/tcp
sudo ufw deny  5174/tcp   # 明确禁止外网直连后端
sudo ufw enable
sudo ufw status
```

---

## 8. 自测

```bash
# A. 本机直连后端，没 token 应该 401
curl -o /dev/null -w '%{http_code}\n' http://127.0.0.1:5174/api/instances        # 401

# B. 带 token 应该 200
curl -H "X-NN-Token: <TOKEN>" http://127.0.0.1:5174/api/instances                 # 200

# C. 走 nginx，浏览器视角应该 200（token 由 nginx 补）
curl http://www.netnexus.com.cn/api/instances                                     # 200

# D. WebSocket 握手检查（nginx 应该回 101 或 400+，不能是 502）
curl -i --http1.1 \
  -H "Connection: Upgrade" -H "Upgrade: websocket" \
  -H "Sec-WebSocket-Key: x" -H "Sec-WebSocket-Version: 13" \
  "http://www.netnexus.com.cn/ws/terminal?id=nonexistent"
```

浏览器打开 `http://www.netnexus.com.cn`，能看到前端画布；拖一个设备 → 启动 → 双击打开终端 → 看到 `<NetNexus>` 提示符，全链路通。

---

## 9. 升级流程

**后端升级**（代码改动）：

```bash
# 开发机
cd /path/to/CNetNexus/web/backend
./package.sh
scp dist/netnexus-backend-*.tar.gz user@www.netnexus.com.cn:/tmp/

# 目标机
cd /tmp && tar xzf netnexus-backend-*.tar.gz && cd netnexus-backend-*/
sudo ./install.sh
```

`install.sh` 幂等：不会覆盖已存在的 `/etc/nn-backend.env`，`systemctl restart nn-backend` 自动拉起新代码。

升级**不会动容器**：所有 `nn-topo-*` 容器保持存在，只是容器里的 netnexus 进程会被 kill 掉，让用户在前端点启动再拉起（避免后端重启产生"自动启动"错觉）。

**前端升级**：只需 rsync `dist/` 到 `/var/www/netnexus/`，nginx 下次请求就取到新文件。

---

## 10. 运维命令速查

```bash
# 查看后端状态和日志
sudo systemctl status nn-backend
sudo journalctl -u nn-backend -f

# 重启后端
sudo systemctl restart nn-backend

# 看 nginx 访问/错误日志
sudo tail -f /var/log/nginx/access.log /var/log/nginx/error.log

# 手动清理所有拓扑容器（前端"清空"按钮背后的 API）
curl -X POST -H "X-NN-Token: <TOKEN>" http://127.0.0.1:5174/api/instances/cleanup

# 查看当前实例 / 链路
curl -H "X-NN-Token: <TOKEN>" http://127.0.0.1:5174/api/instances
curl -H "X-NN-Token: <TOKEN>" http://127.0.0.1:5174/api/links
```

---

## 11. 常见排错

| 症状 | 原因 / 处理 |
|---|---|
| `systemctl start nn-backend` 失败，journal 显示 `FATAL: production mode without AUTH_TOKEN / ALLOWED_ORIGINS / loopback bind` | env 文件里 `BIND_HOST` 非 `127.0.0.1` 且没设 token 和 origin。恢复 `BIND_HOST=127.0.0.1` |
| 前端 API 请求 401 | nginx 没注入 `X-NN-Token`，或值跟 env 文件对不上 |
| 前端 API 请求 429 | 触发 `MAX_INSTANCES` / `MAX_LINKS` / 限流，看 journal `[rate-limit]` |
| CORS 报错 | 浏览器访问的域名（含端口）不在 `ALLOWED_ORIGINS` 里 |
| WebSocket 连不上，F12 看到 502 | nginx `/ws/` 块少了 `proxy_http_version 1.1` 或 `Upgrade` 头 |
| WebSocket 连上但立即 `*** disconnected ***` | 检查 nginx `/ws/` 块的 `set $args "token=..."` 是否写了、token 对不对 |
| `docker run failed` 且 detail 含 `invalid image` | 镜像名不在 `IMAGE_ALLOWLIST` 前缀里。改 env 加前缀或改名 |
| 设备配 IPv6 报 `IF: add address ... failed: Permission denied` | 目标机/容器把 IPv6 关了。升级到新版本后后端会自动带 `--sysctl net.ipv6.conf.all/default.disable_ipv6=0`；老版本请手动在容器运行参数补上这两个 sysctl，并保留 `NET_ADMIN` 能力 |
| 设备停止再启动后 ping 不通 | 升级到本版本后应该修好（后端不再 `docker stop` 容器，只 kill 容器内 netnexus 进程） |
| `rate limit exceeded` | 写请求过于频繁，调大 `RATE_LIMIT_MAX` 或 `RATE_LIMIT_WINDOW_MS` |

---

## 12. 开发模式（本机调试）

开发机上：

```bash
cd web/backend
npm install
npm run dev     # NN_ENV=development node server.js
```

dev 默认放开：监听 `0.0.0.0:5174`、**不**校验 token、**不**强 Origin、body 64 MB、
限流 600/min、无实例/链路上限、无容器资源限制。方便联调。

前端开发：

```bash
cd web/frontend
npm install
npm run dev     # Vite 起在 5173，自动 proxy /api 和 /ws 到 5174
```
