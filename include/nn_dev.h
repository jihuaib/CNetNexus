/**
 * @file   nn_dev.h
 * @brief  设备模块公共接口，定义模块注册、消息队列、发布订阅系统 API
 * @author jhb
 * @date   2026/01/22
 */

#ifndef NN_DEV_H
#define NN_DEV_H

#include <glib.h>
#include <stdint.h>

// ============================================================================
// 模块 ID 定义
// ============================================================================

/** DEV 模块 */
#define NN_DEV_MODULE_ID_DEV 0x00000001
/** DB 模块 */
#define NN_DEV_MODULE_ID_DB 0x00000002
/** CFG 模块 */
#define NN_DEV_MODULE_ID_CFG 0x00000003
/** IF 接口模块 */
#define NN_DEV_MODULE_ID_IF 0x00000004
/** BGP 模块 */
#define NN_DEV_MODULE_ID_BGP 0x00000005

// ============================================================================
// 事件 ID 定义（用于单播发布/订阅）
// ============================================================================

/** CFG 模块事件 */
#define NN_DEV_EVENT_CFG 0x00010001

// ============================================================================
// 组播组 ID 定义
// ============================================================================

/** 主机名变更组播组 */
#define NN_DEV_GROUP_HOST_NAME 0x00010001

/** 无效文件描述符 */
#define NN_DEV_INVALID_FD (-1)

/** 模块名称最大长度 */
#define NN_DEV_MODULE_NAME_MAX_LEN 12

/**
 * @brief 模块初始化回调函数类型
 * @return 成功返回 0，失败返回非零值
 */
typedef int32_t (*nn_module_init_fn)();

/**
 * @brief 模块清理回调函数类型
 */
typedef void (*nn_module_cleanup_fn)(void);

// ============================================================================
// 消息队列系统 API
// ============================================================================

/**
 * @brief 模块间通信消息结构
 */
struct nn_dev_message
{
    uint32_t msg_type;       /**< 消息类型 */
    uint32_t sender_id;      /**< 发送方模块 ID */
    uint32_t request_id;     /**< 请求 ID（用于关联请求和响应） */
    void *data;              /**< 消息数据 */
    size_t data_len;         /**< 数据长度 */
    void (*free_fn)(void *); /**< 数据释放函数 */
};

typedef struct nn_dev_message nn_dev_message_t;
typedef struct nn_dev_module_mq nn_dev_module_mq_t;

/**
 * @brief 创建消息
 * @param msg_type 消息类型
 * @param sender_id 发送方模块 ID
 * @param request_id 请求 ID
 * @param data 消息数据
 * @param data_len 数据长度
 * @param free_fn 数据释放函数
 * @return 新创建的消息，失败返回 NULL
 */
nn_dev_message_t *nn_dev_message_create(uint32_t msg_type, uint32_t sender_id, uint32_t request_id, void *data,
                                        size_t data_len, void (*free_fn)(void *));

/**
 * @brief 释放消息
 * @param msg 待释放的消息
 */
void nn_dev_message_free(nn_dev_message_t *msg);

/**
 * @brief 创建模块消息队列
 * @return 新创建的消息队列，失败返回 NULL
 */
nn_dev_module_mq_t *nn_dev_mq_create();

/**
 * @brief 销毁模块消息队列
 * @param mq 待销毁的消息队列
 */
void nn_dev_mq_destroy(nn_dev_module_mq_t *mq);

/**
 * @brief 向模块消息队列发送消息（线程安全）
 * @param event_fd 事件文件描述符
 * @param mq 目标消息队列
 * @param msg 待发送的消息
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_mq_send(int event_fd, nn_dev_module_mq_t *mq, nn_dev_message_t *msg);

/**
 * @brief 从消息队列接收消息（非阻塞，线程安全）
 * @param event_fd 事件文件描述符
 * @param mq 源消息队列
 * @return 接收到的消息，无消息时返回 NULL
 */
nn_dev_message_t *nn_dev_mq_receive(int event_fd, nn_dev_module_mq_t *mq);

// ============================================================================
// 发布/订阅系统 API
// ============================================================================

/**
 * @brief 注册模块到发布/订阅系统
 * @param module_id 模块 ID
 * @param eventfd 事件文件描述符
 * @param mq 模块消息队列
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_pubsub_register(uint32_t module_id, int eventfd, nn_dev_module_mq_t *mq);

/**
 * @brief 从发布/订阅系统注销模块
 * @param module_id 模块 ID
 */
void nn_dev_pubsub_unregister(uint32_t module_id);

/**
 * @brief 订阅指定发布者的事件（单播通道，点对点）
 * @param subscriber_id 订阅者模块 ID
 * @param publisher_id 发布者模块 ID
 * @param event_id 事件 ID
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_pubsub_subscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);

/**
 * @brief 取消订阅指定发布者的事件
 * @param subscriber_id 订阅者模块 ID
 * @param publisher_id 发布者模块 ID
 * @param event_id 事件 ID
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_pubsub_unsubscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);

/**
 * @brief 发布事件消息给所有订阅者
 * @param publisher_id 发布者模块 ID
 * @param event_id 事件 ID
 * @param msg 待发布的消息
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_pubsub_publish(uint32_t publisher_id, uint32_t event_id, nn_dev_message_t *msg);

/**
 * @brief 发布事件消息到指定目标模块
 * @param publisher_id 发布者模块 ID
 * @param event_id 事件 ID
 * @param target_module_id 目标模块 ID
 * @param msg 待发布的消息
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_pubsub_publish_to_module(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                    nn_dev_message_t *msg);

/**
 * @brief 同步查询：发送消息并等待响应
 * @param publisher_id 发布者模块 ID
 * @param event_id 事件 ID
 * @param target_module_id 目标模块 ID
 * @param msg 请求消息
 * @param timeout_ms 超时时间（毫秒）
 * @return 响应消息，超时或失败返回 NULL
 */
nn_dev_message_t *nn_dev_pubsub_query(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                      nn_dev_message_t *msg, uint32_t timeout_ms);

/**
 * @brief 直接向指定模块发送响应消息
 * @param target_module_id 目标模块 ID
 * @param msg 响应消息
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_pubsub_send_response(uint32_t target_module_id, nn_dev_message_t *msg);

// ============================================================================
// 公共 API
// ============================================================================

/**
 * @brief 注册模块（包含初始化和清理回调）
 * @param id 模块 ID
 * @param name 模块名称
 * @param init 初始化回调函数
 * @param cleanup 清理回调函数
 */
void nn_dev_register_module(uint32_t id, const char *name, nn_module_init_fn init, nn_module_cleanup_fn cleanup);

/**
 * @brief 根据模块 ID 获取模块名称
 * @param module_id 模块 ID
 * @param module_name 输出模块名称缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int nn_dev_get_module_name(uint32_t module_id, char *module_name);

/**
 * @brief 请求关闭所有模块
 */
void nn_dev_request_shutdown(void);

/**
 * @brief 检查是否已请求关闭
 * @return 已请求关闭返回非零值，否则返回 0
 */
int nn_dev_shutdown_requested(void);

#endif // NN_DEV_H
