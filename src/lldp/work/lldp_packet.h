/**
 * @file   lldp_packet.h
 * @brief  LLDP TLV 编解码
 * @author jhb
 * @date   2026/06/07
 */
#ifndef LLDP_PACKET_H
#define LLDP_PACKET_H

#include <stddef.h>
#include <stdint.h>

#define LLDP_ETHERTYPE 0x88CCu
#define LLDP_TLV_MAX_VALUE_LEN 511u
#define LLDP_ID_MAX_LEN 256u
#define LLDP_TEXT_MAX_LEN 512u
#define LLDP_MGMT_ADDR_MAX_LEN 32u

typedef enum lldp_tlv_type
{
    LLDP_TLV_END = 0,
    LLDP_TLV_CHASSIS_ID = 1,
    LLDP_TLV_PORT_ID = 2,
    LLDP_TLV_TTL = 3,
    LLDP_TLV_PORT_DESC = 4,
    LLDP_TLV_SYSTEM_NAME = 5,
    LLDP_TLV_SYSTEM_DESC = 6,
    LLDP_TLV_SYSTEM_CAP = 7,
    LLDP_TLV_MGMT_ADDR = 8,
} lldp_tlv_type_t;

typedef enum lldp_chassis_subtype
{
    LLDP_CHASSIS_SUBTYPE_CHASSIS_COMPONENT = 1,
    LLDP_CHASSIS_SUBTYPE_INTERFACE_ALIAS = 2,
    LLDP_CHASSIS_SUBTYPE_PORT_COMPONENT = 3,
    LLDP_CHASSIS_SUBTYPE_MAC_ADDRESS = 4,
    LLDP_CHASSIS_SUBTYPE_NETWORK_ADDRESS = 5,
    LLDP_CHASSIS_SUBTYPE_INTERFACE_NAME = 6,
    LLDP_CHASSIS_SUBTYPE_LOCAL = 7,
} lldp_chassis_subtype_t;

typedef enum lldp_port_subtype
{
    LLDP_PORT_SUBTYPE_INTERFACE_ALIAS = 1,
    LLDP_PORT_SUBTYPE_PORT_COMPONENT = 2,
    LLDP_PORT_SUBTYPE_MAC_ADDRESS = 3,
    LLDP_PORT_SUBTYPE_NETWORK_ADDRESS = 4,
    LLDP_PORT_SUBTYPE_INTERFACE_NAME = 5,
    LLDP_PORT_SUBTYPE_AGENT_CIRCUIT_ID = 6,
    LLDP_PORT_SUBTYPE_LOCAL = 7,
} lldp_port_subtype_t;

typedef struct lldp_id
{
    uint8_t subtype;
    uint16_t len;
    uint8_t data[LLDP_ID_MAX_LEN];
} lldp_id_t;

typedef struct lldp_mgmt_addr
{
    uint8_t present;
    uint8_t addr_subtype;
    uint8_t addr_len;
    uint8_t addr[LLDP_MGMT_ADDR_MAX_LEN];
    uint8_t if_subtype;
    uint32_t if_number;
    uint8_t oid_len;
    uint8_t oid[LLDP_MGMT_ADDR_MAX_LEN];
} lldp_mgmt_addr_t;

typedef struct lldp_packet
{
    lldp_id_t chassis_id;
    lldp_id_t port_id;
    uint16_t ttl;
    char port_desc[LLDP_TEXT_MAX_LEN];
    char system_name[LLDP_TEXT_MAX_LEN];
    char system_desc[LLDP_TEXT_MAX_LEN];
    uint16_t caps_supported;
    uint16_t caps_enabled;
    lldp_mgmt_addr_t mgmt_addr;
    uint8_t has_end;
} lldp_packet_t;

typedef struct lldp_packet_build_info
{
    lldp_id_t chassis_id;
    lldp_id_t port_id;
    uint16_t ttl;
    const char *port_desc;
    const char *system_name;
    const char *system_desc;
    uint16_t caps_supported;
    uint16_t caps_enabled;
    const lldp_mgmt_addr_t *mgmt_addr;
} lldp_packet_build_info_t;

int lldp_packet_build_basic(uint8_t *buf, size_t cap, const lldp_packet_build_info_t *info, size_t *len_out);
int lldp_packet_parse(const uint8_t *buf, size_t len, lldp_packet_t *out);
void lldp_packet_clear(lldp_packet_t *pkt);

#endif /* LLDP_PACKET_H */
