# 添加新模块

为 CNetNexus 创建一个完整的新模块（所有文件 + 注册）。

## 使用方式

`/add-module <模块名> <端口号>`

例如：`/add-module mpls 4010`

---

## 执行步骤

收到请求后，依次完成以下所有步骤。

---

### 第一步：确认信息

在开始前，从以下文件确认现有模块的 ID 和端口，避免冲突：
- `include/dev.h` — `DEV_MODULE_ID_*` 和 `DEV_MODULE_PORT_*` 定义
- `src/CMakeLists.txt` — 已有 `add_subdirectory()` 列表

确定新模块的：
- `MODULE_ID`：在现有最大 ID 基础上 +1
- `PORT`：用户指定的端口号
- `CATEGORY`：IPC 消息大类（通常与模块序号对应，如第 9 个模块 → `0x0009`）

---

### 第二步：创建目录结构

每个模块是**独立可执行进程**（不是 .so 动态库），由 DEV 在启动时 fork/exec。

```
src/{module}/
├── {module}_proc.c       # 独立进程入口（main 函数）
├── {module}_main.c       # 模块主文件（三阶段初始化 + IPC 分发）
├── {module}_main.h       # 模块全局状态声明
├── {module}_cli.c        # CLI 命令处理
├── {module}_cli.h        # CLI 处理函数声明 + group-id 常量
├── CMakeLists.txt        # 模块构建配置
└── resources/
    ├── module.conf       # 模块配置（ID、端口、可执行文件名）
    └── commands.xml      # CLI 命令定义
```

---

### 第三步：创建各文件

#### `src/{module}/{module}_main.h`

```c
/**
 * @file   {module}_main.h
 * @brief  {MODULE} 模块全局状态声明
 * @author 作者
 * @date   创建日期
 */
#ifndef {MODULE}_MAIN_H
#define {MODULE}_MAIN_H

#include <glib.h>

#include "cli.h"
#include "dev.h"

/**
 * @brief {MODULE} 模块本地状态
 */
typedef struct {module}_local
{
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文 */
    cli_chunk_stream_t show_stream; /**< CLI show 命令分片输出状态 */
    volatile int running;           /**< 运行标志 */
    /* 在此添加模块特有字段 */
} {module}_local_t;

/** 全局模块状态 */
extern {module}_local_t *g_{module}_local;

/**
 * @brief IPC 消息处理主回调
 */
void {module}_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 模块初始化（由 {module}_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int {module}_module_init(void);

#endif /* {MODULE}_MAIN_H */
```

#### `src/{module}/{module}_proc.c`

```c
/**
 * @file   {module}_proc.c
 * @brief  {MODULE} 独立进程入口
 * @author 作者
 * @date   创建日期
 */
#include <signal.h>
#include <sys/prctl.h>

#include "{module}_main.h"

int main(void)
{
    /* 父进程（DEV）意外退出时自动终止本进程 */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* 忽略 SIGPIPE（TCP 写入已关闭连接时不崩溃） */
    signal(SIGPIPE, SIG_IGN);

    /* 显式初始化模块（IPC 连接、本地状态） */
    if ({module}_module_init() != 0)
    {
        return 1;
    }

    /* 阻塞 SIGTERM/SIGINT，通过 sigwait 捕获关闭信号 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig = 0;
    sigwait(&mask, &sig);

    return 0;
}
```

#### `src/{module}/{module}_main.c`

