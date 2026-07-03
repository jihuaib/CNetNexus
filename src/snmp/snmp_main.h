/**
 * @file   snmp_main.h
 * @brief  SNMP 模块主入口
 */
#ifndef SNMP_MAIN_H
#define SNMP_MAIN_H

#include <glib.h>
#include <signal.h>

#include "dev.h"
#include "snmp.h"

typedef struct snmp_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} snmp_local_t;

extern snmp_local_t *g_snmp_local;

static inline dev_ipc_context_t *snmp_local_ipc_ctx(void)
{
    return g_snmp_local->dev_ipc_ctx;
}

int snmp_module_init(void);
void snmp_module_cleanup(void);
void snmp_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

int snmp_agent_init(void);
void snmp_agent_loop(volatile sig_atomic_t *shutdown_flag);
void snmp_agent_shutdown(void);
int snmp_agent_value_set(const snmp_value_msg_t *value);
int snmp_agent_subtree_clear(const snmp_subtree_clear_msg_t *clear);
void snmp_agent_send_trap(const snmp_trap_msg_t *trap);
void snmp_agent_apply_config(const snmp_config_msg_t *cfg);

#endif /* SNMP_MAIN_H */
