/**
 * @file   ospfv3_db.h
 * @brief  OSPFv3 persistent configuration
 */
#ifndef OSPFV3_DB_H
#define OSPFV3_DB_H

#include <stddef.h>
#include <stdint.h>

#include "ospfv3_worker.h"

#define OSPFV3_TABLE_INSTANCE "ospfv3_instance"
#define OSPFV3_TABLE_AREA "ospfv3_area"
#define OSPFV3_TABLE_INTERFACE "ospfv3_interface"

int ospfv3_db_init(void);
int ospfv3_db_restore(void);

int ospfv3_db_set_instance(uint32_t process_id, uint32_t vrf_id, const char *vrf_name);
int ospfv3_db_del_instance(uint32_t process_id);
int ospfv3_db_set_router_id(uint32_t process_id, uint32_t router_id);
int ospfv3_db_get_instance(uint32_t process_id, uint32_t *router_id_out, uint32_t *vrf_id_out, char *vrf_name_out,
                           size_t vrf_name_out_size);
int ospfv3_db_resolve_vrf(const char *vrf_name, uint32_t *vrf_id_out);

int ospfv3_db_set_area(uint32_t process_id, uint32_t area_id);
int ospfv3_db_del_area(uint32_t process_id, uint32_t area_id);
int ospfv3_db_area_exists(uint32_t process_id, uint32_t area_id, gboolean *exists);
int ospfv3_db_area_in_use(uint32_t process_id, uint32_t area_id, char *ifname_out, size_t ifname_out_size);

int ospfv3_db_set_interface(uint32_t process_id, const ospfv3_if_cfg_t *cfg);
int ospfv3_db_get_interface(uint32_t process_id, const char *ifname, ospfv3_if_cfg_t *cfg_out);
int ospfv3_db_del_interface(uint32_t process_id, const char *ifname);

#endif /* OSPFV3_DB_H */
