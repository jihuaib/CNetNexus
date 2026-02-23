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
    DEV_PHASE_REGISTERED = 0, /**< 已注册（无 IPC） */
    DEV_PHASE_LOADED,         /**< 已加载（constructor 执行完毕，IPC+本地状态已初始化） */
    DEV_PHASE_IPC_READY,      /**< Phase 1: IPC 连接已建立 */
    DEV_PHASE_DB_RECOVERED,   /**< Phase 2: 预留（DB 恢复） */
    DEV_PHASE_READY           /**< Phase 3: 预留 */
};

/** 模块描述结构体 */
typedef struct module
{
    uint32_t module_id;                 /**< 模块 ID */
    char name[DEV_MODULE_NAME_MAX_LEN]; /**< 模块名称 */
    char *db_name;                      /**< 数据库名称（从 .conf 读取，可为 NULL） */
    void *dl_handle;                    /**< dlopen 句柄（动态加载，可为 NULL） */
    uint8_t phase;                      /**< 当前初始化阶段 */
    uint16_t port;                      /**< IPC 监听端口（从 module.conf 读取） */
} dev_module_t;

/**
 * @brief 扫描并加载所有模块（从 module.conf 发现，dlopen 触发 constructor 自注册）
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
 * @brief 将模块添加到注册表（GTree）
 * @param module_id 模块 ID
 * @param name 模块名称
 * @return 成功返回指向新建 dev_module_t 的指针，失败返回 NULL
 */
dev_module_t *dev_add_module_to_registry(uint32_t module_id, const char *name);

/**
 * @brief 内部获取模块名称
 * @param module_id 模块 ID
 * @param module_name 输出缓冲区
 * @return 成功返回 0，失败返回 -1
 */
int dev_get_module_name_inner(uint32_t module_id, char *module_name);

#endif // DEV_MODULE_H
