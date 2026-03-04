# 添加 CLI 命令

为 CNetNexus 模块添加新 CLI 命令（XML 定义 + C 处理函数）。

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

---

## 第二步：查阅现有定义

先读取目标模块的现有文件，了解已有的 group-id、cfg-id 范围：

```
读取 src/{module}/resources/commands.xml
读取 src/{module}/{module}_cli.c
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

        <!-- 可选关键字也用 cfg-id 标识，用于 is_no 检测 -->
        <element cfg-id="2" type="keyword">
            <name>no</name>
            <description>删除配置</description>
        </element>
    </elements>

    <commands>
        <!-- 基本命令 -->
        <command>
            <expression>1 2</expression>    <!-- 元素位置序号（1-based） -->
            <views>3</views>                <!-- CLI_VIEW_CONFIG = 3 -->
        </command>

        <!-- 带可选项命令 -->
        <command>
            <expression>[ 3 ] 1 2</expression>   <!-- [ ] = 可选, { | } = 必选之一 -->
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

在 `src/{module}/{module}_cli.c` 中添加静态处理函数：

### 模板 A：简单响应（show 命令）

```c
/**
 * @brief 处理 "show xxx" 命令
 */
static int handle_show_xxx(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    cli_tlv_entry_t entry;
    /* 通常 show 命令不需要解析参数，直接输出 */

    /* 若有参数，遍历 TLV */
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            /* 跳过上下文 TLV（来自父视图的保存状态） */
            cli_tlv_entry_free(&entry);
            continue;
        }
        /* 处理参数 */
        cli_tlv_entry_free(&entry);
    }

    /* 构造输出 */
    GString *output = g_string_new("");
    g_string_append_printf(output, "%-20s %s\r\n", "Field", "Value");
    /* ... 填充数据 ... */

    {MODULE}_send_cli_response(msg, output->str);
    g_string_free(output, TRUE);
    return ERRCODE_SUCCESS;
}
```

### 模板 B：配置命令（支持 no 前缀）

```c
/**
 * @brief 处理 "[no] xxx <param>" 命令
 */
static int handle_xxx_config(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    uint32_t param_value = 0;
    char str_param[64] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            /* 读取上下文中的父视图参数（如 AS 号） */
            if (entry.cfg_id == (CFG_TLV_CONTEXT_FLAG | 2))
            {
                param_value = (uint32_t)cli_tlv_entry_get_int(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* "no" 关键字的 cfg-id */
                is_no = TRUE;
                break;
            case 2: /* 整数参数 */
                param_value = (uint32_t)cli_tlv_entry_get_int(&entry);
                break;
            case 3: /* 字符串参数 */
                snprintf(str_param, sizeof(str_param), "%s",
                         cli_tlv_entry_get_str(&entry));
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        /* 删除操作 */
        /* ... */
        {MODULE}_send_cli_response(msg, "");
    }
    else
    {
        /* 创建/修改操作 */
        /* ... */
        {MODULE}_send_cli_response(msg, "");
    }

    return ERRCODE_SUCCESS;
}
```

### 模板 C：切换视图（进入子视图）

```c
/**
 * @brief 处理进入子视图的命令（如 "bgp <as-number>"）
 */
static int handle_enter_subview(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t key_id = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1) /* 关键参数 cfg-id */
        {
            key_id = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    /* 验证对象是否存在 */
    if (!xxx_find(key_id))
    {
        {MODULE}_send_cli_response(msg, "Error: object not found\r\n");
        return ERRCODE_FAIL;
    }

    /* 构建视图提示符（含动态参数） */
    char prompt[128];
    snprintf(prompt, sizeof(prompt), "<NetNexus(config-xxx-%u)>", key_id);

    /* 将关键参数写入上下文 TLV，子视图命令可读取 */
    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u32(ctx_buf, 1);        /* cfg_id（与子命令 CONTEXT_FLAG|cfg_id 对应） */
    ctx_write_u32(ctx_buf, key_id);   /* 值 */

    {MODULE}_send_view_change(msg, ctx_buf, prompt);
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
    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, msg->payload, msg->payload_len) != 0)
    {
        return ERRCODE_FAIL;
    }

    switch (parser.group_id)
    {
        case 1:  /* 已有的 group */
            return handle_existing(msg, &parser);

        case N:  /* 新增的 group-id */
            return handle_xxx_config(msg, &parser);  /* 新函数 */

        default:
            {MODULE}_send_cli_response(msg, "Unknown command\r\n");
            return ERRCODE_FAIL;
    }
}
```

---

## 第六步：定义 group-id 常量（可选）

在 `src/{module}/{module}_cli.h` 中定义常量，方便维护：

```c
/* CLI 命令组 ID 定义 */
#define {MODULE}_CLI_GROUP_ID_PROTOCOL    1
#define {MODULE}_CLI_GROUP_ID_NEIGHBOR    2
#define {MODULE}_CLI_GROUP_ID_XXX         N   /* 新增 */
```

---

## 第七步：构建并测试

```bash
./scripts/dev/build.sh
./scripts/dev/start.sh

# 连接测试
telnet localhost 3788

# 测试新命令
> enable
# config t
(config)# your-new-command arg1
(config)# ?           # 查看帮助
(config)# your-new-  # Tab 补全测试
```

---

## 常见问题

**Q: 命令不出现在 tab 补全中？**
- 检查 `<views>` 中的视图 ID 是否与当前视图匹配
- 检查 XML 格式是否正确（特别是 `&lt;` `&gt;` 转义）

**Q: 处理函数收不到参数？**
- 确认参数元素有 `cfg-id` 属性（关键字通常不需要）
- 确认 `switch(entry.cfg_id)` 中的 case 值与 XML 中的 `cfg-id` 一致

**Q: 上下文参数读取失败？**
- 检查 `entry.cfg_id == (CFG_TLV_CONTEXT_FLAG | N)` 中的 N 是否与父视图 context TLV 写入的 cfg_id 一致
- 用 `CFG_TLV_IS_CONTEXT(entry.cfg_id)` 过滤上下文条目

**Q: 视图切换后提示符不正确？**
- 检查 XML 中 `<view>` 的 `<template>` 是否正确定义（`%u` 用于整数，`%s` 用于字符串）
- 确认 `view-id` 与 `include/cli.h` 中的视图 ID 一致
