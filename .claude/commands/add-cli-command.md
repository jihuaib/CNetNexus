# 添加 CLI 命令

为 CNetNexus 模块添加新 CLI 命令（XML 定义 + C 处理函数 + DB 操作 + 内存恢复 + BDR）。

## 使用方式

`/add-cli-command <模块名> <命令描述>`

例如：`/add-cli-command bgp show bgp peer detail`

---

## 第一步：分析需求

在开始之前，明确以下信息：

1. **目标模块**：命令属于哪个模块（bgp / if / dev / db / cfg）
2. **可用视图**：命令在哪个 CLI 视图下可用（参考 `include/cli.h` 的 `CLI_VIEW_*`）
3. **命令语法**：完整命令语法，包括关键字和参数
4. **参数类型**：每个参数的类型（`uint`、`string`、`ip`、`prefix` 等）
5. **是否切换视图**：执行后是否进入子视图（如 `bgp 65000` 后进入 BGP 视图）
6. **命令类型**：show（只读）/ 配置（读写）/ 进入视图

---

## 第二步：查阅现有定义

先读取目标模块的现有文件，了解已有的 group-id、cfg-id 范围：

```
读取 src/{module}/resources/commands.xml
读取 src/{module}/{module}_cli.c
读取 src/{module}/{module}_cli.h   (group-id 常量)
读取 src/{module}/{module}_db.c/h  (表定义和 DB 操作)
读取 src/{module}/{module}_bdr.c/h (show current-configuration)
读取 include/cli.h  (视图 ID 定义)
```

---

## 第三步：定义 XML（commands.xml）

在 `src/{module}/resources/commands.xml` 的 `<command_groups>` 中添加新的 `<group>`：

### XML 结构模板

```xml
<group group-id="N">
    <elements>
        <!-- 关键字元素（type="keyword"，无 cfg-id） -->
        <element type="keyword">
            <name>show</name>
            <description>显示信息</description>
        </element>

        <!-- 参数元素（type="parameter"，必须有 cfg-id） -->
        <element cfg-id="1" type="parameter">
            <name>&lt;as-number&gt;</name>
            <type>uint(1-4294967295)</type>
            <description>BGP AS 号</description>
        </element>
    </elements>

    <commands>
        <!-- 基本命令（支持 no 前缀时，框架自动设 CLI_PAYLOAD_FLAG_NO_CMD，无需在 XML 中定义 no 关键字） -->
        <command>
            <expression>1 2</expression>    <!-- 元素位置序号（1-based） -->
            <views>3</views>                <!-- CLI_VIEW_CONFIG = 3 -->
        </command>

        <!-- 带可选项命令 -->
        <command>
            <expression>1 [ 2 ]</expression>   <!-- [ ] = 可选, { | } = 必选之一 -->
            <views>3</views>
        </command>

        <!-- 切换视图命令 -->
        <command>
            <expression>1 2</expression>
            <views>3</views>
            <view-id>4</view-id>            <!-- 执行后切换到的视图 ID -->
        </command>
    </commands>
</group>
```

> **重要**：`no` 前缀由 CLI 框架统一处理，框架会设置 `CLI_PAYLOAD_FLAG_NO_CMD` 标志位，
> **不需要**在 XML 中定义 `no` 关键字元素，也不需要单独的 group。

### 参数类型速查

| XML type | 说明 |
|----------|------|
| `uint(1-4294967295)` | 无符号整数范围 |
| `int(-100-100)` | 有符号整数范围 |
| `string(1-63)` | 字符串长度限制 |
| `ip` | IPv4 地址 |
| `prefix` | IPv4 前缀（如 10.0.0.0/8） |
| `mac` | MAC 地址 |

### 视图 ID 速查（include/cli.h）

| 常量 | 值 | 说明 |
|------|---|------|
| `CLI_VIEW_GLOBAL` | 1 | 全局视图（登录前） |
| `CLI_VIEW_USER` | 2 | 用户视图 |
| `CLI_VIEW_CONFIG` | 3 | 配置视图 |
| `CLI_VIEW_BGP` | 4 | BGP 配置视图 |
| `CLI_VIEW_IF` | 5 | 接口配置视图 |
| `CLI_VIEW_ROUTE` | 6 | 路由视图 |
| `CLI_VIEW_BGP_AF_IPV4` | 7 | BGP AF IPv4 视图 |

