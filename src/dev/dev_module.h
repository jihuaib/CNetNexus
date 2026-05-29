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
#include <sys/types.h>
#include <time.h>

#include "dev.h"

/** 模块崩溃自愈窗口：crash_count 在窗口内累加，超过窗口归零重新计数 */
#define DEV_MODULE_CRASH_WINDOW_SEC 60
/** 窗口内允许的最大自动重启次数；超过则放弃，等待人工介入 */
#define DEV_MODULE_CRASH_MAX_RETRIES 5

/** 模块状态。模块靠 NOTIFY_READY 显式上报 READY；中间 LOADED 表示已 fork 但未 ready。 */
enum
{
    DEV_PHASE_REGISTERED = 0, /**< 已注册（未 fork / 已退出） */
    DEV_PHASE_LOADED,         /**< 已 fork，等待 NOTIFY_READY */
    DEV_PHASE_READY           /**< 模块自报 NOTIFY_READY，可用 */
};

/** 模块描述结构体 */
typedef struct module
{
    uint32_t module_id;                 /**< 模块 ID */
    char name[DEV_MODULE_NAME_MAX_LEN]; /**< 模块名称 */
    pid_t child_pid;                    /**< 子进程 PID（多进程模式，0 表示未启动） */
    uint8_t phase;                      /**< 当前初始化阶段 */
    uint16_t port;                      /**< IPC 监听端口（从 module.conf 读取） */

    /* 按需启动 & 订阅机制相关 */
    uint8_t on_demand; /**< 1=按需启动（首次被订阅且 auto_start=1 时由 DEV fork） */
    uint8_t pending_restart; /**< 1=用户/DEV 主动请求重启；SIGCHLD 收到后自动 fork 同样进程（无视 on_demand） */
    uint8_t pending_stop; /**< 1=用户主动 stop；SIGCHLD 收到后保持 REGISTERED，不告警、不重启 */
    uint8_t pre_cleaned; /**< 1=模块在 exit 前已通过 PRE_EXIT RPC 让 DEV 同步做完清理；SIGCHLD 据此跳过重复清理 */
    pid_t pre_cleaned_pid; /**< PRE_EXIT 时把 child_pid 转存到这里，留给 SIGCHLD 的 find_by_pid 兜底；child_pid
                              同时清零以便订阅判断 */
    time_t last_crash_time; /**< 上一次意外退出时间戳，用于 crash backoff 窗口判定 */
    uint32_t crash_count;   /**< 当前窗口内的连续意外退出次数 */
    char exe_name[64];      /**< 可执行文件名（按需启动时使用） */
    char revive_table[64]; /**< on-demand 模块的"配置存在标识表"，DEV 在 boot 时扫到非空即自动 fork */
    uint32_t epoch;        /**< 每次启动 +1，订阅方据此判断对端是否重启过 */
    GList *subscribers; /**< 订阅者列表 GList<GUINT_TO_POINTER(subscriber_module_id)>，本模块 READY/DOWN 时推送给他们 */
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
 * @brief 软件级重启（重启全部业务模块并重新完成三阶段初始化）
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int32_t dev_reboot_software(void);

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
 * @param module_name 输出缓冲区（至少 DEV_MODULE_NAME_MAX_LEN 字节）
 * @return 成功返回 0，失败返回 -1
 */
int dev_get_module_name_inner(uint32_t module_id, char *module_name);

/**
 * @brief 按名称反向查找模块 ID
 * @param name      模块名称字符串
 * @param module_id 输出模块 ID
 * @return 成功返回 ERRCODE_SUCCESS，未找到返回 ERRCODE_FAIL
 */
int dev_get_module_id_by_name(const char *name, uint32_t *module_id);

/**
 * @brief 按 ID 在注册表中查找模块（供 dev_subscribe.c 等使用）
 * @param module_id 模块 ID
 * @return 找到返回指针；未找到返回 NULL
 */
dev_module_t *dev_module_find(uint32_t module_id);

/**
 * @brief 按需启动一个已注册但未运行的模块（fork + exec）
 *
 * 仅当 module->on_demand==1 且 module->child_pid<=0 时实际启动；
 * 已在运行则幂等返回成功。
 *
 * @param module 模块条目
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int dev_module_spawn_on_demand(dev_module_t *module);

/**
 * @brief 按 PID 反查模块（供 SIGCHLD 处理使用）
 * @param pid 子进程 PID
 * @return 找到返回指针；未找到返回 NULL
 */
dev_module_t *dev_module_find_by_pid(pid_t pid);

/**
 * @brief 标记是否正在执行批量清理（cleanup_all_modules / dev_reboot_software）
 *
 * SIGCHLD 处理器据此抑制"crashed/respawn"逻辑——批量退出全是预期内的。
 *
 * @param flag 1=进入清理；0=清理结束
 */
void dev_module_set_cleanup_in_progress(int flag);

/**
 * @brief 查询批量清理标志（供 SIGCHLD 处理器使用）
 */
int dev_module_is_cleanup_in_progress(void);

/**
 * @brief 重新 fork 一个已注册模块（用于显式重启，绕过 on_demand 判定）
 *
 * 仅当 module->child_pid <= 0 时实际启动；已在运行则幂等返回成功。
 *
 * @param module 模块条目
 * @return 成功返回 ERRCODE_SUCCESS，失败返回 ERRCODE_FAIL
 */
int dev_module_respawn(dev_module_t *module);

#endif // DEV_MODULE_H
