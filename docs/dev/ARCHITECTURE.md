# CNetNexus 架构文档

## 概述

CNetNexus 采用**多进程架构**，每个功能模块作为独立进程运行，模块间通过 TCP IPC 通信。
进程管理器 `netnexus` 负责启动、监控和关闭所有子进程。

---

## 进程拓扑

```
                          ┌──────────────┐
                          │   netnexus   │
                          │  进程管理器   │
                          └──────┬───────┘
                                 │ fork/exec
          ┌──────────┬───────────┼───────────┬──────────┐
          ↓          ↓           ↓           ↓          ↓
     ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌────────┐ ┌─────────┐
     │  db  │ │ cfg  │ │ bgp  │ │ if  │ │route │
     │ :3790   │ │ :3791   │ │ :3793   │ │ :3792  │ │ :3794   │
     │ 数据库  │ │CLI/Telnet│ │  BGP    │ │ 接口   │ │  路由   │
     └─────────┘ └────┬────┘ └─────────┘ └────────┘ └─────────┘
                      │
                  Telnet :3788
                      │
                   用户终端
```

## 模块说明

| 进程 | 模块 ID | IPC 端口 | 职责 |
|------|---------|----------|------|
| `db` | `0x00000002` | 3790 | SQLite 数据库服务，处理所有 CRUD 请求 |
| `cfg` | `0x00000003` | 3791 | Telnet CLI 服务器（端口 3788），XML 命令解析，命令分发 |
| `bgp` | `0x00000005` | 3793 | BGP 协议管理，邻居/路由配置 |
| `if` | `0x00000004` | 3792 | 网络接口管理，接口状态维护 |
| `route` | `0x00000006` | 3794 | 路由表管理 |

---

## 进程管理器

**入口文件**：`src/main.c`

### 启动顺序

进程按依赖顺序启动，保证被依赖方先就绪：

```
1. db     (等待 500ms)    ← 其他模块依赖 DB
2. cfg    (等待 500ms)    ← 需要先注册 CLI 命令树
3. bgp    (等待 100ms)
4. if     (等待 100ms)
5. route  (等待 100ms)
```

### 信号处理

使用 `signalfd` + `epoll` 统一处理：

| 信号 | 行为 |
|------|------|
| `SIGCHLD` | 回收子进程，非关闭状态下延迟 2 秒后自动重启 |
| `SIGINT` / `SIGTERM` | 触发优雅关闭流程 |

### 关闭流程

1. 按**逆序**发送 `SIGTERM`：`route → if → bgp → cfg → db`
2. 等待最多 **5 秒**
3. 对未退出的进程发送 `SIGKILL`
4. `waitpid()` 回收所有子进程

---

## IPC 通信

### 连接矩阵

每个模块通过 TCP 建立 IPC 连接，箭头表示主动连接方向（客户端 → 服务器）：

```
           db    cfg   bgp   if   route
db       -        ←        ←        ←        -
cfg      →        -        →        →        →
bgp      →        ←        -        -        -
if       →        ←        -        -        -
route    -        ←        -        -        -
```

| 模块 | 连接到（客户端） | 接受来自（服务器） |
|------|------------------|-------------------|
| `db` | 无 | cfg, bgp, if |
| `cfg` | db, bgp, if, route | 无 |
| `bgp` | db, cfg | cfg |
| `if` | db, cfg | cfg |
| `route` | cfg | cfg |

### IPC 帧格式

所有字段使用网络字节序（大端），总头部 20 字节：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     magic (0x4E4E4950)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         msg_type                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        sender_id                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       request_id                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      payload_len                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     payload (TLV)...                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 大小 | 说明 |
|------|------|------|
| `magic` | 4 字节 | 固定值 `0x4E4E4950`（ASCII "NNIP"） |
| `msg_type` | 4 字节 | 消息类型（CLI / DB RPC / 内部控制） |
| `sender_id` | 4 字节 | 发送方模块 ID |
| `request_id` | 4 字节 | 请求/响应配对 ID |
| `payload_len` | 4 字节 | TLV 负载长度 |

### 消息类型

**IPC 内部控制消息**：

