# 添加 CLI 命令

为 CNetNexus 模块添加新 CLI 命令（XML 定义 + C 处理函数 + DB 操作 + 内存恢复 + BDR）。

## 使用方式

`/add-cli-command <模块名> <命令描述>`

例如：`/add-cli-command bgp show bgp peer detail`

---

## 第一步：分析需求

在开始之前，明确以下信息：

1. **目标模块**：命令属于哪个模块（bgp / if / route / {module} 等）
2. **可用视图**：命令在哪个 CLI 视图下可用（参考 `include/cli.h` 的 `CLI_VIEW_*` 字符串常量）
3. **命令语法**：完整命令语法，包括关键字和参数
4. **参数类型**：每个参数的类型（`uint`、`string`、`ip` 等）
5. **是否切换视图**：执行后是否进入子视图（如 `bgp 65000` 后进入 BGP 视图）
6. **命令类型**：show（只读）/ 配置（读写，含 `no` 形式）/ 进入视图

---

## 第二步：查阅现有定义

先读取目标模块的现有文件，了解已有的 group-id、cfg-id 范围：

```
读取 src/{module}/resources/commands.xml
读取 src/{module}/{module}_cli.c
读取 src/{module}/{module}_cli.h   (group-id 常量)
读取 src/{module}/{module}_db.c/h  (表定义和 DB 操作，如有)
读取 src/{module}/{module}_bdr.c/h (show current-configuration，如有)
读取 include/cli.h  (视图名称常量 CLI_VIEW_*)
```

---

## 第三步：定义 XML（commands.xml）

在 `src/{module}/resources/commands.xml` 的 `<command_groups>` 中添加新的 `<group>`。

### 视图名称速查（`include/cli.h`）

视图使用字符串名称，不是数字：

| 常量 | XML 字符串 | 说明 |
|------|-----------|------|
| `CLI_VIEW_GLOBAL` | `global` | 全局视图（所有视图均可用） |
| `CLI_VIEW_USER` | `user` | 用户视图 |
| `CLI_VIEW_CONFIG` | `config` | 配置视图 |
| `CLI_VIEW_BGP` | `bgp` | BGP 配置视图 |
| `CLI_VIEW_IF` | `if` | 接口配置视图 |
| `CLI_VIEW_ROUTE` | `route` | 路由视图 |
| `CLI_VIEW_BGP_AF_IPV4` | `bgp-af-ipv4-uni` | BGP AF IPv4 视图 |
| `CLI_VIEW_BGP_AF_IPV6` | `bgp-af-ipv6-uni` | BGP AF IPv6 视图 |
| `CLI_VIEW_VRF` | `vrf` | VRF 配置视图 |

多视图可用：`<views>user,config</views>`

### XML 结构模板

```xml
<group group-id="N">
    <elements>
        <!-- 关键字：无 cfg-id，仅作为匹配路径节点 -->
        <element type="keyword">
            <name>show</name>
            <description>显示信息</description>
        </element>

        <!-- 带 cfg-id 的关键字：用于在处理函数中区分分支 -->
        <element cfg-id="1" type="keyword">
            <name>bgp</name>
            <description>BGP 路由</description>
        </element>

        <!-- 参数元素：必须有 cfg-id，处理函数通过 cfg_id 读取值 -->
        <element cfg-id="2" type="parameter">
            <name>&lt;as-number&gt;</name>
            <type>uint(1-4294967295)</type>
            <description>BGP AS 号</description>
        </element>

        <!-- no 关键字（配置命令的删除形式，必须显式定义） -->
        <element cfg-id="10" type="keyword">
            <name>no</name>
            <description>删除配置</description>
        </element>
    </elements>

    <commands>
        <!-- show 命令 -->
        <command>
            <expression>1 2</expression>    <!-- 元素位置序号（1-based，按 elements 列表顺序） -->
            <views>global</views>
        </command>

        <!-- 配置命令 -->
        <command>
            <expression>1 2 3</expression>
            <views>config</views>
        </command>

        <!-- no 形式（配置命令的删除形式） -->
        <command>
            <expression>4 1 2</expression>  <!-- no + 原命令 -->
            <views>config</views>
        </command>

        <!-- 切换视图命令 -->
        <command>
            <expression>1 2</expression>
            <views>config</views>
            <view-id>bgp</view-id>          <!-- 执行后切换到的视图名称 -->
        </command>
    </commands>
</group>
```

### 参数类型速查

| XML type | 说明 |
|----------|------|
| `uint(1-4294967295)` | 无符号整数范围 |
| `int(-100-100)` | 有符号整数范围 |
| `string(1-63)` | 字符串长度限制 |
| `string` | 任意字符串 |
| `ip` | IPv4 地址 |
| `prefix` | IPv4 前缀（如 10.0.0.0/8） |

### 关键规则

