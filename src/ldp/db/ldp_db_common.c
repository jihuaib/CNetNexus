/**
 * @file   ldp_db_common.c
 * @brief  LDP DB 共享 helper：PK 构造器
 * @author jhb
 * @date   2026/05/05
 */

#include "ldp_db_internal.h"

void ldp_db_proto_pk(db_filter_builder_t *pk)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "inst_id", (int64_t)LDP_PROTOCOL_INST_ID);
}

void ldp_db_if_pk(db_filter_builder_t *pk, const char *ifname)
{
    db_filter_init(pk);
    db_filter_add_text(pk, "ifname", ifname ? ifname : "");
}
