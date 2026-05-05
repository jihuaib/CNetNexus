/**
 * @file   ldp_db.h
 * @brief  LDP 模块 DB 操作接口
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_DB_H
#define LDP_DB_H

#include <stdint.h>

#include "if.h"

#define LDP_TABLE_PROTOCOL "ldp_protocol"
#define LDP_TABLE_INTERFACE "ldp_interface"

/** 单实例 LDP 协议配置（PK = inst_id，固定为 1） */
#define LDP_PROTOCOL_INST_ID 1u

typedef struct ldp_proto_cfg
{
    uint32_t lsr_id;            /**< 32-bit LSR-ID（点分十进制 v4 表示） */
    uint32_t hello_interval_ms; /**< 全局 hello 间隔（毫秒） */
    uint32_t hold_time_ms;      /**< 全局 hold 时间（毫秒） */
    uint32_t keepalive_ms;      /**< 全局 keepalive 间隔（毫秒） */
    uint8_t admin_up;           /**< 是否使能 LDP（>0 使能） */
} ldp_proto_cfg_t;

typedef struct ldp_if_cfg
{
    char ifname[IF_LOGICAL_NAME_MAX]; /**< 接口逻辑名 */
    uint8_t enabled;                  /**< 是否使能 LDP */
    uint32_t hello_interval_ms;       /**< 0 表示继承全局值 */
    uint32_t hold_time_ms;            /**< 0 表示继承全局值 */
} ldp_if_cfg_t;

int ldp_db_init(void);
int ldp_db_restore(void);

int ldp_db_set_proto_admin(uint8_t admin_up);
int ldp_db_set_lsr_id(uint32_t lsr_id);
int ldp_db_set_hello_interval(uint32_t hello_ms);
int ldp_db_set_hold_time(uint32_t hold_ms);
int ldp_db_set_keepalive(uint32_t keepalive_ms);
int ldp_db_get_proto_cfg(ldp_proto_cfg_t *cfg_out);

int ldp_db_set_interface(const char *ifname, const ldp_if_cfg_t *cfg);
int ldp_db_del_interface(const char *ifname);
int ldp_db_get_interface(const char *ifname, ldp_if_cfg_t *cfg_out);

#endif /* LDP_DB_H */
