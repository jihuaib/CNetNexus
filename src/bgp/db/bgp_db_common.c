/**
 * @file   bgp_db_common.c
 * @brief  BGP DB 共享 helper：PK 构造器与表清空
 * @author jhb
 * @date   2026/04/26
 */

#include "bgp_db_internal.h"
#include "log.h"

void bgp_db_session_pk(db_filter_builder_t *pk, const char *vrf_name, const char *neighbor_ip)
{
    db_filter_init(pk);
    db_filter_add_text(pk, "vrf_name", vrf_name);
    db_filter_add_text(pk, "neighbor_ip", neighbor_ip);
}

void bgp_db_neighbor_pk(db_filter_builder_t *pk, const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi,
                        const char *neighbor_ip)
{
    db_filter_init(pk);
    db_filter_add_text(pk, "vrf_name", vrf_name);
    db_filter_add_int(pk, "afi", (int64_t)afi);
    db_filter_add_int(pk, "safi", (int64_t)safi);
    db_filter_add_text(pk, "neighbor_ip", neighbor_ip);
}

void bgp_db_vrf_pk(db_filter_builder_t *pk, const char *vrf_name)
{
    db_filter_init(pk);
    db_filter_add_text(pk, "vrf_name", vrf_name);
}

void bgp_db_instance_pk(db_filter_builder_t *pk, const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi)
{
    db_filter_init(pk);
    db_filter_add_text(pk, "vrf_name", vrf_name);
    db_filter_add_int(pk, "afi", (int64_t)afi);
    db_filter_add_int(pk, "safi", (int64_t)safi);
}

void bgp_db_qp_route_pk(db_filter_builder_t *pk, const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi,
                        uint32_t start_dqpn, uint32_t count, const char *prefix_addr, uint8_t mask_len, const char *bid)
{
    db_filter_init(pk);
    db_filter_add_text(pk, "vrf_name", vrf_name);
    db_filter_add_int(pk, "afi", (int64_t)afi);
    db_filter_add_int(pk, "safi", (int64_t)safi);
    db_filter_add_int(pk, "start_dqpn", (int64_t)start_dqpn);
    db_filter_add_int(pk, "route_count", (int64_t)count);
    db_filter_add_text(pk, "prefix_addr", prefix_addr);
    db_filter_add_int(pk, "mask_len", (int64_t)mask_len);
    db_filter_add_text(pk, "bid", bid);
}

int bgp_db_delete_table_all(dev_ipc_context_t *ctx, const char *table_name, int *rows_out)
{
    int rows = db_rpc_delete(ctx, table_name, NULL);
    if (rows < 0)
    {
        LOG_ERROR("BGP failed to clear table %s", table_name);
        return -1;
    }
    if (rows_out)
    {
        *rows_out = rows;
    }
    LOG_INFO("BGP table %s cleared, affected rows: %d", table_name, rows);
    return 0;
}
