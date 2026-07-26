#ifndef RPM_MAIN_H
#define RPM_MAIN_H

#include <glib.h>

#include "dev.h"
#include "rpm.h"

typedef struct rpm_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    GHashTable *policies;    /**< char* -> rpm_policy_t* */
    GHashTable *subscribers; /**< module id -> type mask */
    GMutex lock;
} rpm_local_t;

extern rpm_local_t *g_rpm_local;

int rpm_module_init(void);
void rpm_module_cleanup(void);
dev_ipc_context_t *rpm_local_ipc_ctx(void);

const rpm_policy_t *rpm_policy_lookup(const char *name);
void rpm_policy_store(const rpm_policy_t *policy);
void rpm_policy_remove(const char *name);
void rpm_policy_publish(uint32_t event, const rpm_policy_t *policy);
void rpm_policy_publish_all_to(uint32_t module_id, uint32_t interest_mask);

#endif /* RPM_MAIN_H */