```c
/**
 * @file   {module}_main.c
 * @brief  {MODULE} 模块主文件：IPC 初始化 + 三阶段生命周期
 * @author 作者
 * @date   创建日期
 */
#include "{module}_main.h"
#include "{module}_cli.h"

#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

/* 全局状态 */
{module}_local_t *g_{module}_local = NULL;

/* ─────────────────── 内部辅助 ─────────────────── */

/**
 * @brief 发送阶段响应给 DEV，并释放原始消息
 */
static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(
        DEV_IPC_MSG_TYPE_DEV_MODULE_RESP,
        DEV_MODULE_ID_{MODULE},
        msg->src_module_id,
        msg->request_id,
        NULL, 0, NULL
    );
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

/* ─────────────────── 三阶段初始化 ─────────────────── */

/**
 * @brief Phase 1：MODULE_START - 建立到其他模块的 IPC 连接
 */
static void {module}_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START");

    /* 连接到所需模块（按需修改） */
    if (dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI) != 0)
    {
        LOG_ERROR("Failed to connect to CLI module");
    }
    if (dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB) != 0)
    {
        LOG_ERROR("Failed to connect to DB module");
    }

    send_phase_response(ctx, msg);
}

/**
 * @brief Phase 2：MODULE_CONNECT - 预留
 */
static void {module}_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg);
}

/**
 * @brief Phase 3：MODULE_READY - 初始化数据库表、恢复状态
 */
static void {module}_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY");

    /* TODO: 初始化数据库表 */
    /* int ret = db_rpc_create_table_from_def(ctx, &{MODULE}_XXX_TABLE); */

    /* TODO: 从 DB 恢复内存状态 */
    /* {module}_db_restore(ctx); */

    LOG_INFO("{MODULE} module ready");
    send_phase_response(ctx, msg);
}

/**
 * @brief Shutdown：清理所有资源
 */
static void {module}_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("{MODULE} module shutting down");

    if (g_{module}_local)
    {
        g_{module}_local->running = 0;

        /* TODO: 释放模块特有资源 */

        {module}_cli_cleanup_state();

        g_{module}_local->dev_ipc_ctx = NULL;
        free(g_{module}_local);
        g_{module}_local = NULL;
    }

    LOG_INFO("{MODULE} module cleanup complete");
    send_phase_response(ctx, msg);
}

/* ─────────────────── IPC 消息路由 ─────────────────── */

void {module}_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            {module}_on_start(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            {module}_on_connect(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            {module}_on_ready(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            {module}_on_shutdown(ctx, msg);
            return;

        /* ---- CLI 消息 ---- */
        case CLI_MSG_TYPE:
            {module}_cli_handle_message(msg);
            return;
        case CLI_MSG_TYPE_CONTINUE:
            {module}_cli_handle_continue(msg);
            return;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            {module}_cli_handle_show_config(msg);
            return;

        default:
            LOG_WARN("未知消息类型: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

/* ─────────────────── 模块初始化 ─────────────────── */

int {module}_module_init(void)
{
    LOG_INFO("{MODULE} module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(
        DEV_MODULE_ID_{MODULE},
        "{module}",
        DEV_MODULE_PORT_{MODULE},
        {module}_ipc_msg_handler
    );

    if (!ctx)
    {
        LOG_ERROR("{MODULE}: IPC 初始化失败");
        return -1;
    }

    g_{module}_local = ({module}_local_t *)g_malloc0(sizeof({module}_local_t));
    if (!g_{module}_local)
    {
        LOG_ERROR("{MODULE}: 分配本地状态失败");
        return -1;
    }

    g_{module}_local->dev_ipc_ctx = ctx;
    g_{module}_local->running = 1;

    return 0;
}
```

#### `src/{module}/{module}_cli.h`

```c
/**
 * @file   {module}_cli.h
 * @brief  {MODULE} 模块 CLI 命令处理声明
 * @author 作者
 * @date   创建日期
 */
#ifndef {MODULE}_CLI_H
#define {MODULE}_CLI_H

#include "dev.h"

/** CLI group-id 定义（与 commands.xml 中 group-id 一致） */
#define {MODULE}_CLI_GROUP_ID_SHOW 1 /**< show {module} 命令 */

/**
 * @brief 处理 CLI 命令消息
 */
int {module}_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理分片输出继续请求
 */
int {module}_cli_handle_continue(dev_ipc_message_t *msg);

/**
 * @brief 处理 show current-configuration 请求
 */
int {module}_cli_handle_show_config(dev_ipc_message_t *msg);

/**
 * @brief 清理 CLI 内部状态（如分片输出缓存）
 */
void {module}_cli_cleanup_state(void);

#endif /* {MODULE}_CLI_H */
```

