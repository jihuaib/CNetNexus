/**
 * @file   vrf_main.h
 * @brief  VRF 模块全局状态声明
 * @author jhb
 * @date   2026/03/05
 */
#ifndef VRF_MAIN_H
#define VRF_MAIN_H

#include <glib.h>
#include <stdint.h>

#include "vrf.h"

/**
 * @brief 单条 VRF 表项（内存存储）
 */
typedef struct
{
    uint32_t vrf_id;             /**< VRF ID，0 为公网 VRF */
    char name[VRF_NAME_MAX_LEN]; /**< VRF 名称 */
} vrf_entry_t;

/**
 * @brief VRF 模块本地状态
 */
typedef struct
{
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文 */
    GHashTable *vrf_by_id;          /**< uint32_t → vrf_entry_t*（按 ID 索引） */
    GHashTable *vrf_by_name;        /**< const char* → vrf_entry_t*（按名称索引） */
    uint32_t next_id;               /**< 下一个可分配的 VRF ID（从 1 开始） */
} vrf_local_t;

/** 全局模块状态 */
extern vrf_local_t *g_vrf_local;

/**
 * @brief IPC 消息处理主回调
 * @param ctx IPC 上下文
 * @param msg 接收到的消息
 */
void vrf_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 创建一条 VRF 表项并加入内存表
 * @param name VRF 名称
 * @return 成功返回新 vrf_entry_t*，名称已存在或其他错误返回 NULL
 */
vrf_entry_t *vrf_create(const char *name);

/**
 * @brief 按 ID 查找 VRF 表项
 * @param vrf_id VRF ID
 * @return 找到返回 vrf_entry_t*，否则返回 NULL
 */
vrf_entry_t *vrf_find_by_id(uint32_t vrf_id);

/**
 * @brief 按名称查找 VRF 表项
 * @param name VRF 名称
 * @return 找到返回 vrf_entry_t*，否则返回 NULL
 */
vrf_entry_t *vrf_find_by_name(const char *name);

#endif /* VRF_MAIN_H */
