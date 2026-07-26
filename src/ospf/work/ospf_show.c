/**
 * @file   ospf_show.c
 * @brief  OSPFv2 operational CLI output
 */
#include "ospf_show.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "if.h"
#include "ospf_cli.h"
#include "ospf_main.h"
#include "ospf_worker.h"

typedef struct ospf_show_options
{
    uint32_t process_id;
    gboolean has_process;
    gboolean verbose;
} ospf_show_options_t;

static cli_chunk_stream_t g_ospf_show_stream;

static int ospf_show_send(dev_ipc_message_t *msg, GString *text)
{
    return cli_chunk_stream_start(&g_ospf_show_stream, ospf_local_ipc_ctx(), DEV_MODULE_ID_OSPF, msg, text);
}

static int ospf_show_send_text(dev_ipc_message_t *msg, const char *text)
{
    return ospf_show_send(msg, g_string_new(text ? text : ""));
}

static void ospf_show_format_ipv4(uint32_t address, char *buf, size_t len)
{
    if (!buf || len == 0u)
    {
        return;
    }

    struct in_addr in = {
        .s_addr = htonl(address),
    };
    if (!inet_ntop(AF_INET, &in, buf, (socklen_t)len))
    {
        g_strlcpy(buf, "-", len);
    }
}

static void ospf_show_format_net_addr(const net_addr_t *address, char *buf, size_t len)
{
    if (!buf || len == 0u)
    {
        return;
    }

    g_strlcpy(buf, "-", len);
    if (address && address->family == AF_INET)
    {
        net_addr_to_str(address, buf, len);
    }
}

static void ospf_show_format_interface(uint32_t ifindex, char *buf, size_t len)
{
    if (!buf || len == 0u)
    {
        return;
    }

    const char *ifname = if_api_cache_get_logical_name(ifindex);
    if (ifname && ifname[0] != '\0')
    {
        g_strlcpy(buf, ifname, len);
    }
    else if (ifindex != 0u)
    {
        g_snprintf(buf, len, "%u", ifindex);
    }
    else
    {
        g_strlcpy(buf, "-", len);
    }
}

static const char *ospf_show_interface_state(uint8_t state)
{
    switch ((ospf_interface_state_t)state)
    {
        case OSPF_IF_STATE_DOWN:
            return "Down";
        case OSPF_IF_STATE_WAITING:
            return "Waiting";
        case OSPF_IF_STATE_POINT_TO_POINT:
            return "PointToPoint";
        case OSPF_IF_STATE_DR_OTHER:
            return "DROther";
        case OSPF_IF_STATE_BACKUP:
            return "Backup";
        case OSPF_IF_STATE_DR:
            return "DR";
        default:
            return "Unknown";
    }
}

static const char *ospf_show_network_type(uint8_t type)
{
    switch (type)
    {
        case OSPF_NETWORK_BROADCAST:
            return "Broadcast";
        case OSPF_NETWORK_POINT_TO_POINT:
            return "PointToPoint";
        default:
            return "Unknown";
    }
}

static const char *ospf_show_neighbor_state(uint8_t state)
{
    switch ((ospf_neighbor_state_t)state)
    {
        case OSPF_NBR_STATE_DOWN:
            return "Down";
        case OSPF_NBR_STATE_INIT:
            return "Init";
        case OSPF_NBR_STATE_TWO_WAY:
            return "2-Way";
        case OSPF_NBR_STATE_EXSTART:
            return "ExStart";
        case OSPF_NBR_STATE_EXCHANGE:
            return "Exchange";
        case OSPF_NBR_STATE_LOADING:
            return "Loading";
        case OSPF_NBR_STATE_FULL:
            return "Full";
        default:
            return "Unknown";
    }
}

static const char *ospf_show_lsa_type(uint8_t type)
{
    switch (type)
    {
        case OSPF_LSA_ROUTER:
            return "Router";
        case OSPF_LSA_NETWORK:
            return "Network";
        default:
            return "Unknown";
    }
}

static gint ospf_show_instance_compare(gconstpointer a, gconstpointer b)
{
    const ospf_instance_t *left = *(const ospf_instance_t *const *)a;
    const ospf_instance_t *right = *(const ospf_instance_t *const *)b;
    return (left->process_id > right->process_id) - (left->process_id < right->process_id);
}

