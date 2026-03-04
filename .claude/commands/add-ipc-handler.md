# 添加 IPC 消息类型和处理函数

在两个模块之间添加一个新的 IPC 消息类型（RPC 调用或单向通知）。

## 使用方式

`/add-ipc-handler <发送方模块> <接收方模块> <功能描述>`

例如：`/add-ipc-handler bgp route 通知路由表更新`
例如：`/add-ipc-handler if db 查询接口配置`

---

## 第一步：确定通信模式

先明确需要哪种模式：

| 模式 | 场景 | API |
|------|------|-----|
| **同步 RPC**（请求-响应） | 需要等待结果（如查询数据库、获取配置） | `dev_ipc_query()` |
| **单向通知** | 不需要等待响应（如状态变更通知、事件广播） | `dev_ipc_send()` |

---

## 第二步：查阅现有定义

读取以下文件，确认现有消息类型，避免冲突：

```
include/dev.h          — DEV_IPC_MSG_TYPE_* 和大类定义
src/{receiver}/{receiver}_main.c  — 接收方的 msg_handler switch
```

---

## 第三步：在 `include/dev.h` 添加消息类型

消息类型编码规则：`msg_type = (大类 << 16) | 子类`

```c
/* {RECEIVER} 模块消息（大类 = 0x000X，X 为接收方模块序号） */
/* 在现有 {RECEIVER} 模块消息定义块中追加 */
#define {RECEIVER}_MSG_TYPE_{FEATURE}_REQ   DEV_IPC_MSG_TYPE(0x000X, 0x0001)
#define {RECEIVER}_MSG_TYPE_{FEATURE}_RESP  DEV_IPC_MSG_TYPE(0x000X, 0x00FF)
/* 单向通知（无需 RESP）：*/
#define {RECEIVER}_MSG_TYPE_{FEATURE}_NOTIFY DEV_IPC_MSG_TYPE(0x000X, 0x0001)
```

**注意：** 大类和子类的值必须唯一，不能与同一接收方现有消息冲突。

---

## 第四步：在接收方添加处理函数

**在 `src/{receiver}/{receiver}_main.c` 中：**

### 模式 A：同步 RPC 处理函数

```c
/**
 * @brief 处理来自 {sender} 的 {feature} 请求
 */
static void handle_{feature}_req(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    /* 1. 解析请求 payload */
    if (!msg->payload || msg->payload_len < sizeof(uint32_t))
    {
        LOG_ERROR("{feature}: payload 无效");
        /* 发送错误响应 */
        uint32_t err = (uint32_t)ERRCODE_FAIL;
        dev_ipc_message_t *resp = dev_ipc_message_create(
            {RECEIVER}_MSG_TYPE_{FEATURE}_RESP,
            DEV_MODULE_ID_{RECEIVER},
            msg->src_module_id,
            msg->request_id,
            g_memdup2(&err, sizeof(err)),
            sizeof(err),
            g_free
        );
        dev_ipc_send_response(ctx, resp);
        return;
    }

    /* 示例：payload 为 uint32_t 参数 */
    uint32_t param;
    memcpy(&param, msg->payload, sizeof(uint32_t));
    LOG_DEBUG("{feature}: 收到请求, param=%u", param);

    /* 2. 执行业务逻辑 */
    /* TODO: 实际处理代码 */
    uint32_t result = param * 2;  /* 示例 */

    /* 3. 发送响应 */
    dev_ipc_message_t *resp = dev_ipc_message_create(
        {RECEIVER}_MSG_TYPE_{FEATURE}_RESP,
        DEV_MODULE_ID_{RECEIVER},
        msg->src_module_id,
        msg->request_id,
        g_memdup2(&result, sizeof(result)),
        sizeof(result),
        g_free
    );
    dev_ipc_send_response(ctx, resp);
}
```

### 模式 B：单向通知处理函数

```c
/**
 * @brief 处理来自 {sender} 的 {feature} 通知
 */
static void handle_{feature}_notify(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len == 0)
    {
        LOG_WARN("{feature} 通知: payload 为空");
        return;
    }

    /* 解析通知内容 */
    /* TODO: 按实际 payload 格式解析 */
    LOG_INFO("{feature} 通知已收到，长度=%u", msg->payload_len);

    /* 执行响应逻辑 */
    /* TODO: 实际处理代码 */
}
```

**在接收方的 `msg_handler` switch 中注册：**

```c
void {receiver}_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        /* 原有 case ... */

        case {RECEIVER}_MSG_TYPE_{FEATURE}_REQ:      /* RPC 模式 */
            handle_{feature}_req(ctx, msg);
            return;                                   /* 注意：handler 负责释放 msg */

        case {RECEIVER}_MSG_TYPE_{FEATURE}_NOTIFY:   /* 通知模式 */
            handle_{feature}_notify(ctx, msg);
            break;                                    /* 让外层统一 free */

        default:
            LOG_WARN("未知消息类型: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}
```

**注意：**
- RPC handler 内部调用 `dev_ipc_send_response()` 后 **不** 调用 `dev_ipc_message_free(msg)`（send_response 内部会处理）
- 通知 handler 使用 `break`，由外层 `dev_ipc_message_free(msg)` 释放

