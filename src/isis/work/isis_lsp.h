/**
 * @file   isis_lsp.h
 * @brief  ISIS LSP 报文发送、接收与前缀路由学习
 * @author jhb
 * @date   2026/04/12
 */
#ifndef ISIS_LSP_H
#define ISIS_LSP_H

#include <linux/if_packet.h>
#include <stddef.h>
#include <stdint.h>

#include "isis_worker.h"

void isis_lsp_send_due(isis_instance_cfg_t *inst, int raw_fd, uint64_t now_msec);
void isis_lsp_handle_pdu(int raw_fd, const uint8_t *pdu, size_t pdu_len, const struct sockaddr_ll *sll,
                         const uint8_t src_mac[6]);
void isis_lsp_reconcile_instance(isis_instance_cfg_t *inst, uint64_t now_msec);
void isis_lsp_remove_origin(isis_instance_cfg_t *inst, uint8_t level, const uint8_t system_id[6]);

#endif /* ISIS_LSP_H */