| 类型 | 值 | 说明 |
|------|------|------|
| `IPC_MSG_TYPE_HANDSHAKE` | `0x10000001` | 连接握手 |
| `IPC_MSG_TYPE_HANDSHAKE_ACK` | `0x10000002` | 握手确认 |
| `IPC_MSG_TYPE_HEARTBEAT` | `0x10000003` | 心跳（5 秒间隔） |
| `IPC_MSG_TYPE_HEARTBEAT_ACK` | `0x10000004` | 心跳确认 |
| `IPC_MSG_TYPE_SHUTDOWN` | `0x10000005` | 远程关闭请求 |

**CLI 消息**：

| 类型 | 值 | 说明 |
|------|------|------|
| `CFG_MSG_TYPE_CLI` | `0x00000001` | CLI 命令请求 |
| `CFG_MSG_TYPE_CLI_RESP` | `0x00000002` | CLI 响应（最终） |
| `CFG_MSG_TYPE_CLI_VIEW_CHG` | `0x00000003` | 视图切换 |
| `CFG_MSG_TYPE_CLI_RESP_MORE` | `0x00000004` | CLI 响应（部分，还有更多数据） |
| `CFG_MSG_TYPE_CLI_CONTINUE` | `0x00000005` | 请求下一批数据 |

**DB RPC 消息**：

| 类型 | 值 | 说明 |
|------|------|------|
| `IPC_MSG_TYPE_DB_INSERT` | `0x20000001` | 插入记录 |
| `IPC_MSG_TYPE_DB_UPDATE` | `0x20000002` | 更新记录 |
| `IPC_MSG_TYPE_DB_DELETE` | `0x20000003` | 删除记录 |
| `IPC_MSG_TYPE_DB_QUERY` | `0x20000004` | 查询记录 |
| `IPC_MSG_TYPE_DB_EXISTS` | `0x20000005` | 检查记录是否存在 |
| `IPC_MSG_TYPE_DB_RESP` | `0x20000006` | DB 操作响应 |

### 连接生命周期

- 启动时解析 `ipc.conf` 获取各模块的 `host:port`
- `ipc_connect()` 非阻塞连接 + HANDSHAKE 握手
- 心跳间隔 5 秒，超时 15 秒判定断连
- 断连后指数退避重连（500ms → 10s）

### 核心 API

```c
/* 初始化 IPC 上下文（启动 IO 线程和监听 socket） */
ipc_context_t *ipc_init(uint32_t module_id, const char *name,
                               const char *config_path,
                               ipc_msg_handler_fn msg_handler);

/* 连接到远程模块 */
int ipc_connect(ipc_context_t *ctx, uint32_t target_module_id);

/* 异步发送消息 */
int ipc_send(ipc_context_t *ctx, uint32_t target_module_id,
                dev_message_t *msg);

/* 同步查询（阻塞等待响应） */
dev_message_t *ipc_query(ipc_context_t *ctx,
                                uint32_t target_module_id,
                                dev_message_t *msg,
                                uint32_t timeout_ms);

/* 发送响应 */
int ipc_send_response(ipc_context_t *ctx, dev_message_t *msg);

/* 销毁 IPC 上下文 */
void ipc_destroy(ipc_context_t *ctx);
```

---

## 数据流

### CLI 命令处理流程

用户通过 Telnet 连接到 `cfg` 进程（端口 3788），输入命令后的完整数据流如下：

```
用户 (Telnet :3788)
  │
  │  输入: "show bgp peer"
  ↓
┌─────────────────────────────────────────────────────────────┐
│ cfg 进程                                                  │
│                                                              │
│  cli_handler.c    →  接收输入，行编辑，Tab 补全           │
│         │                                                    │
│         ↓                                                    │
│  cli_tree.c       →  匹配命令树，确定 module_id=0x05(BGP) │
│         │                                                    │
│         ↓                                                    │
│  cli_dispatch.c   →  序列化为 TLV 格式                    │
│         │                                                    │
│         ↓                                                    │
│  ipc_query()      →  通过 IPC 发送到 BGP 进程             │
└─────────┬───────────────────────────────────────────────────┘
          │ TCP :3793
          ↓
┌─────────────────────────────────────────────────────────────┐
│ bgp 进程                                                  │
│                                                              │
│  bgp_ipc_msg_handler  →  接收 IPC 消息                    │
│         │                                                    │
│         ↓                                                    │
│  bgp_cli_handle_message  →  解析 TLV，分发到命令处理函数   │
│         │                                                    │
│         ↓                                                    │
│  cmd_show_bgp_peer()  →  执行业务逻辑                        │
│         │                                                    │
│         ├── db_query()  →  查询 DB（通过 DB RPC）          │
│         │                                                    │
│         ↓                                                    │
│  ipc_send_response()  →  返回结果给 CFG                   │
└─────────┬───────────────────────────────────────────────────┘
          │
          ↓
┌─────────────────────────────────────────────────────────────┐
│ cfg 进程                                                  │
│                                                              │
│  cli_pager_output()  →  输出到 Telnet 终端                │
└─────────────────────────────────────────────────────────────┘
```