1. **元素位置序号**：`<expression>` 中的数字是元素在 `<elements>` 列表中的 1-based 位置，与 `cfg-id` 无关
2. **cfg-id**：处理函数读取值时使用，关键字不需要 cfg-id（纯路径节点），有值或需要区分分支时才加
3. **no 关键字**：必须在 XML 中显式定义为元素（cfg-id 自定），处理函数通过判断该 cfg-id 是否出现来识别删除操作
4. **最后一个元素**自动标记为 `is_end_node`，即命令的有效结束点

---

## 第四步：添加 C 处理函数

在 `src/{module}/{module}_cli.c` 中添加静态处理函数。

### 辅助函数（参考已有模块的 send_resp 实现）

```c
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
```

### 模板 A：show 命令（从内存读取）

> **原则**：show 命令只读内存（`g_{module}_local->xxx`），不查 DB。
> DB 只用于持久化和启动恢复，运行时状态以内存为准。

```c
/**
 * @brief 处理 "show xxx" 命令（从内存读取）
 */
static int handle_show_xxx(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* 跳过所有 TLV 条目（含上下文条目） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        /* 如需读取过滤参数，在此处理 entry.cfg_id */
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    /* 从内存构造输出 */
    g_string_append_printf(buf, "\r\nXxx Information:\r\n");
    g_string_append_printf(buf, "  %-20s: %u\r\n", "param", g_{module}_local->xxx->param);

    return cli_chunk_stream_start(&g_{module}_local->show_stream,
                                  g_{module}_local->dev_ipc_ctx,
                                  DEV_MODULE_ID_{MODULE}, msg, buf);
}
```

### 模板 B：配置命令（含 no 形式，含 DB 写入）

```c
/**
 * @brief 处理 "[no] xxx <param>" 命令
 *
 * group_id=N
 * cfg-id 映射: 1=xxx关键字, 2=param参数, 10=no关键字
 */
static int handle_xxx_config(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    uint32_t param_value = 0;
    int has_param = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            /* 上下文条目：读取父视图传递的参数（如 AS 号、VRF ID 等） */
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 10:
                is_no = TRUE;
                break;
            case 2:
                param_value = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_param = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    /* ---- 删除场景 ---- */
    if (is_no)
    {
        /* 1. 更新内存 */
        if (g_{module}_local->xxx)
        {
            xxx_destroy(g_{module}_local->xxx);
            g_{module}_local->xxx = NULL;
        }

        /* 2. 删除 DB */
        (void){module}_db_del_xxx(g_{module}_local->dev_ipc_ctx);

        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* ---- 配置场景 ---- */
    if (!has_param)
    {
        send_resp(msg, "Error: Missing parameter\r\n");
        return ERRCODE_FAIL;
    }

    /* 1. 先写 DB（失败则不更新内存，保持一致性） */
    if ({module}_db_set_xxx(g_{module}_local->dev_ipc_ctx, param_value) != 0)
    {
        send_resp(msg, "Error: Database write failed\r\n");
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

    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}
```

---

## 第五步：注册到 dispatch 函数

在模块的 `{module}_cli_handle_message` 中添加新 case：

```c
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
        LOG_ERROR("{MODULE} payload 解析失败");
        send_resp(msg, "Error: Command payload parsing failed\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("{MODULE} CLI TLV (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case {MODULE}_CLI_GROUP_ID_EXISTING:
            result = handle_existing(msg, &parser);
            break;
        case {MODULE}_CLI_GROUP_ID_NEW:     /* 新增 */
            result = handle_xxx_config(msg, &parser);
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
```

---

## 第六步：添加 DB 操作函数（{module}_db.c/h）

### 在 {module}_db.h 中添加声明

```c
/** {MODULE} xxx 表名 */
#define {MODULE}_TABLE_XXX "{module}_xxx"

/**
 * @brief 插入或更新 xxx 配置
 */
int {module}_db_set_xxx(dev_ipc_context_t *ctx, uint32_t param);

/**
 * @brief 删除 xxx 配置
 */
int {module}_db_del_xxx(dev_ipc_context_t *ctx);
```

### 在 {module}_db.c 中添加表定义和实现

```c
/* {module}_xxx 表列定义 */
static const db_column_def_t {MODULE}_XXX_COLS[] = {
    {"param",     DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY | DB_COL_NOT_NULL, NULL},
    {"str_field", DB_TYPE_TEXT,    DB_COL_NOT_NULL,                      NULL},
};

static const db_table_def_t {MODULE}_XXX_TABLE = {
    .table_name = {MODULE}_TABLE_XXX,
    .cols       = {MODULE}_XXX_COLS,
    .num_cols   = G_N_ELEMENTS({MODULE}_XXX_COLS),
};

/* 在 {module}_on_ready 中调用建表 */
int {module}_db_init(dev_ipc_context_t *ctx)
{
    return db_rpc_create_table_from_def(ctx, &{MODULE}_XXX_TABLE);
}

int {module}_db_set_xxx(dev_ipc_context_t *ctx, uint32_t param)
{
    if (!ctx)
    {
        return -1;
    }

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "param", (int64_t)param);

    db_condition_t cond = {"param", DB_CMP_EQ, db_value_int((int64_t)param)};
    db_filter_t filter = {&cond, 1};

    int ret = db_rpc_upsert(ctx, {MODULE}_TABLE_XXX, rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);

    return (ret == ERRCODE_SUCCESS) ? 0 : -1;
}

int {module}_db_del_xxx(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return -1;
    }
    return db_rpc_delete(ctx, {MODULE}_TABLE_XXX, NULL);
}
```