### 多视图可用

```xml
<views>2,3</views>   <!-- 在用户视图和配置视图均可用 -->
```

---

## 第四步：添加 C 处理函数

在 `src/{module}/{module}_cli.c` 中添加静态处理函数。

### 模板 A：show 命令（从内存读取）

> **原则**：show 命令只读内存（`g_{module}_local->xxx`），不查 DB。
> DB 只用于持久化和启动恢复，运行时状态以内存为准。

```c
/**
 * @brief 处理 "show xxx" 命令（从内存读取，不查 DB）
 */
static int handle_show_xxx(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* 跳过所有 TLV（show 命令通常无需参数，或仅读取上下文） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    size_t offset = 0;

    if (!g_{module}_local->xxx)
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "No xxx configured.\r\n");
        {module}_send_cli_response(msg, resp_buf);
        return ERRCODE_SUCCESS;
    }

    /* 遍历内存结构直接输出 */
    CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\nXxx Information:\r\n");
    CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "============================\r\n");
    CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset,
                   "  %-20s: %u\r\n", "param", g_{module}_local->xxx->param);
    /* ... 更多字段 ... */

    {module}_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}
```

### 模板 B：配置命令（支持 no 前缀，含 DB 写入和内存赋值）

```c
/**
 * @brief 处理 "[no] xxx <param>" 命令
 *
 * group_id=N, cfg_id: 1=param
 */
static int handle_xxx_config(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* is_no 从 flags 读取，不依赖 XML 中的 no 关键字 */
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t param_value = 0;
    char str_param[64] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id))
        {
            /* 视图切换命令才需要读取模板，普通配置命令可跳过 */
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            /* 读取父视图传递的上下文参数（如 AS 号、接口名等） */
            if (CFG_TLV_CONTEXT_ID(entry.cfg_id) == 2)
            {
                param_value = (uint32_t)cli_tlv_entry_get_int(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* 整数参数 */
                param_value = (uint32_t)cli_tlv_entry_get_int(&entry);
                break;
            case 2: /* 字符串参数 */
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(str_param, sizeof(str_param), "%s", s);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    /* ---- 删除场景 ---- */
    if (is_no)
    {
        /* 1. 先更新内存 */
        if (g_{module}_local->xxx)
        {
            xxx_destroy(g_{module}_local->xxx);
            g_{module}_local->xxx = NULL;
        }

        /* 2. 再删除 DB */
        (void){module}_db_del_xxx(g_{module}_local->dev_ipc_ctx);

        {module}_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* ---- 配置场景 ---- */
    /* 1. 先写 DB（失败则不更新内存，保持一致性） */
    if ({module}_db_set_xxx(g_{module}_local->dev_ipc_ctx, param_value) != 0)
    {
        {module}_send_cli_response(msg, "Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    /* 2. 更新内存 */
    if (!g_{module}_local->xxx)
    {
        g_{module}_local->xxx = xxx_create(param_value);
    }
    else
    {
        g_{module}_local->xxx->param = param_value;
    }

    {module}_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}
```

### 模板 C：切换视图（进入子视图）

