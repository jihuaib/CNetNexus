#ifndef RPM_CLI_H
#define RPM_CLI_H

#include "dev.h"

#define RPM_CLI_GROUP_POLICY 1
#define RPM_CLI_GROUP_IF_MATCH_PREFIX 2
#define RPM_CLI_GROUP_APPLY_MED 3
#define RPM_CLI_GROUP_APPLY_LOCAL_PREF 4
#define RPM_CLI_GROUP_APPLY_COMMUNITY 5
#define RPM_CANDIDATE_QUERY_ROUTE_POLICY 1

int rpm_cli_handle_config_msg(dev_ipc_message_t *msg);
void rpm_cli_send_response(dev_ipc_message_t *msg, const char *text);
void rpm_cli_handle_candidates(dev_ipc_message_t *msg);

#endif /* RPM_CLI_H */