---

## 第七步：添加启动恢复（MODULE_READY 阶段）

```c
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
        if (param == 0)
        {
            continue;
        }

        /* 恢复内存对象 */
        g_{module}_local->xxx = xxx_create(param);
        LOG_INFO("{MODULE} 恢复 xxx: param=%u", param);
    }

    db_result_free(result);
}

/* 在 {module}_on_ready 中调用 */
static void {module}_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    {module}_db_init(ctx);
    restore_xxx(ctx);
    send_phase_response(ctx, msg);
}
```

---

## 第八步：添加 BDR（show current-configuration）

BDR 负责从 DB 读取配置并输出可重放的 CLI 命令文本，响应 `CLI_MSG_TYPE_SHOW_CONFIG` 消息。

### 在 {module}_bdr.h 中声明

```c
/**
 * @brief 处理 show current-configuration 请求
 */
void {module}_bdr_show_config(dev_ipc_message_t *msg);
```

### 在 {module}_bdr.c 中实现

```c
#include "{module}_bdr.h"
#include "{module}_main.h"
#include "cli.h"
#include "dev.h"
#include "log.h"

static void send_config_resp(dev_ipc_message_t *msg, const char *text)
{
    char *payload = g_strdup(text ? text : "");
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

void {module}_bdr_show_config(dev_ipc_message_t *msg)
{
    GString *out = g_string_new("");

    /* TODO: 从 DB 读取并生成可重放 CLI 命令 */
    db_result_t *result = NULL;
    if (db_rpc_query(g_{module}_local->dev_ipc_ctx, {MODULE}_TABLE_XXX, NULL, 0, NULL, &result)
        == ERRCODE_SUCCESS && result && result->num_rows > 0)
    {
        /* 按行生成配置命令 */
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            db_row_t *row = result->rows[i];
            uint32_t param = (uint32_t)db_row_get_int(row, "param", 0);
            g_string_append_printf(out, "!\r\nxxx %u\r\n", param);
        }
        if (out->len > 0)
        {
            g_string_append(out, "!\r\n");
        }
        db_result_free(result);
    }

    send_config_resp(msg, out->str);
    g_string_free(out, TRUE);
}
```

### 在 {module}_main.c 的消息路由中注册

```c
case CLI_MSG_TYPE_SHOW_CONFIG:
    {module}_bdr_show_config(msg);
    return;
```

---

## 第九步：定义 group-id 常量

在 `src/{module}/{module}_cli.h` 中定义常量，方便维护：

```c
/** CLI 命令组 ID 定义（与 commands.xml 中 group-id 一致） */
#define {MODULE}_CLI_GROUP_ID_SHOW        1
#define {MODULE}_CLI_GROUP_ID_CONFIG      2
#define {MODULE}_CLI_GROUP_ID_NEW         N   /* 新增 */
```

---

## 第十步：构建并测试

```bash
./scripts/dev/build.sh
./scripts/dev/start.sh

telnet localhost 3788

# 测试配置命令
(config)# xxx 100
(config)# no xxx 100

# 测试 show（从内存读取）
> show xxx

# 测试重启恢复（配置 → 重启 → 确认内存已恢复）

# 测试 show current-configuration
(config)# show current-configuration

# 测试 Tab 补全
(config)# xxx <Tab>
```

---

## 常见问题

**Q: no 命令不生效？**
- 确认 XML 中已定义 `no` 关键字元素（有 cfg-id），并在 expression 中包含它
- 确认处理函数通过判断 `entry.cfg_id == no_cfg_id` 来检测删除操作

**Q: 命令不出现在 tab 补全中？**
- 检查 `<views>` 中的视图名称是否与当前视图匹配（用字符串，如 `config`）
- 检查 XML 格式（特别是 `&lt;` `&gt;` 转义）

**Q: 处理函数收不到参数？**
- 确认参数元素有 `cfg-id` 属性
- 确认 `switch(entry.cfg_id)` 的 case 值与 XML 中的 `cfg-id` 一致
- 注意跳过 `CLI_TLV_IS_CTX(&entry)` 的上下文条目

**Q: 重启后配置没有恢复？**
- 检查 restore 函数是否在 `MODULE_READY` 阶段（`{module}_on_ready`）被调用
- 检查 DB 表名是否与 `db_rpc_query` 中的表名一致

**Q: show current-configuration 没有该模块的输出？**
- 检查 `CLI_MSG_TYPE_SHOW_CONFIG` 是否在 `{module}_ipc_msg_handler` 中处理
- 检查 `send_config_resp` 是否发送了非空内容（空字符串会被 CFG 跳过）
