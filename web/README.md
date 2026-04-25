# NetNexus 拓扑编排 Web UI

一个 Vue 3 + Node.js 实现的轻量拓扑编排面板，用于把 NetNexus 设备拖到画布上、选择镜像、连线、启动，并直接在浏览器里通过 Web 终端连接到容器内的 telnet CLI（端口 3788）。

## 目录结构

```
web/
├── backend/         # Express + WebSocket，调用 docker，桥接浏览器 <-> telnet
│   ├── server.js
│   └── package.json
├── frontend/        # Vue 3 + Vite：官网落地页 + 拓扑编排（`/top`）
│   ├── src/
│   │   ├── App.vue              # 根壳，仅 `<router-view />`
│   │   ├── router/index.js      # `/` 客户端站；`/netnexus` C 侧说明；`/netnexus/top` 拓扑
│   │   ├── views/HomeView.vue
│   │   ├── views/ClassicLandingView.vue   # 路由 `/`，客户端软件介绍（默认）
│   │   ├── views/TopologyView.vue
│   │   ├── landing/             # 原 web_temp 官网组件与样式
│   │   ├── main.js
│   │   ├── styles.css
│   │   └── components/          # 拓扑画布相关
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
- Docker（已构建好包含 `tcpdump` 的 `netnexus` 镜像，推荐 `netnexus:latest`）

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

- 前端：客户端默认 <http://localhost:5173/>；C / NetNexus Web 说明 <http://localhost:5173/netnexus>；拓扑编排 <http://localhost:5173/netnexus/top>（`/top`、`/home` 会重定向到新路径）
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
6. 单击一条**连线**后，右侧会显示链路详情和 **抓包** 面板：
   - 点击 **开始抓包**，后端会启动一个辅助 `tcpdump` 容器抓取该链路的 docker bridge 流量
   - 页面会实时显示 `tcpdump` 文本输出
   - 点击 **停止抓包** 后可用 **下载 pcap** 保存到本地
7. 选中节点后按 `Delete` 或在面板里点击 **删除**，会停止并移除对应容器；关联连线也会同步删除。

> 抓包依赖本地 `netnexus` 镜像内带有 `tcpdump`。当前 Dockerfile 已把 `tcpdump` 打进 deployable 基底，重建 `netnexus:latest` 后即可直接用于抓包辅助容器。

## API 一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/images` | 列出本地 `*netnexus*` 镜像 |
| GET | `/api/instances` | 当前实例列表 |
| POST | `/api/instances` | `{ id, image }`，启动一个容器 |
| DELETE | `/api/instances/:id` | 停止并删除容器 |
| POST | `/api/links` | `{ from, to }`，记录两节点连线 |
| GET | `/api/links/:id/capture` | 查看某条链路最近一次抓包状态 |
| POST | `/api/links/:id/capture/start` | 启动链路抓包 |
| POST | `/api/links/:id/capture/stop` | 停止链路抓包 |
| GET | `/api/captures/:id/download` | 下载抓包 `pcap` |
| WS | `/ws/terminal?id=<instId>` | 浏览器 ↔ 容器 telnet 3788 桥接 |

## 自定义

- 后端默认端口 `5174`：`PORT=6000 npm start`
- 后端 docker 命令前缀使用 sudo：`USE_SUDO=1`
- 前端通过 Vite 代理转发 `/api` 与 `/ws` 到后端，构建产物可由任意静态服务器托管，并自行配置反向代理

## 已知限制

- 连线目前仅在 UI 上展示，并未在容器之间真正建立 veth/macvlan，多机互通请使用 GNS3
- 实例信息存于内存，重启后端即丢失（容器仍在 docker 中，可手动 `docker rm -f nn-topo-*` 清理）
- `npm start` 在前台运行，关闭终端会一并停止前后端
- 如果本地 `netnexus` 镜像是在旧 Dockerfile 下构建的，镜像里可能还没有 `tcpdump`；这种情况下需要重新构建镜像后再使用抓包