```c
/**
 * @brief 处理进入子视图的命令（如 "bgp <as-number>"）
 *
 * group_id=N, cfg_id: 1=key-param
 */
static int handle_enter_subview(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t key_id = 0;
    char view_template[CFG_CLI_MAX_VIEW_LEN] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_VIEW_TEMPLATE(entry.cfg_id))
        {
            const char *tmpl = cli_tlv_entry_get_text(&entry);
            if (tmpl)
            {
                snprintf(view_template, sizeof(view_template), "%s", tmpl);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            key_id = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        /* 删除整个子对象 */
        if (!g_{module}_local->xxx)
        {
            {module}_send_cli_response(msg, "");
            return ERRCODE_FAIL;
        }
        xxx_destroy(g_{module}_local->xxx);
        g_{module}_local->xxx = NULL;
        (void){module}_db_del_xxx(g_{module}_local->dev_ipc_ctx);
        {module}_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 创建对象（不存在时） */
    if (!g_{module}_local->xxx)
    {
        if ({module}_db_set_xxx(g_{module}_local->dev_ipc_ctx, key_id) != 0)
        {
            {module}_send_cli_response(msg, "Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
        g_{module}_local->xxx = xxx_create(key_id);
    }
    else if (g_{module}_local->xxx->key != key_id)
    {
        {module}_send_cli_response(msg, "Error: key mismatch.\r\n");
        return ERRCODE_FAIL;
    }

    /* 构建视图提示符和上下文 TLV */
    char out_prompt[CLI_CLI_MAX_PROMPT_LEN];
    snprintf(out_prompt, CLI_CLI_MAX_PROMPT_LEN, view_template, key_id);

    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u16(ctx_buf, 1);                          /* num_entries = 1 */
    ctx_write_u32(ctx_buf, 1);                          /* cfg_id（子命令用 CFG_TLV_CONTEXT_FLAG|1 读取） */
    ctx_write_u8(ctx_buf, (uint8_t)DB_TYPE_INTEGER);
    ctx_write_u16(ctx_buf, 8);
    ctx_write_i64(ctx_buf, (int64_t)key_id);

    {module}_send_view_change(msg, ctx_buf, out_prompt);
    g_byte_array_free(ctx_buf, TRUE);
    return ERRCODE_SUCCESS;
}
```

---

## 第五步：注册到 dispatch 函数

在模块的 CLI dispatch 函数（`{module}_cli_handle_message`）中添加新的 case：

```c
int {module}_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        {module}_send_cli_response(msg, "Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case {MODULE}_CLI_GROUP_ID_EXISTING:
            result = handle_existing(msg, &parser);
            break;
        case {MODULE}_CLI_GROUP_ID_NEW:         /* 新增 */
            result = handle_xxx_config(msg, &parser);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            {module}_send_cli_response(msg, "Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
```

---

## 第六步：添加 DB 操作函数（{module}_db.c/h）

### 在 {module}_db.h 中添加声明

```c
/** {MODULE} xxx 表名 */
#define {MODULE}_TABLE_XXX "{module}_xxx"

/**
 * @brief 插入或更新 xxx 配置
 * @param ctx   模块 IPC 上下文
 * @param param 关键参数
 * @return 0 成功，-1 失败
 */
int {module}_db_set_xxx(dev_ipc_context_t *ctx, uint32_t param);

/**
 * @brief 删除 xxx 配置
 * @param ctx 模块 IPC 上下文
 * @return 删除行数，-1 失败
 */
int {module}_db_del_xxx(dev_ipc_context_t *ctx);
```

### 在 {module}_db.c 中添加表定义和实现

```c
/* {module}_xxx 表列定义 */
static const db_column_def_t {MODULE}_XXX_COLS[] = {
    {"param", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
    {"str_field", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
};

/* {module}_xxx 表定义 */
static const db_table_def_t {MODULE}_XXX_TABLE = {
    .table_name = {MODULE}_TABLE_XXX,
    .cols = {MODULE}_XXX_COLS,
    .num_cols = G_N_ELEMENTS({MODULE}_XXX_COLS),
};

/* 在 {module}_db_init() 中注册建表 */
int {module}_db_init(dev_ipc_context_t *ctx)
{
    /* ... 已有建表 ... */
    int ret = db_rpc_create_table_from_def(ctx, &{MODULE}_XXX_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("{MODULE} 建表失败: %s", {MODULE}_TABLE_XXX);
        return -1;
    }
    return 0;
}

/* set：插入或更新（upsert） */
int {module}_db_set_xxx(dev_ipc_context_t *ctx, uint32_t param)
{
    if (!ctx)
    {
        return -1;
    }

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "param", (int64_t)param);
    /* db_record_set_text(rec, "str_field", str_val); */

    int ret = db_rpc_upsert(ctx, {MODULE}_TABLE_XXX, rec, NULL);
    db_record_free(rec);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("{MODULE} 写入 xxx param=%u 失败", param);
        return -1;
    }
    LOG_INFO("{MODULE} xxx param=%u 已写入", param);
    return 0;
}

/* del：删除全部行（按主键过滤则传 filter） */
int {module}_db_del_xxx(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return -1;
    }

    int rows = db_rpc_delete(ctx, {MODULE}_TABLE_XXX, NULL);
    if (rows < 0)
    {
        LOG_ERROR("{MODULE} 删除 xxx 失败");
        return -1;
    }
    LOG_INFO("{MODULE} 删除 xxx，影响行数: %d", rows);
    return rows;
}

/* 按主键过滤删除示例 */
int {module}_db_del_xxx_by_key(dev_ipc_context_t *ctx, uint32_t param)
{
    if (!ctx)
    {
        return -1;
    }

    db_condition_t cond = {
        .field_name = "param",
        .op = DB_CMP_EQ,
        .value = db_value_int((int64_t)param),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};

    int rows = db_rpc_delete(ctx, {MODULE}_TABLE_XXX, &filter);
    db_value_free(&cond.value);

    if (rows < 0)
    {
        LOG_ERROR("{MODULE} 删除 xxx param=%u 失败", param);
        return -1;
    }
    return rows;
}
```