static gint ospf_show_interface_compare(gconstpointer a, gconstpointer b)
{
    const ospf_if_cfg_t *left = *(const ospf_if_cfg_t *const *)a;
    const ospf_if_cfg_t *right = *(const ospf_if_cfg_t *const *)b;
    return g_strcmp0(left->ifname, right->ifname);
}

static gint ospf_show_neighbor_compare(gconstpointer a, gconstpointer b)
{
    const ospf_neighbor_t *left = *(const ospf_neighbor_t *const *)a;
    const ospf_neighbor_t *right = *(const ospf_neighbor_t *const *)b;

    if (left->router_id != right->router_id)
    {
        return (left->router_id > right->router_id) - (left->router_id < right->router_id);
    }
    if (left->src_addr != right->src_addr)
    {
        return (left->src_addr > right->src_addr) - (left->src_addr < right->src_addr);
    }
    return g_strcmp0(left->ifname, right->ifname);
}

static gint ospf_show_lsa_compare(gconstpointer a, gconstpointer b)
{
    const ospf_lsa_entry_t *left = *(const ospf_lsa_entry_t *const *)a;
    const ospf_lsa_entry_t *right = *(const ospf_lsa_entry_t *const *)b;

    if (left->area_id != right->area_id)
    {
        return (left->area_id > right->area_id) - (left->area_id < right->area_id);
    }
    if (left->type != right->type)
    {
        return (left->type > right->type) - (left->type < right->type);
    }
    if (left->link_state_id != right->link_state_id)
    {
        return (left->link_state_id > right->link_state_id) - (left->link_state_id < right->link_state_id);
    }
    return (left->advertising_router > right->advertising_router) -
           (left->advertising_router < right->advertising_router);
}

static gint ospf_show_route_compare(gconstpointer a, gconstpointer b)
{
    const ospf_route_t *left = *(const ospf_route_t *const *)a;
    const ospf_route_t *right = *(const ospf_route_t *const *)b;
    int cmp = net_addr_cmp(&left->prefix, &right->prefix);
    if (cmp != 0)
    {
        return cmp;
    }
    if (left->prefix_len != right->prefix_len)
    {
        return (left->prefix_len > right->prefix_len) - (left->prefix_len < right->prefix_len);
    }
    if (left->advertising_router != right->advertising_router)
    {
        return (left->advertising_router > right->advertising_router) -
               (left->advertising_router < right->advertising_router);
    }
    return (left->metric > right->metric) - (left->metric < right->metric);
}

static GPtrArray *ospf_show_collect_values(GHashTable *table, GCompareFunc compare)
{
    GPtrArray *values = g_ptr_array_new();
    if (!values)
    {
        return NULL;
    }

    if (table)
    {
        GHashTableIter iter;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, table);
        while (g_hash_table_iter_next(&iter, NULL, &value))
        {
            if (value)
            {
                g_ptr_array_add(values, value);
            }
        }
    }
    if (compare)
    {
        g_ptr_array_sort(values, compare);
    }
    return values;
}

static GPtrArray *ospf_show_collect_instances(const ospf_show_options_t *options)
{
    GPtrArray *instances = g_ptr_array_new();
    if (!instances)
    {
        return NULL;
    }
    if (!g_ospf_work_local || !g_ospf_work_local->instances)
    {
        return instances;
    }

    if (options && options->has_process)
    {
        ospf_instance_t *inst = ospf_lookup_instance(options->process_id);
        if (inst)
        {
            g_ptr_array_add(instances, inst);
        }
        return instances;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_ospf_work_local->instances);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        if (value)
        {
            g_ptr_array_add(instances, value);
        }
    }
    g_ptr_array_sort(instances, ospf_show_instance_compare);
    return instances;
}

static int ospf_show_parse_options(cli_tlv_parser_t *parser, gboolean allow_verbose, ospf_show_options_t *options)
{
    if (!parser || !options)
    {
        return ERRCODE_FAIL;
    }

    memset(options, 0, sizeof(*options));
    for (;;)
    {
        cli_tlv_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        int next = cli_tlv_next(parser, &entry);
        if (next == 0)
        {
            return ERRCODE_SUCCESS;
        }
        if (next < 0)
        {
            cli_tlv_entry_free(&entry);
            return ERRCODE_FAIL;
        }

        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_OSPF_PROCESS)
            {
                uint32_t process_id = cli_tlv_entry_get_ctx_uint32(&entry);
                if (process_id != 0u)
                {
                    options->process_id = process_id;
                    options->has_process = TRUE;
                }
            }
        }
        else if (entry.cfg_id == 1u)
        {
            uint32_t process_id = 0u;
            if (cli_tlv_entry_get_u32(&entry, &process_id) != 0 || process_id == 0u)
            {
                cli_tlv_entry_free(&entry);
                return ERRCODE_FAIL;
            }
            options->process_id = process_id;
            options->has_process = TRUE;
        }
        else if (allow_verbose && entry.cfg_id == 2u)
        {
            options->verbose = TRUE;
        }
        cli_tlv_entry_free(&entry);
    }
}