#### `src/{module}/{module}_cli.c`

```c
/**
 * @file   {module}_cli.c
 * @brief  {MODULE} 模块 CLI 命令处理实现
 * @author 作者
 * @date   创建日期
 */
#include "{module}_cli.h"
#include "{module}_main.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

/* ─────────────────── 内部辅助 ─────────────────── */

/**
 * @brief 发送 CLI 文本响应
 */
static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    const char *safe = text ? text : "";
    char *payload = g_strdup(safe);
    dev_ipc_message_t *resp = dev_ipc_message_create(
        CLI_MSG_TYPE_RESP,
        DEV_MODULE_ID_{MODULE},
        msg->src_module_id,
        msg->request_id,
        payload,
        strlen(payload) + 1,
        g_free
    );
    if (resp)
    {
        dev_ipc_send_response(g_{module}_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

/**
 * @brief 启动分片输出
 */
static int send_chunked(dev_ipc_message_t *msg, GString *text)
{
    return cli_chunk_stream_start(
        &g_{module}_local->show_stream,
        g_{module}_local->dev_ipc_ctx,
        DEV_MODULE_ID_{MODULE},
        msg,
        text
    );
}

/* ─────────────────── Group 1: show {module} ─────────────────── */

static int handle_show_{module}(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* 跳过所有 TLV 条目（含上下文条目） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    /* TODO: 构造输出内容 */
    g_string_append_printf(buf, "\r\n{MODULE} 模块状态: 运行中\r\n\r\n");

    return send_chunked(msg, buf);
}

/* ─────────────────── 主入口 ─────────────────── */

int {module}_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(&g_{module}_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("{MODULE} CLI payload 解析失败");
        send_resp(msg, "Error: Command payload parsing failed\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("{MODULE} CLI TLV (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case {MODULE}_CLI_GROUP_ID_SHOW:
            result = handle_show_{module}(msg, &parser);
            break;
        default:
            LOG_WARN("{MODULE} 未知 group_id: %u", parser.group_id);
            send_resp(msg, "Error: Unknown command group\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

int {module}_cli_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(
        &g_{module}_local->show_stream,
        g_{module}_local->dev_ipc_ctx,
        DEV_MODULE_ID_{MODULE},
        msg
    );
}

int {module}_cli_handle_show_config(dev_ipc_message_t *msg)
{
    GString *out = g_string_new("");
    if (!out)
    {
        return send_chunked(msg, NULL);
    }

    /* TODO: 从 DB 读取配置并生成可重放的 CLI 命令文本 */

    return send_chunked(msg, out);
}

void {module}_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_{module}_local->show_stream);
}
```

#### `src/{module}/CMakeLists.txt`

```cmake
set({MODULE}_SOURCES
    {module}_main.c
    {module}_cli.c
    # 按需添加更多源文件
)

add_executable(netnexus-{module} {module}_proc.c ${{{MODULE}_SOURCES}})

target_include_directories(netnexus-{module} PRIVATE .)

target_link_libraries(netnexus-{module} PRIVATE
    dev_api
    cli_api
    db_api
)

set_target_properties(netnexus-{module} PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

install(TARGETS netnexus-{module} RUNTIME DESTINATION bin)
```

#### `src/{module}/resources/module.conf`

```
module-id=N
name={module}
exe=netnexus-{module}
port={PORT}
```

> `module-id` 的数字值必须与 `include/dev.h` 中 `DEV_MODULE_ID_{MODULE}` 一致。

#### `src/{module}/resources/commands.xml`

视图名称使用字符串（`global`、`user`、`config`），与 `include/cli.h` 中 `CLI_VIEW_*` 常量对应。

