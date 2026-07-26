/**
 * @file   ospf_db.h
 * @brief  OSPFv2 persistent configuration
 */
#ifndef OSPF_DB_H
#define OSPF_DB_H

#include <stddef.h>
#include <stdint.h>

#include "ospf_worker.h"

#define OSPF_TABLE_INSTANCE "ospf_instance"
#define OSPF_TABLE_AREA "ospf_area"
#define OSPF_TABLE_INTERFACE "ospf_interface"

int ospf_db_init(void);
int ospf_db_restore(void);

int ospf_db_set_instance(uint32_t process_id, uint32_t vrf_id, const char *vrf_name);
int ospf_db_del_instance(uint32_t process_id);
int ospf_db_set_router_id(uint32_t process_id, uint32_t router_id);
int ospf_db_get_instance(uint32_t process_id, uint32_t *router_id_out, uint32_t *vrf_id_out, char *vrf_name_out,
                         size_t vrf_name_out_size);
int ospf_db_resolve_vrf(const char *vrf_name, uint32_t *vrf_id_out);

int ospf_db_set_area(uint32_t process_id, uint32_t area_id);
int ospf_db_del_area(uint32_t process_id, uint32_t area_id);
int ospf_db_area_exists(uint32_t process_id, uint32_t area_id, gboolean *exists);
int ospf_db_area_in_use(uint32_t process_id, uint32_t area_id, char *ifname_out, size_t ifname_out_size);

int ospf_db_set_interface(uint32_t process_id, const ospf_if_cfg_t *cfg);
int ospf_db_get_interface(uint32_t process_id, const char *ifname, ospf_if_cfg_t *cfg_out);
int ospf_db_del_interface(uint32_t process_id, const char *ifname);

#endif /* OSPF_DB_H */