static guint ospf_show_full_neighbor_count(const ospf_instance_t *inst)
{
    guint count = 0u;
    if (!inst || !inst->neighbors)
    {
        return count;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->neighbors);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const ospf_neighbor_t *neighbor = (const ospf_neighbor_t *)value;
        if (neighbor && neighbor->state == OSPF_NBR_STATE_FULL)
        {
            count++;
        }
    }
    return count;
}

static int ospf_show_summary(dev_ipc_message_t *msg, const ospf_show_options_t *options)
{
    GString *out = g_string_new("\r\nOSPFv2 Summary\r\n");
    GPtrArray *instances = ospf_show_collect_instances(options);
    if (!out || !instances)
    {
        if (out)
        {
            g_string_free(out, TRUE);
        }
        if (instances)
        {
            g_ptr_array_free(instances, TRUE);
        }
        return ERRCODE_FAIL;
    }

#define OSPF_SUMMARY_FMT "%-8s %-15s %-15s %-7s %7s %10s %15s %8s %8s\r\n"
    g_string_append_printf(out, OSPF_SUMMARY_FMT, "Process", "VRF", "Router-ID", "Admin", "Areas", "Interfaces",
                           "Full/Total", "LSAs", "Routes");
    g_string_append_printf(out, OSPF_SUMMARY_FMT, "--------", "---------------", "---------------", "-------",
                           "-------", "----------", "---------------", "--------", "--------");

    for (guint i = 0u; i < instances->len; ++i)
    {
        const ospf_instance_t *inst = g_ptr_array_index(instances, i);
        char process[16];
        char router_id[INET_ADDRSTRLEN];
        char areas[16];
        char interfaces[16];
        char neighbors[32];
        char lsas[16];
        char routes[16];
        guint neighbor_total = inst->neighbors ? g_hash_table_size(inst->neighbors) : 0u;

        g_snprintf(process, sizeof(process), "%u", inst->process_id);
        ospf_show_format_ipv4(inst->router_id, router_id, sizeof(router_id));
        g_snprintf(areas, sizeof(areas), "%u", inst->areas ? g_hash_table_size(inst->areas) : 0u);
        g_snprintf(interfaces, sizeof(interfaces), "%u", inst->if_cfgs ? g_hash_table_size(inst->if_cfgs) : 0u);
        g_snprintf(neighbors, sizeof(neighbors), "%u/%u", ospf_show_full_neighbor_count(inst), neighbor_total);
        g_snprintf(lsas, sizeof(lsas), "%u", inst->lsdb ? g_hash_table_size(inst->lsdb) : 0u);
        g_snprintf(routes, sizeof(routes), "%u", inst->routes ? g_hash_table_size(inst->routes) : 0u);
        g_string_append_printf(out, OSPF_SUMMARY_FMT, process, inst->vrf_name, router_id,
                               inst->admin_up ? "Up" : "Down", areas, interfaces, neighbors, lsas, routes);
    }

#undef OSPF_SUMMARY_FMT

    if (instances->len == 0u)
    {
        g_string_append(out, "(no OSPF process)\r\n");
    }
    g_string_append_printf(out, "\r\nTotal %u process(es)\r\n", instances->len);
    g_ptr_array_free(instances, TRUE);
    return ospf_show_send(msg, out);
}

