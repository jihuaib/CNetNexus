/**
 * @file   ospfv3_lsa.h
 * @brief  OSPFv3 LSA database and origination
 */
#ifndef OSPFV3_LSA_H
#define OSPFV3_LSA_H

#include <stddef.h>
#include <stdint.h>

#include "ospfv3_worker.h"

char *ospfv3_lsa_key_new(uint32_t area_id, uint16_t type, uint32_t link_state_id, uint32_t advertising_router);
ospfv3_lsa_entry_t *ospfv3_lsa_lookup(const ospfv3_instance_t *inst, uint32_t area_id, uint16_t type,
                                      uint32_t link_state_id, uint32_t advertising_router);
uint16_t ospfv3_lsa_current_age(const ospfv3_lsa_entry_t *entry, uint64_t now_msec);
int ospfv3_lsa_install(ospfv3_instance_t *inst, uint32_t area_id, const uint8_t *raw, size_t raw_len, uint64_t now_msec,
                       int self_originated, int *changed_out, int *comparison_out);
void ospfv3_lsa_originate_all(ospfv3_instance_t *inst, uint64_t now_msec);
void ospfv3_lsa_age(ospfv3_instance_t *inst, uint64_t now_msec);
void ospfv3_lsa_flush_self(ospfv3_instance_t *inst);
int ospfv3_lsa_compare_header(const uint8_t *header, const ospfv3_lsa_entry_t *current);

#endif /* OSPFV3_LSA_H */
