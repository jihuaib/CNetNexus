/**
 * @file   ldp_lib.h
 * @brief  LDP Label Information Base：local LIB（自分配）+ remote LIB（peer 通告）
 *
 * Local LIB:  FEC(prefix/len) → assigned local label
 * Remote LIB: (peer_lsr_id, FEC) → label received from that peer
 *
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_LIB_H
#define LDP_LIB_H

#include <glib.h>
#include <stdint.h>

/** FEC：M4 仅支持 IPv4 prefix */
typedef struct ldp_fec
{
    uint32_t prefix; /**< host order */
    uint8_t prefix_len;
} ldp_fec_t;

/** local LIB 项 */
typedef struct ldp_local_label
{
    ldp_fec_t fec;
    uint32_t label; /**< 0xFFFFFFFFu 表示 implicit-null 占位（不应直接发送，发送时映射为 LDP_LABEL_IMPLICIT_NULL） */
} ldp_local_label_t;

/** remote LIB 项 */
typedef struct ldp_remote_label
{
    uint32_t peer_lsr_id;
    uint16_t peer_label_space;
    ldp_fec_t fec;
    uint32_t label;
} ldp_remote_label_t;

/* 生命周期 */
void ldp_lib_init(void);
void ldp_lib_cleanup(void);

/* local LIB */
uint32_t ldp_lib_alloc_local_label(const ldp_fec_t *fec);
void ldp_lib_free_local_label(const ldp_fec_t *fec);
const ldp_local_label_t *ldp_lib_lookup_local(const ldp_fec_t *fec);
GHashTable *ldp_lib_local_table(void);

/* remote LIB */
void ldp_lib_set_remote(uint32_t peer_lsr_id, uint16_t peer_label_space, const ldp_fec_t *fec, uint32_t label);
void ldp_lib_del_remote(uint32_t peer_lsr_id, uint16_t peer_label_space, const ldp_fec_t *fec);
void ldp_lib_purge_peer(uint32_t peer_lsr_id, uint16_t peer_label_space);
GHashTable *ldp_lib_remote_table(void);

#endif /* LDP_LIB_H */