static int ospf_show_interfaces(dev_ipc_message_t *msg, const ospf_show_options_t *options)
{
    GString *out = g_string_new("\r\nOSPFv2 Interfaces\r\n");
    GPtrArray *instances = ospf_show_collect_instances(options);
    if (!out || !instances)
    {
        if (out)
        {
            g_string_free(out, TRUE);
        }
        if (instances)
        {
            g_ptr_array_free(instances, TRUE);
        }
        return ERRCODE_FAIL;
    }

    g_string_append(
        out, "Process  Interface       Area             State         Type           Cost  Hello(s) Dead(s)\r\n"
             "--------  --------------  ---------------  ------------  -------------  -----  -------- -------\r\n");

    guint count = 0u;
    for (guint i = 0u; i < instances->len; ++i)
    {
        const ospf_instance_t *inst = g_ptr_array_index(instances, i);
        GPtrArray *interfaces = ospf_show_collect_values(inst->if_cfgs, ospf_show_interface_compare);
        if (!interfaces)
        {
            g_ptr_array_free(instances, TRUE);
            g_string_free(out, TRUE);
            return ERRCODE_FAIL;
        }

        for (guint j = 0u; j < interfaces->len; ++j)
        {
            const ospf_if_cfg_t *cfg = g_ptr_array_index(interfaces, j);
            char area[INET_ADDRSTRLEN];
            ospf_show_format_ipv4(cfg->area_id, area, sizeof(area));
            g_string_append_printf(out, "%-8u  %-14s  %-15s  %-12s  %-13s  %5u  %8u %7u\r\n", inst->process_id,
                                   cfg->ifname, area, ospf_show_interface_state(cfg->state),
                                   ospf_show_network_type(cfg->network_type), (unsigned)cfg->cost,
                                   (unsigned)cfg->hello_interval, (unsigned)cfg->dead_interval);
            count++;
        }
        g_ptr_array_free(interfaces, TRUE);
    }

    if (count == 0u)
    {
        g_string_append(out, "(no OSPF interface)\r\n");
    }
    g_string_append_printf(out, "\r\nTotal %u interface(s)\r\n", count);
    g_ptr_array_free(instances, TRUE);
    return ospf_show_send(msg, out);
}

static uint32_t ospf_show_neighbor_dead_remaining(const ospf_neighbor_t *neighbor, uint64_t now_msec)
{
    if (!neighbor || neighbor->last_seen_msec == 0u)
    {
        return 0u;
    }

    uint64_t deadline = neighbor->last_seen_msec + (uint64_t)neighbor->dead_interval * 1000u;
    if (deadline <= now_msec)
    {
        return 0u;
    }
    return (uint32_t)((deadline - now_msec + 999u) / 1000u);
}

static int ospf_show_neighbors(dev_ipc_message_t *msg, const ospf_show_options_t *options)
{
    GString *out = g_string_new("\r\nOSPFv2 Neighbors\r\n");
    GPtrArray *instances = ospf_show_collect_instances(options);
    if (!out || !instances)
    {
        if (out)
        {
            g_string_free(out, TRUE);
        }
        if (instances)
        {
            g_ptr_array_free(instances, TRUE);
        }
        return ERRCODE_FAIL;
    }

    if (options->verbose)
    {
        g_string_append(out, "Process  Router-ID        Address          Interface       State      Dead(s) Pri DR     "
                             "         BDR             DD-Seq     Requests\r\n"
                             "--------  ---------------  ---------------  --------------  ---------  ------- --- "
                             "--------------- --------------- ---------- --------\r\n");
    }
    else
    {
        g_string_append(out, "Process  Router-ID        Address          Interface       State      Dead(s)\r\n"
                             "--------  ---------------  ---------------  --------------  ---------  -------\r\n");
    }

    guint count = 0u;
    uint64_t now_msec = ospf_now_msec();
    for (guint i = 0u; i < instances->len; ++i)
    {
        const ospf_instance_t *inst = g_ptr_array_index(instances, i);
        GPtrArray *neighbors = ospf_show_collect_values(inst->neighbors, ospf_show_neighbor_compare);
        if (!neighbors)
        {
            g_ptr_array_free(instances, TRUE);
            g_string_free(out, TRUE);
            return ERRCODE_FAIL;
        }

        for (guint j = 0u; j < neighbors->len; ++j)
        {
            const ospf_neighbor_t *neighbor = g_ptr_array_index(neighbors, j);
            char router_id[INET_ADDRSTRLEN];
            char address[INET_ADDRSTRLEN];
            ospf_show_format_ipv4(neighbor->router_id, router_id, sizeof(router_id));
            ospf_show_format_ipv4(neighbor->src_addr, address, sizeof(address));

            if (options->verbose)
            {
                char dr[INET_ADDRSTRLEN];
                char bdr[INET_ADDRSTRLEN];
                ospf_show_format_ipv4(neighbor->dr, dr, sizeof(dr));
                ospf_show_format_ipv4(neighbor->bdr, bdr, sizeof(bdr));
                g_string_append_printf(
                    out, "%-8u  %-15s  %-15s  %-14s  %-9s  %7u %3u %-15s %-15s 0x%08x %8u\r\n", inst->process_id,
                    router_id, address, neighbor->ifname, ospf_show_neighbor_state(neighbor->state),
                    ospf_show_neighbor_dead_remaining(neighbor, now_msec), (unsigned)neighbor->priority, dr, bdr,
                    neighbor->dd_sequence, neighbor->request_keys ? neighbor->request_keys->len : 0u);
            }
            else
            {
                g_string_append_printf(out, "%-8u  %-15s  %-15s  %-14s  %-9s  %7u\r\n", inst->process_id, router_id,
                                       address, neighbor->ifname, ospf_show_neighbor_state(neighbor->state),
                                       ospf_show_neighbor_dead_remaining(neighbor, now_msec));
            }
            count++;
        }
        g_ptr_array_free(neighbors, TRUE);
    }

    if (count == 0u)
    {
        g_string_append(out, "(no OSPF neighbor)\r\n");
    }
    g_string_append_printf(out, "\r\nTotal %u neighbor(s)\r\n", count);
    g_ptr_array_free(instances, TRUE);
    return ospf_show_send(msg, out);
}