### CLI TLV 消息格式

CFG 将命令参数打包为 TLV 格式发送给目标模块：

```
┌──────────────────┬──────────────────┬──────────┬──────────────┐
│  group_id (4B)   │  cfg_id_1 (4B)   │ len (2B) │ value (变长) │
├──────────────────┼──────────────────┼──────────┼──────────────┤
│                  │  cfg_id_2 (4B)   │ len (2B) │ value (变长) │
│                  ├──────────────────┼──────────┼──────────────┤
│                  │       ...        │   ...    │     ...      │
└──────────────────┴──────────────────┴──────────┴──────────────┘
```

- `group_id`：命令组 ID（对应 XML 中的 `<group group-id="N">`）
- `cfg_id`：元素 ID（对应 XML 中的 `<element>` 序号）
- 上下文参数的 `cfg_id` 高位设置标记 `0x80000000`

### CLI 分页协议

对于大量输出数据，使用多轮请求/响应：

```
CFG                         目标模块
 │                              │
 │── CLI 请求 ───────────────→ │
 │                              │
 │←── CLI_RESP_MORE (部分) ──── │   (还有更多数据)
 │                              │
 │── CLI_CONTINUE ──────────→  │   (请求下一批)
 │                              │
 │←── CLI_RESP (最终) ──────── │   (最后一批)
 │                              │
```

### DB RPC 数据流

业务模块（BGP、IF 等）通过 `libdb_client.so` 透明访问远程 DB 进程：

```
bgp 进程                      db 进程
    │                                │
    │  db_query("bgp", ...)       │
    │  (libdb_client.so)          │
    │         │                      │
    │         ↓                      │
    │  序列化参数 → TLV              │
    │         │                      │
    │  ipc_query(DB, msg, 5s)     │
    │────── TCP :3790 ─────────────→ │
    │                                │  db_ipc_msg_handler()
    │                                │         │
    │                                │  反序列化参数
    │                                │         │
    │                                │  db_api.c → SQLite
    │                                │         │
    │                                │  序列化结果
    │                                │         │
    │←───── DB_RESP ─────────────── │
    │                                │
    │  反序列化 → db_result_t     │
    │                                │
```

`db_client.so` 提供与本地 `db_api.c` 完全相同的函数签名，调用方代码无需修改：

```c
/* 以下函数在 db_client 中通过 IPC RPC 实现，接口与本地版本完全一致 */
int db_insert(const char *db_name, const char *table_name, ...);
int db_update(const char *db_name, const char *table_name, ...);
int db_delete(const char *db_name, const char *table_name, ...);
int db_query(const char *db_name, const char *table_name, ..., db_result_t **result);
int db_exists(const char *db_name, const char *table_name, ..., gboolean *exists);
```

---

## 构建产物

### 可执行文件（`build/bin/`）

| 文件 | 链接库 | 说明 |
|------|--------|------|
| `netnexus` | utils | 进程管理器 |
| `db` | db, cfg, ipc, dev, utils | 数据库进程 |
| `cfg` | cfg, db, ipc, dev, utils | CLI 进程 |
| `bgp` | cfg, db_client, ipc, dev, utils | BGP 进程 |
| `if` | cfg, db_client, ipc, dev, utils | 接口进程 |
| `route` | cfg, db_client, ipc, dev, utils | 路由进程 |

