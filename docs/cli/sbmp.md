# SBMP（BMP 服务器）CLI 文档

本文档描述 SBMP 模块（module-id: 8）提供的 BMP 服务器命令。

## 1. 配置命令（config 视图）

### 1.1 `bmp-server`
进入 BMP 服务器配置视图。

- **用法**：`bmp-server`
- **视图**：`config`
- **视图切换**：切换到 `config-bmp-server`

### 1.2 `no bmp-server`
删除 BMP 服务器配置并停止监听。

- **用法**：`no bmp-server`
- **视图**：`config`

## 2. BMP 服务器视图命令（`config-bmp-server`）

### 2.1 `server port <port-number>`
配置 BMP 服务器监听端口并启动监听。

- **用法**：`server port <port-number>`
- **视图**：`config-bmp-server`
- **参数**：
    - `<port-number>`：监听端口，类型为 `uint`，范围 1-65535。

### 2.2 `no server port`
停止监听并删除已配置端口。

- **用法**：`no server port`
- **视图**：`config-bmp-server`

## 3. 查看命令（global 视图）

### 3.1 `show bmp-server`
显示 BMP 服务器状态和运行统计。

### 3.2 `show bmp-server client [<client-id>]`
显示 BMP 客户端概要或指定客户端详情。

### 3.3 `show bmp-server peer [client <client-id>] [peer <peer-id>]`
显示对等体运行状态，可按客户端或对等体过滤。

### 3.4 `show bmp-server route af { ipv4-unicast | ipv6-unicast } [client <client-id>] [peer <peer-id>] [policy { pre | post | loc-rib }]`
显示内存中的镜像路由，可按客户端或对等体过滤。

- `policy pre`：策略前路由视图（Adj-RIB-In）
- `policy post`：策略后路由视图
- `policy loc-rib`：Loc-RIB 路由视图
- 省略 policy：显示 pre/post/loc-rib

## 4. 视图上下文

| 视图名称 | 提示符模板 | 说明 |
|---|---|---|
| `config-bmp-server` | `<NetNexus(config-bmp-server)>` | BMP 服务器配置视图 |
