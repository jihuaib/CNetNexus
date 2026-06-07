/**
 * @file   lldp_db_common.c
 * @brief  LLDP DB 共享 helper
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_db_internal.h"

void lldp_db_proto_pk(db_filter_builder_t *pk)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "inst_id", (int64_t)LLDP_PROTOCOL_INST_ID);
}

void lldp_db_if_pk(db_filter_builder_t *pk, const char *ifname)
{
    db_filter_init(pk);
    db_filter_add_text(pk, "ifname", ifname ? ifname : "");
}
