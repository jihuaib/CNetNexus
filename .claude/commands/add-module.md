# 添加新模块

为 CNetNexus 创建一个完整的新模块（所有文件 + 注册）。

## 使用方式

`/add-module <模块名> <端口号>`

例如：`/add-module mpls 4007`

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
- `CATEGORY`：IPC 消息大类（通常与模块序号对应，如第 7 个模块 → `0x0007`）

---

### 第二步：创建目录结构

```
src/{module}/
├── {module}_main.c       # 模块主文件（constructor + 三阶段初始化）
├── {module}_main.h       # 模块全局状态声明
├── {module}_cli.c        # CLI 命令处理
├── {module}_cli.h        # CLI 处理函数声明
├── CMakeLists.txt        # 模块构建配置
└── resources/
    ├── module.conf       # 模块配置（ID、端口、.so 名）
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

#include "nn_dev.h"

/**
 * @brief {MODULE} 模块本地状态
 */
typedef struct
{
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文 */
    /* 在此添加模块特有字段 */
} {module}_local_t;

/** 全局模块状态 */
extern {module}_local_t *g_{module}_local;

/**
 * @brief IPC 消息处理主回调
 */
void {module}_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

#endif /* {MODULE}_MAIN_H */
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
#include "nn_dev.h"
#include "nn_errcode.h"
#include <string.h>
#include <stdlib.h>

/* 全局状态 */
{module}_local_t *g_{module}_local = NULL;

/* ─────────────────── 内部辅助 ─────────────────── */

/**
 * @brief 发送阶段响应给 DEV
 */
static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int errcode)
{
    uint32_t code = (uint32_t)errcode;
    dev_ipc_message_t *resp = dev_ipc_message_create(
        msg->msg_type,
        DEV_MODULE_ID_{MODULE},
        msg->src_module_id,
        msg->request_id,
        g_memdup2(&code, sizeof(code)),
        sizeof(code),
        g_free
    );
    dev_ipc_send_response(ctx, resp);
}

/* ─────────────────── 三阶段初始化 ─────────────────── */

/**
 * @brief Phase 1：MODULE_START - 建立到其他模块的 IPC 连接
 */
static void {module}_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START");

    /* 连接到所需模块（按需修改） */
    dev_ipc_connect(ctx, DEV_MODULE_ID_CFG, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CFG);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB,  DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

/**
 * @brief Phase 2：MODULE_CONNECT - 预留（数据库恢复等）
 */
static void {module}_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

/**
 * @brief Phase 3：MODULE_READY - 初始化数据库表、启动工作线程
 */
static void {module}_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY");

    /* TODO: 初始化数据库表 */
    /* int ret = db_rpc_create_table_from_def(ctx, &{MODULE}_XXX_TABLE); */

    /* TODO: 启动模块工作线程（如需要） */

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

/**
 * @brief Shutdown：清理所有资源
 */
static void {module}_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("{MODULE} 模块清理");

    /* TODO: 停止工作线程 */
    /* TODO: 释放模块特有资源 */

    g_{module}_local->dev_ipc_ctx = NULL;
    g_free(g_{module}_local);
    g_{module}_local = NULL;

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

/* ─────────────────── IPC 消息路由 ─────────────────── */

void {module}_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
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
        case CFG_MSG_TYPE_CLI:
            {module}_cli_handle_message(msg);
            break;
        case CFG_MSG_TYPE_CLI_CONTINUE:
            {module}_cli_handle_continue(msg);
            break;
        default:
            LOG_WARN("未知消息类型: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

/* ─────────────────── .so 构造器 ─────────────────── */

/**
 * @brief .so 构造器：dlopen 时自动执行，初始化 IPC 和本地状态
 */
__attribute__((constructor)) static void {module}_so_init(void)
{
    LOG_INFO("{MODULE} .so 加载，自初始化");

    dev_ipc_context_t *ctx = dev_ipc_init(
        DEV_MODULE_ID_{MODULE},
        "{module}",
        DEV_MODULE_PORT_{MODULE},
        {module}_msg_handler
    );

    if (!ctx)
    {
        LOG_ERROR("{MODULE}: IPC 初始化失败");
        return;
    }

    g_{module}_local = g_malloc0(sizeof({module}_local_t));
    g_{module}_local->dev_ipc_ctx = ctx;
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

#include "nn_dev.h"

/**
 * @brief 处理 CLI 命令消息
 */
int {module}_cli_handle_message(dev_ipc_message_t *msg);

/**
 * @brief 处理分块输出的继续请求
 */
int {module}_cli_handle_continue(dev_ipc_message_t *msg);

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
#include "nn_dev.h"
#include "nn_errcode.h"
#include <string.h>
#include <stdio.h>

/* CLI 命令组 ID */
#define {MODULE}_CLI_GROUP_ID_SHOW    1
/* 在此添加更多 group ID */

/* ─────────────────── 内部辅助 ─────────────────── */

/**
 * @brief 发送 CLI 文本响应
 */
static void {module}_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    size_t len = text ? strlen(text) + 1 : 1;
    char *payload = g_strdup(text ? text : "");
    dev_ipc_message_t *resp = dev_ipc_message_create(
        CFG_MSG_TYPE_CLI_RESP,
        DEV_MODULE_ID_{MODULE},
        msg->src_module_id,
        msg->request_id,
        payload,
        len,
        g_free
    );
    dev_ipc_send_response(g_{module}_local->dev_ipc_ctx, resp);
}

/* ─────────────────── 命令处理函数 ─────────────────── */

/**
 * @brief 处理 "show {module}" 命令（group-id=1）
 */
static int handle_show_{module}(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* 跳过所有 TLV（show 命令通常不需要参数） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    /* TODO: 构造输出内容 */
    GString *output = g_string_new("");
    g_string_append_printf(output, "{MODULE} 模块状态: 运行中\r\n");

    {module}_send_cli_response(msg, output->str);
    g_string_free(output, TRUE);
    return ERRCODE_SUCCESS;
}

/* ─────────────────── 消息 dispatch ─────────────────── */

int {module}_cli_handle_message(dev_ipc_message_t *msg)
{
    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, msg->payload, msg->payload_len) != 0)
    {
        {module}_send_cli_response(msg, "Error: 解析命令失败\r\n");
        return ERRCODE_FAIL;
    }

    switch (parser.group_id)
    {
        case {MODULE}_CLI_GROUP_ID_SHOW:
            return handle_show_{module}(msg, &parser);
        default:
            {module}_send_cli_response(msg, "Error: 未知命令\r\n");
            return ERRCODE_FAIL;
    }
}

int {module}_cli_handle_continue(dev_ipc_message_t *msg)
{
    /* TODO: 处理分页输出（如需要） */
    dev_ipc_message_free(msg);
    return ERRCODE_SUCCESS;
}
```

