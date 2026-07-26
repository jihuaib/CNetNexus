/**
 * @file   lldp_db.h
 * @brief  LLDP DB 操作接口
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_DB_H
#define LLDP_DB_H

#include <stdint.h>

#include "if.h"
#include "lldp.h"

#define LLDP_TABLE_PROTOCOL "lldp_protocol"
#define LLDP_TABLE_INTERFACE "lldp_interface"
#define LLDP_PROTOCOL_INST_ID 1u

typedef struct lldp_proto_cfg
{
    uint8_t admin_up;
    uint32_t tx_interval_sec;
    uint32_t hold_multiplier;
    uint32_t reinit_delay_sec;
    uint32_t tx_delay_sec;
} lldp_proto_cfg_t;

typedef struct lldp_if_cfg
{
    char ifname[IF_LOGICAL_NAME_MAX];
    uint8_t enabled;
    uint8_t admin_status;
    uint32_t tx_interval_sec;
    uint32_t hold_multiplier;
    char port_desc[LLDP_PORT_DESC_MAX];
} lldp_if_cfg_t;

int lldp_db_init(void);
int lldp_db_restore(void);

int lldp_db_set_proto_admin(uint8_t admin_up);
int lldp_db_set_proto_tx_interval(uint32_t tx_interval_sec);
int lldp_db_set_proto_hold_multiplier(uint32_t hold_multiplier);
int lldp_db_get_proto_cfg(lldp_proto_cfg_t *cfg_out);
int lldp_db_has_config(gboolean *has_config_out);

gboolean lldp_db_interface_is_implicit_default(const lldp_if_cfg_t *cfg);
int lldp_db_set_interface(const char *ifname, const lldp_if_cfg_t *cfg);
int lldp_db_del_interface(const char *ifname);
int lldp_db_get_interface(const char *ifname, lldp_if_cfg_t *cfg_out);

#endif /* LLDP_DB_H */
