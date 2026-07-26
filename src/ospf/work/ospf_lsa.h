/**
 * @file   ospf_lsa.h
 * @brief  OSPFv2 LSA database and origination
 */
#ifndef OSPF_LSA_H
#define OSPF_LSA_H

#include <stddef.h>
#include <stdint.h>

#include "ospf_worker.h"

char *ospf_lsa_key_new(uint32_t area_id, uint8_t type, uint32_t link_state_id, uint32_t advertising_router);
ospf_lsa_entry_t *ospf_lsa_lookup(const ospf_instance_t *inst, uint32_t area_id, uint8_t type, uint32_t link_state_id,
                                  uint32_t advertising_router);
uint16_t ospf_lsa_current_age(const ospf_lsa_entry_t *entry, uint64_t now_msec);
int ospf_lsa_install(ospf_instance_t *inst, uint32_t area_id, const uint8_t *raw, size_t raw_len, uint64_t now_msec,
                     int self_originated, int *changed_out, int *comparison_out);
void ospf_lsa_originate_all(ospf_instance_t *inst, uint64_t now_msec);
void ospf_lsa_age(ospf_instance_t *inst, uint64_t now_msec);
void ospf_lsa_flush_self(ospf_instance_t *inst);
int ospf_lsa_compare_header(const uint8_t *header, const ospf_lsa_entry_t *current);

#endif /* OSPF_LSA_H */
