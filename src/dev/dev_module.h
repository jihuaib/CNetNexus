/**
 * @file   dev_module.h
 * @brief  Dev 模块注册头文件（三阶段初始化）
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DEV_MODULE_H
#define DEV_MODULE_H

#include <glib.h>
#include <stdint.h>

#include "dev.h"
#include "ipc.h"

/** 模块初始化阶段 */
enum
{
    DEV_PHASE_REGISTERED = 0, /**< 已注册 */
    DEV_PHASE_IPC_CREATED,    /**< IPC 上下文已创建（入口函数执行完毕） */
    DEV_PHASE_STARTED,        /**< Phase 1: MODULE_START 已完成 */
    DEV_PHASE_CONNECTED,      /**< Phase 2: MODULE_CONNECT 已完成 */
    DEV_PHASE_READY           /**< Phase 3: MODULE_READY 已完成 */
};

/** 模块入口函数类型：创建 IPC 上下文并返回 */
typedef ipc_context_t *(*module_entry_fn)(void);

/** 模块描述结构体 */
typedef struct module
{
    uint32_t module_id;                     /**< 模块 ID */
    char name[DEV_MODULE_NAME_MAX_LEN];     /**< 模块名称 */
    ipc_msg_handler_fn msg_handler;         /**< 消息处理回调 */
    ipc_context_t *ipc_ctx;                 /**< IPC 上下文 */
    char *db_name;                          /**< 数据库名称（从 .conf 读取，可为 NULL） */
    char *xml_path;                         /**< commands.xml 路径（自动发现，可为 NULL） */
    void *dl_handle;                        /**< dlopen 句柄（动态加载，可为 NULL） */
    uint8_t phase;                          /**< 当前初始化阶段 */
} dev_module_t;

/**
 * @brief 扫描并加载所有模块（从 module.conf 发现，dlopen + 调用入口函数）
 * @return 成功返回 0，失败返回 -1
 */
int32_t dev_scan_and_load_modules(void);

/**
 * @brief 初始化所有已注册模块（三阶段流程）
 * @return 失败的模块数
 */
int32_t dev_init_all_modules(void);

/**
 * @brief 清理所有模块（逆序 shutdown）
 */
void cleanup_all_modules(void);

/**
 * @brief 遍历模块注册表
 * @param func 遍历回调
 * @param user_data 用户数据
 */
void dev_module_foreach(GTraverseFunc func, gpointer user_data);

/**
 * @brief 内部注册模块
 * @param id 模块 ID
 * @param name 模块名称
 * @param handler 消息处理回调
 */
void dev_register_module_inner(uint32_t id, const char *name, ipc_msg_handler_fn handler);

/**
 * @brief 内部获取模块名称
 * @param module_id 模块 ID
 * @param module_name 输出缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int dev_get_module_name_inner(uint32_t module_id, char *module_name);

/** 请求关闭 */
void dev_request_shutdown_inner(void);

/** 检查关闭标志 */
int dev_shutdown_requested_inner(void);

#endif // DEV_MODULE_H