static uint16_t ospf_show_lsa_age(const ospf_lsa_entry_t *entry, uint64_t now_msec)
{
    uint64_t elapsed = 0u;
    if (entry && now_msec >= entry->installed_msec)
    {
        elapsed = (now_msec - entry->installed_msec) / 1000u;
    }
    uint64_t age = entry ? (uint64_t)entry->age + elapsed : 0u;
    return age >= OSPF_LSA_MAX_AGE ? OSPF_LSA_MAX_AGE : (uint16_t)age;
}

static int ospf_show_lsdb(dev_ipc_message_t *msg, const ospf_show_options_t *options)
{
    GString *out = g_string_new("\r\nOSPFv2 Link State Database\r\n");
    GPtrArray *instances = ospf_show_collect_instances(options);
    if (!out || !instances)
    {
        if (out)
        {
            g_string_free(out, TRUE);
        }
        if (instances)
        {
            g_ptr_array_free(instances, TRUE);
        }
        return ERRCODE_FAIL;
    }

    g_string_append(
        out, "Process  Area             Type     Link State ID    Advertising Router Seq        Age  Checksum\r\n"
             "--------  ---------------  -------  ---------------  ------------------ ---------- ---- --------\r\n");

    guint count = 0u;
    uint64_t now_msec = ospf_now_msec();
    for (guint i = 0u; i < instances->len; ++i)
    {
        const ospf_instance_t *inst = g_ptr_array_index(instances, i);
        GPtrArray *entries = ospf_show_collect_values(inst->lsdb, ospf_show_lsa_compare);
        if (!entries)
        {
            g_ptr_array_free(instances, TRUE);
            g_string_free(out, TRUE);
            return ERRCODE_FAIL;
        }

        for (guint j = 0u; j < entries->len; ++j)
        {
            const ospf_lsa_entry_t *entry = g_ptr_array_index(entries, j);
            char area[INET_ADDRSTRLEN];
            char link_state_id[INET_ADDRSTRLEN];
            char advertising_router[INET_ADDRSTRLEN];
            ospf_show_format_ipv4(entry->area_id, area, sizeof(area));
            ospf_show_format_ipv4(entry->link_state_id, link_state_id, sizeof(link_state_id));
            ospf_show_format_ipv4(entry->advertising_router, advertising_router, sizeof(advertising_router));
            g_string_append_printf(out, "%-8u  %-15s  %-7s  %-15s  %-18s 0x%08x %4u 0x%04x\r\n", inst->process_id, area,
                                   ospf_show_lsa_type(entry->type), link_state_id, advertising_router, entry->sequence,
                                   (unsigned)ospf_show_lsa_age(entry, now_msec), (unsigned)entry->checksum);
            count++;
        }
        g_ptr_array_free(entries, TRUE);
    }

    if (count == 0u)
    {
        g_string_append(out, "(no OSPF LSA)\r\n");
    }
    g_string_append_printf(out, "\r\nTotal %u LSA(s)\r\n", count);
    g_ptr_array_free(instances, TRUE);
    return ospf_show_send(msg, out);
}