---

## 第七步：添加启动恢复（{module}_db_restore）

在 `{module}_db.c` 中为新表添加恢复逻辑，在 MODULE_READY 阶段（`{module}_on_ready`）调用：

```c
/**
 * @brief 从 {module}_xxx 表恢复内存状态
 */
static void restore_xxx(dev_ipc_context_t *ctx)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, {MODULE}_TABLE_XXX, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        uint32_t param = (uint32_t)db_row_get_int(row, "param", 0);
        /* const char *str_val = db_row_get_text(row, "str_field", NULL); */

        if (param == 0)
        {
            continue;
        }

        /* 恢复内存对象 */
        xxx_t *obj = xxx_create(param);
        /* obj->str_field = g_strdup(str_val); */
        g_{module}_local->xxx = obj;  /* 或加入 hash/list */

        LOG_INFO("{MODULE} 恢复 xxx: param=%u", param);
    }

    db_result_free(result);
}

/* 在总恢复函数中调用（各表独立恢复，互不依赖） */
void {module}_db_restore(dev_ipc_context_t *ctx)
{
    restore_xxx(ctx);
    /* restore_yyy(ctx); */
}
```

在 `{module}_on_ready` 中调用：

```c
static void {module}_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if ({module}_db_init(ctx) != 0)
    {
        LOG_WARN("{MODULE} 数据库初始化失败，继续启动");
    }
    {module}_db_restore(ctx);   /* 从 DB 恢复内存状态 */
    /* ... 其他初始化 ... */
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}
```

---

## 第八步：添加 BDR（show current-configuration）

BDR（Builder）负责从 DB 读取配置并输出可重放的 CLI 命令文本，响应 `CFG_MSG_TYPE_SHOW_CONFIG` 消息。

### 在 {module}_bdr.h 中声明

```c
/**
 * @brief 处理 show current-configuration 请求，输出模块配置文本
 * @param msg 原始消息（来自 CFG 模块）
 */
void {module}_bdr_show_config(dev_ipc_message_t *msg);
```

### 在 {module}_bdr.c 中实现

```c
/**
 * @file   {module}_bdr.c
 * @brief  {MODULE} 配置构建器：读取 DB 并生成 show current-configuration 输出
 */
#include "{module}_bdr.h"
#include "{module}_db.h"
#include "{module}_main.h"
#include "cli.h"
#include "dev.h"
#include "log.h"

static void send_config_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(
        CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_{MODULE}, msg->src_module_id,
        msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_{module}_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

/**
 * @brief 追加 xxx 配置行（从 DB 查询）
 * @return TRUE 表示有配置，FALSE 表示无配置
 */
static gboolean bdr_append_xxx(dev_ipc_context_t *ctx, char *buf, size_t buf_size, size_t *off)
{
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, {MODULE}_TABLE_XXX, NULL, 0, NULL, &result) != ERRCODE_SUCCESS ||
        !result || result->num_rows == 0)
    {
        if (result) db_result_free(result);
        return FALSE;
    }

    db_row_t *row = result->rows[0];
    uint32_t param = (uint32_t)db_row_get_int(row, "param", 0);

    CLI_BUF_APPEND(buf, buf_size, *off, "!\r\n");
    CLI_BUF_APPEND(buf, buf_size, *off, "xxx %u\r\n", param);
    /* 子配置（如 session、neighbor 等）继续追加 */

    db_result_free(result);
    return TRUE;
}

void {module}_bdr_show_config(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = g_{module}_local->dev_ipc_ctx;
    char buf[CLI_MAX_RESP_LEN];
    size_t off = 0;

    if (!bdr_append_xxx(ctx, buf, sizeof(buf), &off))
    {
        /* 模块无配置时返回空字符串，CFG 会跳过该模块 */
        send_config_resp(msg, "");
        return;
    }

    /* 追加子配置（session、AF 邻居等） */
    /* bdr_append_yyy(ctx, buf, sizeof(buf), &off); */

    CLI_BUF_APPEND(buf, sizeof(buf), off, "!\r\n");
    send_config_resp(msg, buf);
}
```