---

## 第五步：在发送方添加调用函数

创建一个封装好的 API 函数，放在 `src/{sender}/{sender}_xxx.c` 中：

### 模式 A：同步 RPC 调用

```c
/**
 * @brief 向 {receiver} 发起 {feature} RPC 请求
 * @param ctx   本模块的 IPC 上下文
 * @param param 请求参数
 * @param result 输出：响应结果
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int {sender}_{feature}_rpc(dev_ipc_context_t *ctx, uint32_t param, uint32_t *result)
{
    if (!ctx || !result)
        return ERRCODE_FAIL;

    /* 1. 打包请求 payload */
    uint32_t payload = param;

    /* 2. 创建请求消息 */
    dev_ipc_message_t *req = dev_ipc_message_create(
        {RECEIVER}_MSG_TYPE_{FEATURE}_REQ,
        DEV_MODULE_ID_{SENDER},
        DEV_MODULE_ID_{RECEIVER},
        0,                          /* request_id 由 query 自动分配 */
        g_memdup2(&payload, sizeof(payload)),
        sizeof(payload),
        g_free
    );
    if (!req)
        return ERRCODE_FAIL;

    /* 3. 同步 RPC（阻塞，默认 5000ms 超时） */
    dev_ipc_message_t *resp = dev_ipc_query(
        ctx,
        DEV_MODULE_ID_{RECEIVER},
        req,
        0                           /* 0 = 使用默认超时 5000ms */
    );
    dev_ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("{feature} RPC 超时或失败");
        return ERRCODE_FAIL;
    }

    /* 4. 解析响应 */
    if (resp->payload && resp->payload_len >= sizeof(uint32_t))
    {
        memcpy(result, resp->payload, sizeof(uint32_t));
    }

    dev_ipc_message_free(resp);
    return ERRCODE_SUCCESS;
}
```

### 模式 B：单向通知发送

```c
/**
 * @brief 向 {receiver} 发送 {feature} 通知（不等待响应）
 * @param ctx   本模块的 IPC 上下文
 * @param data  通知数据
 * @param len   数据长度
 */
void {sender}_{feature}_notify(dev_ipc_context_t *ctx, const void *data, size_t len)
{
    if (!ctx)
        return;

    void *payload = len > 0 ? g_memdup2(data, len) : NULL;

    dev_ipc_message_t *msg = dev_ipc_message_create(
        {RECEIVER}_MSG_TYPE_{FEATURE}_NOTIFY,
        DEV_MODULE_ID_{SENDER},
        DEV_MODULE_ID_{RECEIVER},
        0,
        payload,
        len,
        payload ? g_free : NULL
    );
    if (!msg)
        return;

    dev_ipc_send(ctx, DEV_MODULE_ID_{RECEIVER}, msg);
    /* 注意：dev_ipc_send 内部不释放 msg，需要调用方释放 */
    dev_ipc_message_free(msg);
}
```

---

## 第六步：确保 Phase 1 已建立连接

在发送方的 `{sender}_on_start()` 中，确认已连接到接收方：

```c
static void {sender}_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    /* 确认以下连接已存在（如没有则添加） */
    dev_ipc_connect(ctx, DEV_MODULE_ID_{RECEIVER}, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_{RECEIVER});

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}
```

---

## 第七步：声明到头文件（可选）

如果调用函数需要跨文件使用，在 `src/{sender}/{sender}_xxx.h` 中声明：

```c
/**
 * @brief 向 {receiver} 发起 {feature} RPC 请求
 * @param ctx   IPC 上下文
 * @param param 请求参数
 * @param result 输出结果
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int {sender}_{feature}_rpc(dev_ipc_context_t *ctx, uint32_t param, uint32_t *result);
```

**注意：** 跨模块调用必须通过 IPC，不能直接 `#include` 对方模块的头文件。

---

## 第八步：构建验证

```bash
./scripts/dev/build.sh

# 运行并测试
./scripts/dev/start.sh

# 观察日志中的 IPC 消息
# 在合适时机触发调用，确认：
# 1. 发送方日志：显示发送请求
# 2. 接收方日志：显示收到请求并处理
# 3. 发送方日志（RPC 模式）：显示收到响应
```

---

## 常见错误

**错误：`dev_ipc_query` 超时**
- 检查接收方 `msg_handler` 的 switch 中是否有对应 case
- 检查接收方是否正确调用了 `dev_ipc_send_response()`（而非 `dev_ipc_send()`）
- 确认 `request_id` 未被修改（resp 的 request_id 必须与 req 一致）

**错误：接收方收不到消息**
- 确认发送方 Phase 1 中已调用 `dev_ipc_connect()` 连接到接收方
- 确认 `DEV_MODULE_PORT_{RECEIVER}` 与接收方 `module.conf` 的 port 一致

**错误：消息类型冲突**
- 检查 `include/dev.h` 中同一大类下是否有重复子类值
- 大类编号（0x000X）应与模块在系统中的序号对应，子类从 0x0001 开始递增
