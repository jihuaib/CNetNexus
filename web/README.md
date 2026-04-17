# NetNexus 拓扑编排 Web UI

一个 Vue 3 + Node.js 实现的轻量拓扑编排面板，用于把 NetNexus 设备拖到画布上、选择镜像、连线、启动，并直接在浏览器里通过 Web 终端连接到容器内的 telnet CLI（端口 3788）。

## 目录结构

```
web/
├── backend/         # Express + WebSocket，调用 docker，桥接浏览器 <-> telnet
│   ├── server.js
│   └── package.json
├── frontend/        # Vue 3 + Vite，货架 / 画布 / 连线 / xterm 终端
│   ├── src/
│   │   ├── App.vue
│   │   ├── main.js
│   │   ├── styles.css
│   │   └── components/
│   │       ├── DeviceShelf.vue
│   │       ├── TopologyCanvas.vue
│   │       └── WebTerminal.vue
│   ├── index.html
│   ├── vite.config.js
│   └── package.json
├── start.sh         # 一键安装依赖并启动前后端
└── README.md
```

## 前置要求

- Node.js 18+（推荐 20+）
- Docker（已构建好 `netnexus:latest` 或 `netnexus:debug` 镜像）

构建 NetNexus 镜像（项目根目录执行）：

```bash
docker build --target production -t netnexus:latest .
# 或调试镜像
docker build --target debug --build-arg BUILD_TYPE=Debug -t netnexus:debug .
```

## 一键启动

```bash
./web/start.sh
# 如需用 sudo 调用 docker：
USE_SUDO=1 ./web/start.sh
```

启动后访问：

- 前端：<http://localhost:5173>
- 后端：<http://localhost:5174>（已通过 Vite 代理，无需直接访问）

## 操作流程

1. 从左侧 **设备货架** 拖一个 `NetNexus` 到画布
2. 在右侧 **属性** 面板里：
   - 修改设备名称
   - 选择镜像（自动列出本地的 `*netnexus*` 镜像）
3. 点击 **启动**，后端会执行：
   ```
   docker run -d --name nn-topo-<id> \
     --cap-add=SYS_PTRACE --cap-add=NET_ADMIN \
     --security-opt seccomp=unconfined \
     -p <宿主机端口>:3788 <image>
   ```
4. 节点变成 `running` 后，点击 **网页连接**（或双击节点），弹出 **xterm 终端**，浏览器与容器内 telnet 3788 实时双向通信，回车即可看到 CLI 提示符。
5. 拖动节点右侧的小蓝点到另一个节点，可以创建一条**连线**（当前为逻辑连线，仅记录拓扑关系；真正的多设备 veth 组网建议结合 GNS3 使用 `gns3/netnexus.gns3a`）。
6. 选中节点后按 `Delete` 或在面板里点击 **删除**，会停止并移除对应容器。

## API 一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/images` | 列出本地 `*netnexus*` 镜像 |
| GET | `/api/instances` | 当前实例列表 |
| POST | `/api/instances` | `{ id, image }`，启动一个容器 |
| DELETE | `/api/instances/:id` | 停止并删除容器 |
| POST | `/api/links` | `{ from, to }`，记录两节点连线 |
| WS | `/ws/terminal?id=<instId>` | 浏览器 ↔ 容器 telnet 3788 桥接 |

## 自定义

- 后端默认端口 `5174`：`PORT=6000 npm start`
- 后端 docker 命令前缀使用 sudo：`USE_SUDO=1`
- 前端通过 Vite 代理转发 `/api` 与 `/ws` 到后端，构建产物可由任意静态服务器托管，并自行配置反向代理

## 已知限制

- 连线目前仅在 UI 上展示，并未在容器之间真正建立 veth/macvlan，多机互通请使用 GNS3
- 实例信息存于内存，重启后端即丢失（容器仍在 docker 中，可手动 `docker rm -f nn-topo-*` 清理）
- `npm start` 在前台运行，关闭终端会一并停止前后端
