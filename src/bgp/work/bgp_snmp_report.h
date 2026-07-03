/**
 * @file   bgp_snmp_report.h
 * @brief  BGP SNMP standard notification reporting
 */
#ifndef BGP_SNMP_REPORT_H
#define BGP_SNMP_REPORT_H

#include "bgp_fsm.h"
#include "bgp_session.h"

void bgp_snmp_report_neighbor_state(const bgp_session_t *sess, bgp_fsm_state_t old_state, bgp_fsm_state_t new_state);

#endif /* BGP_SNMP_REPORT_H */
