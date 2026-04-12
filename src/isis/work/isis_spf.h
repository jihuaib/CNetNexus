/**
 * @file   isis_spf.h
 * @brief  ISIS SPF 拓扑最短路计算与前缀路由重算
 * @author jhb
 * @date   2026/04/12
 */
#ifndef ISIS_SPF_H
#define ISIS_SPF_H

#include <stddef.h>
#include <stdint.h>

#include "if_api.h"
#include "isis_worker.h"

void isis_spf_process_lsp(isis_instance_cfg_t *inst, uint8_t level, const uint8_t origin_sysid[6],
                          const isis_neighbor_t *nexthop_nbr, const isis_if_cfg_t *if_cfg,
                          const if_api_cache_entry_t *if_entry, const uint8_t *tlvs, size_t tlv_len);
void isis_spf_reconcile_instance(isis_instance_cfg_t *inst);
void isis_spf_withdraw_neighbor_routes(isis_instance_cfg_t *inst, const isis_neighbor_t *nbr);
void isis_spf_withdraw_origin_routes(isis_instance_cfg_t *inst, uint8_t level, const uint8_t origin_sysid[6]);

#endif /* ISIS_SPF_H */