static int ospf_show_routes(dev_ipc_message_t *msg, const ospf_show_options_t *options)
{
    GString *out = g_string_new("\r\nOSPFv2 Routes\r\n");
    GPtrArray *instances = ospf_show_collect_instances(options);
    if (!out || !instances)
    {
        if (out)
        {
            g_string_free(out, TRUE);
        }
        if (instances)
        {
            g_ptr_array_free(instances, TRUE);
        }
        return ERRCODE_FAIL;
    }

    g_string_append(out,
                    "Process  Prefix              Metric  Nexthop          Interface       Advertising-Source\r\n"
                    "--------  ------------------  ------  ---------------  --------------  ------------------\r\n");

    guint count = 0u;
    for (guint i = 0u; i < instances->len; ++i)
    {
        const ospf_instance_t *inst = g_ptr_array_index(instances, i);
        GPtrArray *routes = ospf_show_collect_values(inst->routes, ospf_show_route_compare);
        if (!routes)
        {
            g_ptr_array_free(instances, TRUE);
            g_string_free(out, TRUE);
            return ERRCODE_FAIL;
        }

        for (guint j = 0u; j < routes->len; ++j)
        {
            const ospf_route_t *route = g_ptr_array_index(routes, j);
            char prefix_address[INET_ADDRSTRLEN];
            char prefix[32];
            char nexthop[INET_ADDRSTRLEN];
            char interface[IF_LOGICAL_NAME_MAX];
            char source[INET_ADDRSTRLEN];
            ospf_show_format_net_addr(&route->prefix, prefix_address, sizeof(prefix_address));
            g_snprintf(prefix, sizeof(prefix), "%s/%u", prefix_address, (unsigned)route->prefix_len);
            ospf_show_format_net_addr(&route->nexthop, nexthop, sizeof(nexthop));
            ospf_show_format_interface(route->out_ifindex, interface, sizeof(interface));
            ospf_show_format_ipv4(route->advertising_router, source, sizeof(source));
            g_string_append_printf(out, "%-8u  %-18s  %6u  %-15s  %-14s  %-18s\r\n", inst->process_id, prefix,
                                   route->metric, nexthop, interface, source);
            count++;
        }
        g_ptr_array_free(routes, TRUE);
    }

    if (count == 0u)
    {
        g_string_append(out, "(no OSPF route)\r\n");
    }
    g_string_append_printf(out, "\r\nTotal %u route(s)\r\n", count);
    g_ptr_array_free(instances, TRUE);
    return ospf_show_send(msg, out);
}

int ospf_show_handle_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        int rc = cli_chunk_stream_continue(&g_ospf_show_stream, ospf_local_ipc_ctx(), DEV_MODULE_ID_OSPF, msg);
        dev_ipc_message_free(msg);
        return rc;
    }

    if (msg->msg_type != CLI_MSG_TYPE || !msg->payload)
    {
        dev_ipc_message_free(msg);
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        int rc = ospf_show_send_text(msg, "OSPF Error: invalid show payload\r\n");
        dev_ipc_message_free(msg);
        return rc;
    }

    ospf_show_options_t options;
    gboolean allow_verbose = parser.group_id == OSPF_CLI_GROUP_SHOW_NEIGHBOR;
    int rc = ospf_show_parse_options(&parser, allow_verbose, &options);
    if (rc != ERRCODE_SUCCESS)
    {
        rc = ospf_show_send_text(msg, "OSPF Error: invalid show filter\r\n");
    }
    else
    {
        switch (parser.group_id)
        {
            case OSPF_CLI_GROUP_SHOW_SUMMARY:
                rc = ospf_show_summary(msg, &options);
                break;
            case OSPF_CLI_GROUP_SHOW_INTERFACE:
                rc = ospf_show_interfaces(msg, &options);
                break;
            case OSPF_CLI_GROUP_SHOW_NEIGHBOR:
                rc = ospf_show_neighbors(msg, &options);
                break;
            case OSPF_CLI_GROUP_SHOW_LSDB:
                rc = ospf_show_lsdb(msg, &options);
                break;
            case OSPF_CLI_GROUP_SHOW_ROUTE:
                rc = ospf_show_routes(msg, &options);
                break;
            default:
                rc = ospf_show_send_text(msg, "OSPF Error: unknown show command\r\n");
                break;
        }
    }

    cli_tlv_cleanup(&parser);
    dev_ipc_message_free(msg);
    return rc;
}

void ospf_show_cleanup(void)
{
    cli_chunk_stream_reset(&g_ospf_show_stream);
}