#### `src/{module}/CMakeLists.txt`

```cmake
set({MODULE}_SOURCES
    {module}_main.c
    {module}_cli.c
)

add_library({module} SHARED ${{{MODULE}_SOURCES}})

target_include_directories({module} PRIVATE .)

target_link_libraries({module} PRIVATE
    dev_api
    ${GLIB_LIBRARIES}
)

set_target_properties({module} PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
```

#### `src/{module}/resources/module.conf`

```
module-id=N
name={module}
so=lib{module}.so
port={PORT}
```

#### `src/{module}/resources/commands.xml`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<configuration module-id="N">
    <command_groups>
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
                <command>
                    <expression>1 2</expression>
                    <views>2,3</views>
                </command>
            </commands>
        </group>
    </command_groups>
</configuration>
```

---

### 第四步：注册到 `include/dev.h`

在 `include/dev.h` 中添加新模块的 ID、端口和消息类型：

```c
/* 模块 ID（在现有定义后追加） */
#define DEV_MODULE_ID_{MODULE}    0x0000000N

/* 模块端口（在现有定义后追加） */
#define DEV_MODULE_PORT_{MODULE}  {PORT}

/* {MODULE} 模块消息类型（大类 = 0x000N） */
#define {MODULE}_MSG_TYPE_REQ     DEV_IPC_MSG_TYPE(0x000N, 0x0001)
#define {MODULE}_MSG_TYPE_RESP    DEV_IPC_MSG_TYPE(0x000N, 0x00FF)
```

---

### 第五步：注册到 `src/CMakeLists.txt`

在 `src/CMakeLists.txt` 的 `add_subdirectory` 列表中追加：

```cmake
add_subdirectory({module})
```

---

### 第六步：添加到 DEV 的 CLI 路由（如需要）

如果 CFG 需要把 CLI 命令路由到新模块，在 `src/cfg/cli_dispatch.c`（或类似文件）中确认路由表包含新模块 ID。通常这是基于 XML 中 `module-id` 自动路由的，无需手动修改。

---

### 第七步：构建验证

```bash
./scripts/dev/build.sh

# 确认 .so 已生成
ls build/lib/lib{module}.so

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
- [ ] `CMakeLists.txt` 使用 `SHARED` 库（动态加载需要）
- [ ] `add_subdirectory({module})` 已加入 `src/CMakeLists.txt`
- [ ] `.so constructor` 中调用了 `dev_ipc_init()`
- [ ] 三阶段 handler（`on_start`/`on_connect`/`on_ready`/`on_shutdown`）均已实现
- [ ] `{module}_msg_handler` 中的 switch 包含四个阶段消息类型
- [ ] 构建成功，无编译错误
- [ ] `show {module}` 命令在 telnet 中可用
