#include "bgp_rpm.h"

#include <stdio.h>
#include <string.h>

#include "bgp_instance.h"
#include "bgp_peer.h"
#include "bgp_protocol.h"
#include "bgp_session.h"
#include "bgp_update_group.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "log.h"
#include "rpm.h"
#include "vrf.h"

static void bgp_rpm_rebind_peer(bgp_peer_t *peer, bgp_session_t *sess, const rpm_policy_t *policy, bool valid,
                                bool clear)
{
    (void)bgp_update_group_withdraw_peer_aro(peer, sess);
    if (peer->subgroups)
    {
        bgp_subgroup_peer_leave(peer, sess);
    }
    if (clear)
    {
        memset(&peer->export_policy, 0, sizeof(peer->export_policy));
        peer->export_policy_valid = false;
    }
    else
    {
        peer->export_policy = *policy;
        peer->export_policy_valid = valid;
    }
}

void bgp_cfg_apply_export_policy(bgp_apply_cmd_t *apply)
{
    bgp_protocol_t *proto = g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
    if (!proto)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: BGP not configured.");
        return;
    }
    uint32_t vrf_id = BGP_VRF_PUBLIC_ID;
    if (strcmp(apply->vrf_name, VRF_PUBLIC_VRF_NAME) != 0)
    {
        const vrf_api_cache_entry_t *entry = vrf_api_cache_lookup_by_name(apply->vrf_name);
        if (!entry)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
            return;
        }
        vrf_id = entry->vrf_id;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: VRF not found.");
        return;
    }
    bgp_instance_t *inst =
        g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(apply->u.export_policy.afi, apply->u.export_policy.safi));
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Address family not enabled.");
        return;
    }
    bgp_peer_t *peer = g_hash_table_lookup(inst->peer_hash, &apply->u.export_policy.addr);
    if (!peer)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BGP Error: Neighbor not enabled in this address family.");
        return;
    }

    const char *wanted = apply->isNo ? "" : apply->u.export_policy.policy.name;
    if (strcmp(peer->export_policy.name, wanted) == 0 &&
        (apply->isNo || (peer->export_policy.revision == apply->u.export_policy.policy.revision &&
                         peer->export_policy_valid == apply->u.export_policy.policy_valid)))
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &apply->u.export_policy.addr);
    bgp_rpm_rebind_peer(peer, sess, &apply->u.export_policy.policy, apply->u.export_policy.policy_valid, apply->isNo);
    if (sess && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED && peer->state == BGP_PEER_STATE_ESTABLISHED)
    {
        bgp_update_group_catchup_session(sess);
    }
    apply->rc = BGP_APPLY_RC_OK;
}

void bgp_rpm_handle_event(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(rpm_policy_event_t) || !g_bgp_work_local ||
        !g_bgp_work_local->protocol)
    {
        return;
    }
    const rpm_policy_event_t *event = msg->payload;
    if (event->event == RPM_POLICY_EVENT_SMOOTH_END || (event->object_mask & RPM_OBJECT_ROUTE_POLICY) == 0u)
    {
        return;
    }

    GHashTable *sessions = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter vit;
    gpointer vv;
    g_hash_table_iter_init(&vit, g_bgp_work_local->protocol->vrf_hash);
    while (g_hash_table_iter_next(&vit, NULL, &vv))
    {
        bgp_vrf_t *vrf = vv;
        GHashTableIter iit;
        gpointer iv;
        g_hash_table_iter_init(&iit, vrf->inst_hash);
        while (g_hash_table_iter_next(&iit, NULL, &iv))
        {
            bgp_instance_t *inst = iv;
            GHashTableIter pit;
            gpointer pv;
            g_hash_table_iter_init(&pit, inst->peer_hash);
            while (g_hash_table_iter_next(&pit, NULL, &pv))
            {
                bgp_peer_t *peer = pv;
                if (strcmp(peer->export_policy.name, event->policy.name) != 0)
                {
                    continue;
                }
                bgp_session_t *sess = bgp_vrf_find_session(vrf, &peer->addr);
                bool valid = event->event == RPM_POLICY_EVENT_UPSERT;
                rpm_policy_t policy = event->policy;
                if (!valid)
                {
                    policy = peer->export_policy;
                    policy.node_count = 0;
                }
                bgp_rpm_rebind_peer(peer, sess, &policy, valid, false);
                if (sess)
                {
                    g_hash_table_add(sessions, sess);
                }
            }
        }
    }

    GHashTableIter sit;
    gpointer sess_ptr;
    g_hash_table_iter_init(&sit, sessions);
    while (g_hash_table_iter_next(&sit, &sess_ptr, NULL))
    {
        bgp_session_t *sess = sess_ptr;
        if (sess->fsm_state == BGP_FSM_STATE_ESTABLISHED)
        {
            bgp_update_group_catchup_session(sess);
        }
    }
    g_hash_table_destroy(sessions);
    LOG_INFO("BGP: RPM policy event applied name=%s event=%u", event->policy.name, event->event);
}

bool bgp_rpm_eval_export(const bgp_nh_subgroup_t *sg, const bgp_nlri_entry_t *nlri, bgp_attr_t *attr)
{
    if (!sg || !attr || !sg->peer_list)
    {
        return false;
    }
    const bgp_peer_t *peer = sg->peer_list->data;
    if (!peer || peer->export_policy.name[0] == '\0')
    {
        return true;
    }
    if (!peer->export_policy_valid)
    {
        return false;
    }

    const net_prefix_t *prefix = NULL;
    if (nlri && nlri->type == BGP_NLRI_PREFIX)
    {
        prefix = &nlri->prefix.prefix;
    }
    rpm_policy_result_t result;
    if (rpm_policy_evaluate(&peer->export_policy, prefix, &result) != RPM_POLICY_DECISION_PERMIT)
    {
        return false;
    }
    if ((result.apply_mask & RPM_APPLY_LOCAL_PREF) != 0u)
    {
        attr->local_pref = result.local_pref;
        attr->has_local_pref = true;
    }
    if ((result.apply_mask & RPM_APPLY_MED) != 0u)
    {
        attr->med = result.med;
        attr->has_med = true;
    }
    if ((result.apply_mask & RPM_APPLY_COMMUNITY) != 0u)
    {
        g_strlcpy(attr->communities, result.community, sizeof(attr->communities));
    }
    return true;
}
