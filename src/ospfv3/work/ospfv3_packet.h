/**
 * @file   ospfv3_packet.h
 * @brief  OSPFv3 packet I/O and neighbor synchronization
 */
#ifndef OSPFV3_PACKET_H
#define OSPFV3_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "ospfv3_worker.h"

#define OSPFV3_HEADER_LEN 16u
#define OSPFV3_LSA_HEADER_LEN 20u

uint16_t ospfv3_internet_checksum(const uint8_t *data, size_t len);
uint16_t ospfv3_lsa_checksum(uint8_t *lsa, size_t len);
int ospfv3_lsa_checksum_valid(const uint8_t *lsa, size_t len);

int ospfv3_packet_socket_open(void);
void ospfv3_packet_socket_close(void);
void ospfv3_packet_handle_read(void);
void ospfv3_packet_tick(uint64_t now_msec);
void ospfv3_packet_reconcile_interface(ospfv3_instance_t *inst, ospfv3_if_cfg_t *cfg);
void ospfv3_packet_remove_interface(ospfv3_instance_t *inst, const char *ifname);
void ospfv3_packet_reset_instance(ospfv3_instance_t *inst);
void ospfv3_packet_flood_lsa(ospfv3_instance_t *inst, const ospfv3_lsa_entry_t *entry,
                             const ospfv3_neighbor_t *exclude);
void ospfv3_packet_send_lsdb(ospfv3_instance_t *inst, ospfv3_neighbor_t *nbr);

#endif /* OSPFV3_PACKET_H */
