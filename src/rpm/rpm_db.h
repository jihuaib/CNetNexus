#ifndef RPM_DB_H
#define RPM_DB_H

#include "rpm.h"

#define RPM_TABLE_POLICY "rpm_policy"
#define RPM_TABLE_NODE "rpm_policy_node"

int rpm_db_init(void);
int rpm_db_restore(void);
int rpm_db_get_policy(const char *name, rpm_policy_t *policy);
int rpm_db_upsert_node(const char *name, uint32_t type_mask, const rpm_policy_node_t *node);
int rpm_db_delete_node(const char *name, uint32_t sequence);
int rpm_db_delete_policy(const char *name);
int rpm_db_update_node(const char *name, const rpm_policy_node_t *node);
int rpm_db_list_policies(GPtrArray **out);

#endif /* RPM_DB_H */
