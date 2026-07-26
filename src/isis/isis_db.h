/**
 * @file   isis_db.h
 * @brief  ISIS 模块 DB 操作接口
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_DB_H
#define ISIS_DB_H

#include <stddef.h>
#include <stdint.h>

#include "isis_worker.h"

#define ISIS_TABLE_INSTANCE "isis_instance"
#define ISIS_TABLE_INTERFACE "isis_interface"

int isis_db_init(void);
int isis_db_restore(void);

int isis_db_set_instance(uint32_t tag, uint32_t vrf_id, const char *vrf_name);
int isis_db_get_instance_vrf(uint32_t tag, uint32_t *vrf_id_out, char *vrf_name_out, size_t vrf_name_out_size);
int isis_db_resolve_vrf(const char *vrf_name, uint32_t *vrf_id_out);
int isis_db_del_instance(uint32_t tag);
int isis_db_set_net(uint32_t tag, const char *net);
int isis_db_set_is_type(uint32_t tag, uint8_t is_type);
int isis_db_set_cost_style(uint32_t tag, uint8_t cost_style);
int isis_db_set_af(uint32_t tag, uint16_t afi, int enabled);
int isis_db_is_af_enabled(uint32_t tag, uint16_t afi, int *enabled_out);

int isis_db_set_interface_af_cfg(uint32_t tag, const char *ifname, uint16_t afi, const isis_if_af_cfg_t *cfg);
int isis_db_get_interface_af_cfg(uint32_t tag, const char *ifname, uint16_t afi, isis_if_af_cfg_t *cfg_out);
int isis_db_del_interface_af(uint32_t tag, const char *ifname, uint16_t afi);

int isis_db_set_interface_cfg(uint32_t tag, const char *ifname, const isis_if_cfg_t *cfg);
int isis_db_get_interface_cfg(uint32_t tag, const char *ifname, isis_if_cfg_t *cfg_out);

int isis_db_set_interface(uint32_t tag, const char *ifname, uint32_t metric);
int isis_db_del_interface(uint32_t tag, const char *ifname);
int isis_db_get_interface(uint32_t tag, const char *ifname, uint32_t *metric_out);
int isis_db_get_default_tag(uint32_t *tag_out);

#endif /* ISIS_DB_H */
