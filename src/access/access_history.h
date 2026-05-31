/**
 * @file   access_history.h
 * @brief  ACCESS 线（终端）命令历史管理：会话级与全局历史
 * @author jhb
 * @date   2026/05/30
 */
#ifndef ACCESS_HISTORY_H
#define ACCESS_HISTORY_H

#include <stdint.h>
#include <time.h>

/** 单条命令最大长度 */
#define ACCESS_MAX_CMD_LEN 1024
/** 客户端地址字符串最大长度 */
#define ACCESS_MAX_CLIENT_IP_LEN 64
/** 单线会话历史条数 */
#define ACCESS_SESSION_HISTORY_SIZE 20
/** 全局历史条数 */
#define ACCESS_GLOBAL_HISTORY_SIZE 200

/** 历史条目 */
typedef struct
{
    char *command;                            /**< 命令字符串 */
    time_t timestamp;                         /**< 执行时刻 */
    char client_ip[ACCESS_MAX_CLIENT_IP_LEN]; /**< 客户端地址 */
} access_history_entry_t;

/** 单线会话历史 */
typedef struct
{
    access_history_entry_t entries[ACCESS_SESSION_HISTORY_SIZE];
    uint32_t count;
    uint32_t current_idx;
    int32_t browse_idx;                   /**< 浏览位置（-1=当前输入，0..N=历史） */
    char temp_buffer[ACCESS_MAX_CMD_LEN]; /**< 暂存未提交的当前输入 */
} access_session_history_t;

/** 全局历史 */
typedef struct
{
    access_history_entry_t entries[ACCESS_GLOBAL_HISTORY_SIZE];
    uint32_t count;
    uint32_t current_idx;
} access_global_history_t;

void access_session_history_init(access_session_history_t *history);
void access_session_history_add(access_session_history_t *history, const char *cmd, const char *client_ip);
const char *access_session_history_get(access_session_history_t *history, uint32_t relative_idx);
const access_history_entry_t *access_session_history_get_entry(access_session_history_t *history,
                                                               uint32_t relative_idx);
void access_session_history_cleanup(access_session_history_t *history);

void access_global_history_init(access_global_history_t *history);
void access_global_history_add(access_global_history_t *history, const char *cmd, const char *client_ip);
const access_history_entry_t *access_global_history_get_entry(access_global_history_t *history, uint32_t relative_idx);
void access_global_history_cleanup(access_global_history_t *history);

#endif // ACCESS_HISTORY_H
