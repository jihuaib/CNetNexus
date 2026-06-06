/**
 * @file   db_config.h
 * @brief  running/startup 配置管理：运行库为临时库，save 落盘命名快照，startup 指定恢复源和模式
 * @author jhb
 * @date   2026/05/31
 */
#ifndef DB_CONFIG_H
#define DB_CONFIG_H

#include <glib.h>
#include <stddef.h>

/** 默认配置名（save 省略名称时使用） */
#define DB_CONFIG_DEFAULT_NAME "startup"

/** 配置名最大长度（不含终止符） */
#define DB_CONFIG_NAME_MAX 63

typedef enum db_config_startup_mode
{
    DB_CONFIG_STARTUP_MODE_NONE = 0,
    DB_CONFIG_STARTUP_MODE_DB,
    DB_CONFIG_STARTUP_MODE_CFG,
} db_config_startup_mode_t;

/**
 * @brief startup 模式名
 *
 * @param mode startup 模式
 * @return "db" / "cfg" / "none"
 */
const char *db_config_startup_mode_name(db_config_startup_mode_t mode);

/**
 * @brief 获取运行库文件路径（替代原 netnexus.db）
 *
 * 设置了 NN_WORK_DIR 时为 $NN_WORK_DIR/data/running.db，否则 ./data/running.db。
 *
 * @param path_buf 输出缓冲
 * @param buf_size 缓冲大小
 * @return ERRCODE_SUCCESS
 */
int db_config_running_path(char *path_buf, size_t buf_size);

/**
 * @brief 开机预处理：在打开运行库句柄之前调用
 *
 * 流程：
 *   1. 删除上次运行残留的 running.db / -wal / -shm（运行库为临时库，不跨重启保留）
 *   2. 读 startup.cfg 指针，若模式为 db 且命名快照存在，则整库恢复到 running.db
 *   3. 若无 startup 指针、模式为 cfg 或快照缺失，则保持空库（cfg 模式由 CLI 后续回放）
 *
 * 纯本地文件操作，不依赖其它模块，必须在 db_initialize_database 打开句柄前完成。
 *
 * @return ERRCODE_SUCCESS（即便恢复失败也返回成功，退化为空库启动）
 */
int db_config_boot_prepare(void);

/**
 * @brief save configuration：将当前运行库快照和 BDR cfg 文本保存为命名配置
 *
 * 使用 SQLite Online Backup API（WAL 安全），.db/.cfg 均先写临时文件，再一起发布。
 *
 * @param name           配置名，NULL/空时使用当前 startup 名或默认名
 * @param cfg_text       show current-configuration 导出的 BDR 文本
 * @param saved_name     输出最终保存名，可为 NULL
 * @param saved_name_cap saved_name 缓冲大小
 * @param err            输出错误描述（调用方 g_free），可为 NULL
 * @return ERRCODE_SUCCESS 或错误码
 */
int db_config_save(const char *name, const char *cfg_text, char *saved_name, size_t saved_name_cap, char **err);

/**
 * @brief startup configuration：设置下次启动使用的配置名和恢复模式
 *
 * 校验对应命名快照存在后，原子写入 startup.cfg 指针，格式为 "<mode> <name>"。
 *
 * @param name 配置名（不可为空）
 * @param mode 启动恢复模式（db/cfg）
 * @param err  输出错误描述（调用方 g_free），可为 NULL
 * @return ERRCODE_SUCCESS 或错误码
 */
int db_config_set_startup(const char *name, db_config_startup_mode_t mode, char **err);

/**
 * @brief 读取当前 startup 指针指向的配置名和恢复模式
 *
 * @param name_buf 输出缓冲（无指针时写入空串）
 * @param buf_size 缓冲大小
 * @param mode     输出启动恢复模式（无指针时为 NONE），可为 NULL
 * @return ERRCODE_SUCCESS
 */
int db_config_get_startup(char *name_buf, size_t buf_size, db_config_startup_mode_t *mode);

#endif // DB_CONFIG_H
