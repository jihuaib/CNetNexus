/**
 * @file   isis_db_common.c
 * @brief  ISIS DB 共享 helper：PK 构造器与接口配置规范化
 * @author jhb
 * @date   2026/04/26
 */

#include <string.h>

#include "isis_db_internal.h"

void isis_db_instance_pk(db_filter_builder_t *pk, uint32_t tag)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "tag", (int64_t)tag);
}

void isis_db_interface_tag_pk(db_filter_builder_t *pk, uint32_t tag)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "tag", (int64_t)tag);
}

void isis_db_interface_key_pk(db_filter_builder_t *pk, uint32_t tag, const char *ifname)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "tag", (int64_t)tag);
    db_filter_add_text(pk, "ifname", ifname);
}

void isis_db_interface_af_pk(db_filter_builder_t *pk, uint32_t tag, const char *ifname, uint16_t afi)
{
    db_filter_init(pk);
    db_filter_add_int(pk, "tag", (int64_t)tag);
    db_filter_add_text(pk, "ifname", ifname);
    db_filter_add_int(pk, "afi", (int64_t)afi);
}

void isis_db_if_af_cfg_defaults(isis_if_af_cfg_t *cfg)
{
    if (!cfg)
    {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 0u;
    cfg->metric = ISIS_DEFAULT_IF_METRIC;
    cfg->hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    cfg->hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    cfg->passive = ISIS_DEFAULT_IF_PASSIVE;
}

void isis_db_if_af_cfg_sanitize(isis_if_af_cfg_t *cfg)
{
    if (!cfg)
    {
        return;
    }

    cfg->enabled = cfg->enabled ? 1u : 0u;
    if (cfg->metric == 0u)
    {
        cfg->metric = ISIS_DEFAULT_IF_METRIC;
    }
    if (cfg->metric > ISIS_MAX_IF_METRIC)
    {
        cfg->metric = ISIS_MAX_IF_METRIC;
    }
    if (cfg->hello_interval == 0u)
    {
        cfg->hello_interval = ISIS_DEFAULT_HELLO_INTERVAL;
    }
    if (cfg->hold_multiplier == 0u)
    {
        cfg->hold_multiplier = ISIS_DEFAULT_HOLD_MULTIPLIER;
    }
    if (cfg->hold_multiplier > ISIS_MAX_HOLD_MULTIPLIER)
    {
        cfg->hold_multiplier = ISIS_MAX_HOLD_MULTIPLIER;
    }
    cfg->passive = cfg->passive ? 1u : 0u;
}

void isis_db_if_cfg_defaults(isis_if_cfg_t *cfg, const char *ifname)
{
    if (!cfg)
    {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    if (ifname)
    {
        g_strlcpy(cfg->ifname, ifname, sizeof(cfg->ifname));
    }
    isis_db_if_af_cfg_defaults(&cfg->v4);
    isis_db_if_af_cfg_defaults(&cfg->v6);
    cfg->last_hello_tx_msec = 0u;
}

void isis_db_if_apply_row_to_af(db_row_t *row, isis_if_af_cfg_t *af_cfg)
{
    if (!row || !af_cfg)
    {
        return;
    }

    af_cfg->enabled = db_row_get_int(row, "enabled", 1) ? 1u : 0u;

    int64_t metric = db_row_get_int(row, "metric", ISIS_DEFAULT_IF_METRIC);
    int64_t hello = db_row_get_int(row, "hello_interval", ISIS_DEFAULT_HELLO_INTERVAL);
    int64_t hold = db_row_get_int(row, "hold_multiplier", ISIS_DEFAULT_HOLD_MULTIPLIER);
    int64_t passive = db_row_get_int(row, "passive", ISIS_DEFAULT_IF_PASSIVE);

    if (metric > 0 && metric <= ISIS_MAX_IF_METRIC)
    {
        af_cfg->metric = (uint32_t)metric;
    }
    if (hello > 0 && hello <= ISIS_MAX_HELLO_INTERVAL)
    {
        af_cfg->hello_interval = (uint16_t)hello;
    }
    if (hold > 0 && hold <= ISIS_MAX_HOLD_MULTIPLIER)
    {
        af_cfg->hold_multiplier = (uint8_t)hold;
    }
    af_cfg->passive = passive ? 1u : 0u;
    isis_db_if_af_cfg_sanitize(af_cfg);
}
