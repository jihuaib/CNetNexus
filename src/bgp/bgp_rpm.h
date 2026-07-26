#ifndef BGP_RPM_H
#define BGP_RPM_H

#include "bgp.h"
#include "dev.h"

struct bgp_nh_subgroup;
struct bgp_nlri_entry;

void bgp_rpm_handle_event(dev_ipc_message_t *msg);
bool bgp_rpm_eval_export(const struct bgp_nh_subgroup *sg, const struct bgp_nlri_entry *nlri, bgp_attr_t *attr);

#endif /* BGP_RPM_H */