### 共享库（`build/lib/`）

| 库 | 职责 |
|----|------|
| `libipc.so` | IPC 框架：TCP 连接、帧收发、IO 线程、心跳、重连 |
| `libdb_client.so` | DB RPC 代理：序列化 DB 操作 → IPC 请求 → 反序列化响应 |
| `libdb.so` | DB 服务端：schema 管理、SQLite CRUD、IPC 消息处理 |
| `libcfg.so` | CLI 框架：XML 解析、命令树、视图、行编辑、分发 |
| `libdev.so` | 公共工具：消息结构创建/释放、模块 ID 名称查询 |
| `libutils.so` | 通用工具：路径解析 |

### 库依赖关系

```
libutils.so          (无依赖)
libdev.so            (glib)
libipc.so            → libdev.so, glib, pthread
libdb_client.so      → libipc.so, glib
libdb.so             → libipc.so, sqlite3, glib
libcfg.so            → libipc.so, libdev.so, libxml2, glib, pthread
```

---

## 源码目录结构

```
src/
├── main.c                           # 进程管理器入口
├── CMakeLists.txt                   # 顶层构建配置
│
├── ipc/                             # IPC 库 (libipc.so)
│   ├── ipc_context.c/h           #   上下文初始化/销毁
│   ├── ipc_connection.c/h        #   TCP 连接管理、重连
│   ├── ipc_frame.c/h             #   帧序列化/反序列化
│   ├── ipc_io.c/h                #   epoll IO 线程
│   ├── ipc_query.c/h             #   同步查询（condvar）
│   └── resources/ipc.conf           #   端口配置
│
├── db/                              # DB 模块
│   ├── db_process.c              #   进程入口
│   ├── db_main.c/h               #   模块生命周期
│   ├── db_api.c                  #   SQLite CRUD 操作
│   ├── db_schema.c               #   Schema 管理
│   ├── db_registry.c/h           #   数据库定义注册表
│   ├── db_ipc_handler.c/h        #   IPC 消息处理（DB RPC 服务端）
│   └── db_cli.c                  #   DB 模块 CLI 命令处理
│
├── db_client/                       # DB RPC 客户端 (libdb_client.so)
│   ├── db_client.c               #   CRUD 函数的 RPC 代理实现
│   └── db_serialize.c/h          #   参数序列化/反序列化
│
├── cfg/                             # CLI 模块
│   ├── cfg_process.c             #   进程入口
│   ├── cfg_main.c/h              #   模块生命周期、Telnet 服务器
│   ├── cli_handler.c/h           #   会话管理、输入处理
│   ├── cli_dispatch.c/h          #   命令分发、TLV 打包
│   ├── cli_tree.c/h              #   命令树匹配
│   ├── cli_view.c/h              #   视图层级
│   ├── cli_xml_parser.c/h        #   XML 配置解析
│   └── cli_history.c/h           #   命令历史
│
├── bgp/                             # BGP 模块
│   ├── bgp_process.c             #   进程入口
│   ├── bgp_main.c/h              #   模块生命周期
│   └── bgp_cli.c/h               #   CLI 命令处理
│
├── if/                              # 接口模块
│   ├── if_process.c              #   进程入口
│   ├── if_main.c/h               #   模块生命周期
│   └── if_cli.c/h                #   CLI 命令处理
│
├── route/                           # 路由模块
│   ├── route_process.c           #   进程入口
│   ├── route_main.c/h            #   模块生命周期
│   └── route_cli.c/h             #   CLI 命令处理
│
├── dev/                             # 公共工具库 (libdev.so)
│   └── dev_core.c                #   消息创建/释放、模块名查询
│
└── utils/                           # 通用工具 (libutils.so)
    └── path_utils.c/h            #   路径解析
```

### 公共头文件（`include/`）

| 头文件 | 内容 |
|--------|------|
| `dev.h` | 模块 ID 定义、`dev_message_t` 消息结构 |
| `cfg.h` | TLV 协议常量、CLI 消息类型 |
| `db.h` | DB API 接口（CRUD 函数声明、结果结构体） |
| `ipc.h` | IPC 公共 API（init/connect/send/query/destroy） |
| `errcode.h` | 错误码定义 |