```xml
<?xml version="1.0" encoding="UTF-8"?>
<configuration module-id="N">
    <command_groups>

        <!-- ================================================================
             Group 1: show {module} 命令
             ================================================================ -->
        <group group-id="1">
            <elements>
                <element type="keyword">
                    <name>show</name>
                    <description>显示信息</description>
                </element>
                <element type="keyword">
                    <name>{module}</name>
                    <description>{MODULE} 模块信息</description>
                </element>
            </elements>
            <commands>
                <!-- show {module} -->
                <command>
                    <expression>1 2</expression>
                    <views>global</views>
                </command>
            </commands>
        </group>

    </command_groups>
</configuration>
```

**视图名称速查（`include/cli.h`）：**

| 常量 | XML 字符串 | 说明 |
|------|-----------|------|
| `CLI_VIEW_GLOBAL` | `global` | 全局视图（所有视图均可用） |
| `CLI_VIEW_USER` | `user` | 用户视图 |
| `CLI_VIEW_CONFIG` | `config` | 配置视图 |
| `CLI_VIEW_BGP` | `bgp` | BGP 配置视图 |
| `CLI_VIEW_IF` | `if` | 接口配置视图 |
| `CLI_VIEW_ROUTE` | `route` | 路由视图 |

多视图可用：`<views>user,config</views>`

---

### 第四步：注册到 `include/dev.h`

在 `include/dev.h` 中追加新模块的 ID、端口和消息类别：

```c
/** {MODULE} 模块 */
#define DEV_MODULE_ID_{MODULE}    0x0000000N

/** {MODULE} 模块 IPC 监听端口 */
#define DEV_MODULE_PORT_{MODULE}  {PORT}

/** {MODULE} 模块消息大类 */
#define DEV_IPC_CATEGORY_{MODULE} 0x000N
```

如需定义模块间通信消息类型，在 `include/{module}.h`（新建）中定义：

```c
#define {MODULE}_MSG_TYPE_REQ   DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_{MODULE}, 0x0001)
#define {MODULE}_MSG_TYPE_RESP  DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_{MODULE}, 0x00FF)
```

---

### 第五步：注册到 `src/CMakeLists.txt`

```cmake
add_subdirectory({module})
```

---

### 第六步：构建验证

```bash
./scripts/dev/build.sh

# 确认可执行文件已生成
ls build/bin/netnexus-{module}

# 运行并验证模块加载日志
./scripts/dev/start.sh 2>&1 | grep -i "{module}"

# 连接测试
telnet localhost 3788
> show {module}
```

---

## 检查清单

完成后确认以下所有项：

- [ ] `DEV_MODULE_ID_{MODULE}` 在 `include/dev.h` 中定义（无冲突）
- [ ] `DEV_MODULE_PORT_{MODULE}` 在 `include/dev.h` 中定义（无冲突）
- [ ] `resources/module.conf` 的 `module-id` 与 `dev.h` 中的值一致
- [ ] `resources/module.conf` 的 `port` 与 `dev.h` 中的值一致
- [ ] `resources/module.conf` 使用 `exe=netnexus-{module}`（不是 `so=`）
- [ ] `CMakeLists.txt` 使用 `add_executable(netnexus-{module} ...)`
- [ ] `{module}_proc.c` 有 `main()` 函数，调用 `{module}_module_init()`
- [ ] `{module}_module_init()` 调用 `dev_ipc_init()` 并分配 `g_{module}_local`
- [ ] `{module}_ipc_msg_handler` switch 包含四个生命周期消息类型
- [ ] `{module}_ipc_msg_handler` switch 包含 `CLI_MSG_TYPE`、`CLI_MSG_TYPE_CONTINUE`、`CLI_MSG_TYPE_SHOW_CONFIG`
- [ ] `add_subdirectory({module})` 已加入 `src/CMakeLists.txt`
- [ ] 构建成功，无编译错误
- [ ] `show {module}` 命令在 telnet 中可用
