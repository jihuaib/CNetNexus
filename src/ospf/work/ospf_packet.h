/**
 * @file   ospf_packet.h
 * @brief  OSPFv2 packet I/O and neighbor synchronization
 */
#ifndef OSPF_PACKET_H
#define OSPF_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "ospf_worker.h"

#define OSPF_HEADER_LEN 24u
#define OSPF_LSA_HEADER_LEN 20u

uint16_t ospf_internet_checksum(const uint8_t *data, size_t len);
uint16_t ospf_lsa_checksum(uint8_t *lsa, size_t len);
int ospf_lsa_checksum_valid(const uint8_t *lsa, size_t len);

int ospf_packet_socket_open(void);
void ospf_packet_socket_close(void);
void ospf_packet_handle_read(void);
void ospf_packet_tick(uint64_t now_msec);
void ospf_packet_reconcile_interface(ospf_instance_t *inst, ospf_if_cfg_t *cfg);
void ospf_packet_remove_interface(ospf_instance_t *inst, const char *ifname);
void ospf_packet_reset_instance(ospf_instance_t *inst);
void ospf_packet_flood_lsa(ospf_instance_t *inst, const ospf_lsa_entry_t *entry, const ospf_neighbor_t *exclude);
void ospf_packet_send_lsdb(ospf_instance_t *inst, ospf_neighbor_t *nbr);

#endif /* OSPF_PACKET_H */