### 在 {module}_main.c 的消息处理中注册 BDR

```c
void {module}_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        /* ... 已有 case ... */
        case CFG_MSG_TYPE_SHOW_CONFIG:
            {module}_bdr_show_config(msg);
            return;
        default:
            break;
    }
    dev_ipc_message_free(msg);
}
```

---

## 第九步：定义 group-id 常量

在 `src/{module}/{module}_cli.h` 中定义常量，方便维护：

```c
/* CLI 命令组 ID 定义 */
#define {MODULE}_CLI_GROUP_ID_PROTOCOL    1
#define {MODULE}_CLI_GROUP_ID_SHOW        2
#define {MODULE}_CLI_GROUP_ID_NEIGHBOR    3
#define {MODULE}_CLI_GROUP_ID_XXX         N   /* 新增 */
```

---

## 第十步：构建并测试

```bash
./scripts/dev/build.sh
./scripts/dev/start.sh

# 连接测试
telnet localhost 3788

# 测试配置命令
> enable
# config t
(config)# your-new-command arg1
(config)# no your-new-command arg1

# 测试 show（应从内存读取）
(config)# show xxx

# 测试恢复（重启后配置应自动还原）
# Ctrl+C 停止服务，重新 start，验证内存已从 DB 恢复

# 测试 show current-configuration
(config)# show current-configuration

# 测试 Tab 补全和帮助
(config)# ?
(config)# your-new-  <Tab>
```

---

## 常见问题

**Q: is_no 始终为 FALSE？**
- 确认使用 `parser->flags & CLI_PAYLOAD_FLAG_NO_CMD`，不要在 XML 中定义 no 关键字。

**Q: 命令不出现在 tab 补全中？**
- 检查 `<views>` 中的视图 ID 是否与当前视图匹配。
- 检查 XML 格式是否正确（特别是 `&lt;` `&gt;` 转义）。

**Q: 处理函数收不到参数？**
- 确认参数元素有 `cfg-id` 属性（关键字通常不需要）。
- 确认 `switch(entry.cfg_id)` 中的 case 值与 XML 中的 `cfg-id` 一致。

**Q: 上下文参数读取失败？**
- 检查 `CFG_TLV_CONTEXT_ID(entry.cfg_id) == N` 中的 N 是否与父视图 context TLV 写入的 cfg_id 一致。
- 用 `CFG_TLV_IS_CONTEXT(entry.cfg_id)` 过滤上下文条目。

**Q: 视图切换后提示符不正确？**
- 检查 XML 中 `<view>` 的 `<template>` 是否正确定义（`%u` 用于整数，`%s` 用于字符串）。
- 确认 `view-id` 与 `include/cli.h` 中的视图 ID 一致。

**Q: 重启后配置没有恢复？**
- 检查 `{module}_db_restore()` 是否在 `MODULE_READY` 阶段被调用。
- 检查 restore 函数是否正确遍历了所有表并赋值到内存对象。

**Q: show current-configuration 没有输出该模块的配置？**
- 检查 `CFG_MSG_TYPE_SHOW_CONFIG` 消息是否在 `{module}_msg_handler` 中被处理。
- 检查 `bdr_append_xxx` 的 DB 查询是否正确，表名是否匹配。
