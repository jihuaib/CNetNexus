/**
 * @file   snmp_agent.c
 * @brief  Net-SNMP generic value store and trap transport
 */
// clang-format off
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
// clang-format on
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "snmp_main.h"

#ifdef LOG_DEBUG
#    undef LOG_DEBUG
#endif
#ifdef LOG_INFO
#    undef LOG_INFO
#endif
#ifdef LOG_PERROR
#    undef LOG_PERROR
#endif
#include "log.h"

#define NETNEXUS_ENTERPRISE 55555
#define NETNEXUS_SNMP_DEFAULT_PORT 161
#define NETNEXUS_SNMP_DEFAULT_COMMUNITY "public"

typedef struct snmp_value_entry
{
    uint32_t owner_module_id;
    uint32_t value_type;
    oid oid_buf[MAX_OID_LEN];
    size_t oid_len;
    char oid_str[SNMP_OID_MAX_LEN];
    char value[SNMP_VALUE_MAX_LEN];
    int registered;
} snmp_value_entry_t;

static GMutex g_snmp_agent_lock;
static GHashTable *g_values_by_oid = NULL; /* key=numeric oid string, value=snmp_value_entry_t* */
static char g_agent_port[32] = "161";
static char g_agent_endpoint[48] = "udp:161";
static char g_community[SNMP_COMMUNITY_MAX_LEN] = NETNEXUS_SNMP_DEFAULT_COMMUNITY;
static char g_trap_host[SNMP_TRAP_HOST_MAX_LEN] = "";
static uint16_t g_trap_port = SNMP_TRAP_DEFAULT_PORT;

static const oid OID_SYS_DESCR[] = {1, 3, 6, 1, 2, 1, 1, 1, 0};
static const oid OID_SYS_OBJECT_ID[] = {1, 3, 6, 1, 2, 1, 1, 2, 0};
static const oid OID_SYS_UPTIME[] = {1, 3, 6, 1, 2, 1, 1, 3, 0};
static const oid OID_SYS_UPTIME_INSTANCE[] = {1, 3, 6, 1, 2, 1, 1, 3, 0};
static const oid OID_SYS_NAME[] = {1, 3, 6, 1, 2, 1, 1, 5, 0};
static const oid OID_NETNEXUS_OBJECT[] = {1, 3, 6, 1, 4, 1, NETNEXUS_ENTERPRISE, 1};
static const oid OID_SNMP_TRAP_OID[] = {1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0};

static void snmp_value_entry_free(gpointer data)
{
    snmp_value_entry_t *entry = (snmp_value_entry_t *)data;
    if (!entry)
    {
        return;
    }
    if (entry->registered)
    {
        (void)unregister_mib(entry->oid_buf, entry->oid_len);
        entry->registered = 0;
    }
    g_free(entry);
}

static int parse_u16_env(const char *name, uint16_t *out)
{
    const char *v = getenv(name);
    if (!v || v[0] == '\0')
    {
        return -1;
    }
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (!end || *end != '\0' || n == 0 || n > 65535)
    {
        LOG_WARN("SNMP: invalid %s=%s", name, v);
        return -1;
    }
    *out = (uint16_t)n;
    return 0;
}

static void load_env_config(void)
{
    uint16_t port = NETNEXUS_SNMP_DEFAULT_PORT;
    if (parse_u16_env("NN_SNMP_AGENT_PORT", &port) == 0)
    {
        snprintf(g_agent_port, sizeof(g_agent_port), "%u", (unsigned)port);
    }
    else
    {
        snprintf(g_agent_port, sizeof(g_agent_port), "%u", NETNEXUS_SNMP_DEFAULT_PORT);
    }

    snprintf(g_agent_endpoint, sizeof(g_agent_endpoint), "udp:%s", g_agent_port);

    const char *community = getenv("NN_SNMP_COMMUNITY");
    if (community && community[0] != '\0')
    {
        g_strlcpy(g_community, community, sizeof(g_community));
    }

    const char *trap_host = getenv("NN_SNMP_TRAP_HOST");
    if (trap_host && trap_host[0] != '\0')
    {
        g_strlcpy(g_trap_host, trap_host, sizeof(g_trap_host));
    }

    (void)parse_u16_env("NN_SNMP_TRAP_PORT", &g_trap_port);
}

static void enable_debug_if_requested(void)
{
    const char *debug = getenv("NN_SNMP_DEBUG");
    if (debug && debug[0] != '\0' && strcmp(debug, "0") != 0)
    {
        snmp_set_do_debugging(1);
        debug_register_tokens("all");
    }
}

static void oid_to_numeric_string(const oid *oid_buf, size_t oid_len, char *buf, size_t buf_len)
{
    size_t used = 0;
    if (!buf || buf_len == 0)
    {
        return;
    }
    buf[0] = '\0';

    for (size_t i = 0; i < oid_len; ++i)
    {
        int n = snprintf(buf + used, buf_len - used, ".%lu", (unsigned long)oid_buf[i]);
        if (n < 0 || (size_t)n >= buf_len - used)
        {
            buf[buf_len - 1] = '\0';
            return;
        }
        used += (size_t)n;
    }
}

static int parse_oid_numeric(const char *oid_str, oid *oid_buf, size_t *oid_len)
{
    if (!oid_str || oid_str[0] == '\0' || !oid_buf || !oid_len)
    {
        return -1;
    }

    char buf[SNMP_OID_MAX_LEN];
    g_strlcpy(buf, oid_str, sizeof(buf));
    char *p = buf;
    while (*p == ' ' || *p == '\t')
    {
        ++p;
    }

    if (*p != '.')
    {
        char dotted[SNMP_OID_MAX_LEN];
        size_t p_len = strlen(p);
        if (p_len + 2 > sizeof(dotted))
        {
            LOG_WARN("SNMP: OID too long %s", oid_str);
            return -1;
        }
        dotted[0] = '.';
        memcpy(dotted + 1, p, p_len + 1);
        g_strlcpy(buf, dotted, sizeof(buf));
        p = buf;
    }

    size_t len = MAX_OID_LEN;
    if (!read_objid(p, oid_buf, &len))
    {
        LOG_WARN("SNMP: invalid OID %s", oid_str);
        return -1;
    }

    *oid_len = len;
    return 0;
}

static void set_request_value(netsnmp_agent_request_info *reqinfo, netsnmp_request_info *request,
                              const snmp_value_entry_t *entry)
{
    if (!request || !entry)
    {
        return;
    }

    switch (entry->value_type)
    {
        case SNMP_VALUE_INTEGER:
        {
            long n = strtol(entry->value, NULL, 10);
            snmp_set_var_typed_value(request->requestvb, ASN_INTEGER, (const u_char *)&n, sizeof(n));
            break;
        }
        case SNMP_VALUE_GAUGE:
        {
            unsigned long n = strtoul(entry->value, NULL, 10);
            snmp_set_var_typed_value(request->requestvb, ASN_GAUGE, (const u_char *)&n, sizeof(n));
            break;
        }
        case SNMP_VALUE_TIMETICKS:
        {
            unsigned long n = strtoul(entry->value, NULL, 10);
            snmp_set_var_typed_value(request->requestvb, ASN_TIMETICKS, (const u_char *)&n, sizeof(n));
            break;
        }
        case SNMP_VALUE_OID:
        {
            oid value_oid[MAX_OID_LEN];
            size_t value_oid_len = MAX_OID_LEN;
            if (read_objid(entry->value, value_oid, &value_oid_len))
            {
                snmp_set_var_typed_value(request->requestvb, ASN_OBJECT_ID, (const u_char *)value_oid,
                                         value_oid_len * sizeof(oid));
            }
            else
            {
                netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT);
            }
            break;
        }
        case SNMP_VALUE_OCTETS:
        case SNMP_VALUE_STRING:
        default:
            snmp_set_var_typed_value(request->requestvb, ASN_OCTET_STR, (const u_char *)entry->value,
                                     strlen(entry->value));
            break;
    }
}

static int generic_value_handler(netsnmp_mib_handler *handler, netsnmp_handler_registration *reginfo,
                                 netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests)
{
    (void)handler;
    if (!reginfo || !reqinfo)
    {
        return SNMP_ERR_NOERROR;
    }

    if (reqinfo->mode != MODE_GET)
    {
        return SNMP_ERR_NOERROR;
    }

    char key[SNMP_OID_MAX_LEN];
    oid_to_numeric_string(reginfo->rootoid, reginfo->rootoid_len, key, sizeof(key));

    g_mutex_lock(&g_snmp_agent_lock);
    snmp_value_entry_t *entry =
        g_values_by_oid ? (snmp_value_entry_t *)g_hash_table_lookup(g_values_by_oid, key) : NULL;
    for (netsnmp_request_info *request = requests; request; request = request->next)
    {
        if (!entry)
        {
            netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT);
            continue;
        }
        set_request_value(reqinfo, request, entry);
    }
    g_mutex_unlock(&g_snmp_agent_lock);

    return SNMP_ERR_NOERROR;
}

static int register_dynamic_oid(snmp_value_entry_t *entry)
{
    if (!entry || entry->registered)
    {
        return 0;
    }

    netsnmp_handler_registration *reg = netsnmp_create_handler_registration(
        entry->oid_str, generic_value_handler, entry->oid_buf, entry->oid_len, HANDLER_CAN_RONLY);
    if (!reg)
    {
        LOG_WARN("SNMP: failed to create registration for %s", entry->oid_str);
        return -1;
    }

    if (netsnmp_register_instance(reg) != SNMP_ERR_NOERROR)
    {
        LOG_WARN("SNMP: failed to register instance %s", entry->oid_str);
        return -1;
    }

    entry->registered = 1;
    return 0;
}

int snmp_agent_value_set(const snmp_value_msg_t *value)
{
    if (!value || value->oid[0] == '\0')
    {
        return -1;
    }

    oid oid_buf[MAX_OID_LEN];
    size_t oid_len = 0;
    if (parse_oid_numeric(value->oid, oid_buf, &oid_len) != 0)
    {
        return -1;
    }

    char key[SNMP_OID_MAX_LEN];
    oid_to_numeric_string(oid_buf, oid_len, key, sizeof(key));

    g_mutex_lock(&g_snmp_agent_lock);
    if (!g_values_by_oid)
    {
        g_values_by_oid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, snmp_value_entry_free);
    }

    snmp_value_entry_t *entry = (snmp_value_entry_t *)g_hash_table_lookup(g_values_by_oid, key);
    if (!entry)
    {
        entry = (snmp_value_entry_t *)g_malloc0(sizeof(*entry));
        if (!entry)
        {
            g_mutex_unlock(&g_snmp_agent_lock);
            return -1;
        }
        g_strlcpy(entry->oid_str, key, sizeof(entry->oid_str));
        memcpy(entry->oid_buf, oid_buf, oid_len * sizeof(oid));
        entry->oid_len = oid_len;
        g_hash_table_insert(g_values_by_oid, g_strdup(entry->oid_str), entry);
    }

    entry->owner_module_id = value->owner_module_id;
    entry->value_type = value->value_type;
    g_strlcpy(entry->value, value->value, sizeof(entry->value));
    int rc = register_dynamic_oid(entry);
    g_mutex_unlock(&g_snmp_agent_lock);

    return rc;
}

static gboolean clear_subtree_iter(gpointer key, gpointer data, gpointer user_data)
{
    (void)key;
    const snmp_subtree_clear_msg_t *clear = (const snmp_subtree_clear_msg_t *)user_data;
    const snmp_value_entry_t *entry = (const snmp_value_entry_t *)data;

    if (!clear || !entry)
    {
        return FALSE;
    }
    if (clear->owner_module_id != 0 && clear->owner_module_id != entry->owner_module_id)
    {
        return FALSE;
    }
    return g_str_has_prefix(entry->oid_str, clear->oid_prefix);
}

int snmp_agent_subtree_clear(const snmp_subtree_clear_msg_t *clear)
{
    if (!clear || clear->oid_prefix[0] == '\0')
    {
        return -1;
    }

    oid prefix_oid[MAX_OID_LEN];
    size_t prefix_len = 0;
    if (parse_oid_numeric(clear->oid_prefix, prefix_oid, &prefix_len) != 0)
    {
        return -1;
    }

    snmp_subtree_clear_msg_t normalized = *clear;
    oid_to_numeric_string(prefix_oid, prefix_len, normalized.oid_prefix, sizeof(normalized.oid_prefix));

    g_mutex_lock(&g_snmp_agent_lock);
    if (g_values_by_oid)
    {
        g_hash_table_foreach_remove(g_values_by_oid, clear_subtree_iter, &normalized);
    }
    g_mutex_unlock(&g_snmp_agent_lock);

    return 0;
}

void snmp_agent_apply_config(const snmp_config_msg_t *cfg)
{
    if (!cfg)
    {
        return;
    }

    g_mutex_lock(&g_snmp_agent_lock);
    if (cfg->trap_enabled && cfg->trap_host[0] != '\0' && cfg->trap_port > 0 && cfg->trap_port <= 65535)
    {
        g_strlcpy(g_trap_host, cfg->trap_host, sizeof(g_trap_host));
        g_trap_port = (uint16_t)cfg->trap_port;
        LOG_INFO("SNMP: trap target set to %s:%u", g_trap_host, (unsigned)g_trap_port);
    }
    else
    {
        g_trap_host[0] = '\0';
        g_trap_port = SNMP_TRAP_DEFAULT_PORT;
        LOG_INFO("SNMP: trap target disabled");
    }
    g_mutex_unlock(&g_snmp_agent_lock);
}

static char snmp_add_var_type(uint32_t value_type)
{
    switch (value_type)
    {
        case SNMP_VALUE_INTEGER:
            return 'i';
        case SNMP_VALUE_GAUGE:
            return 'u';
        case SNMP_VALUE_TIMETICKS:
            return 't';
        case SNMP_VALUE_OID:
            return 'o';
        case SNMP_VALUE_IPADDR:
            return 'a';
        case SNMP_VALUE_OCTETS:
            return 'x';
        case SNMP_VALUE_STRING:
        default:
            return 's';
    }
}

void snmp_agent_send_trap(const snmp_trap_msg_t *trap)
{
    if (!trap || trap->trap_oid[0] == '\0')
    {
        return;
    }

    char trap_host[SNMP_TRAP_HOST_MAX_LEN];
    char community[SNMP_COMMUNITY_MAX_LEN];
    uint16_t trap_port = SNMP_TRAP_DEFAULT_PORT;

    g_mutex_lock(&g_snmp_agent_lock);
    g_strlcpy(trap_host, g_trap_host, sizeof(trap_host));
    g_strlcpy(community, g_community, sizeof(community));
    trap_port = g_trap_port;
    g_mutex_unlock(&g_snmp_agent_lock);

    if (trap_host[0] == '\0')
    {
        return;
    }

    struct snmp_session session;
    snmp_sess_init(&session);
    session.version = SNMP_VERSION_2c;
    session.community = (u_char *)community;
    session.community_len = strlen(community);

    char peer[192];
    snprintf(peer, sizeof(peer), "udp:%s:%u", trap_host, (unsigned)trap_port);
    session.peername = peer;

    struct snmp_session *ss = snmp_open(&session);
    if (!ss)
    {
        LOG_WARN("SNMP: failed to open trap session to %s", peer);
        return;
    }

    netsnmp_pdu *pdu = snmp_pdu_create(SNMP_MSG_TRAP2);
    if (!pdu)
    {
        snmp_close(ss);
        return;
    }

    char value[64];
    snprintf(value, sizeof(value), "%lu", netsnmp_get_agent_uptime());
    (void)snmp_add_var(pdu, OID_SYS_UPTIME_INSTANCE, OID_LENGTH(OID_SYS_UPTIME_INSTANCE), 't', value);
    (void)snmp_add_var(pdu, OID_SNMP_TRAP_OID, OID_LENGTH(OID_SNMP_TRAP_OID), 'o', trap->trap_oid);

    uint32_t count = trap->var_count;
    if (count > SNMP_TRAP_VAR_MAX)
    {
        count = SNMP_TRAP_VAR_MAX;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        oid var_oid[MAX_OID_LEN];
        size_t var_oid_len = 0;
        if (parse_oid_numeric(trap->vars[i].oid, var_oid, &var_oid_len) != 0)
        {
            continue;
        }
        (void)snmp_add_var(pdu, var_oid, var_oid_len, snmp_add_var_type(trap->vars[i].value_type), trap->vars[i].value);
    }

    if (!snmp_send(ss, pdu))
    {
        snmp_free_pdu(pdu);
        LOG_WARN("SNMP: failed to send trap %s to %s", trap->trap_oid, peer);
    }
    else
    {
        LOG_INFO("SNMP: sent trap %s to %s", trap->trap_oid, peer);
    }

    snmp_close(ss);
}

static void set_string(netsnmp_request_info *request, const char *value)
{
    snmp_set_var_typed_value(request->requestvb, ASN_OCTET_STR, (const u_char *)value, strlen(value));
}

static void set_object_id(netsnmp_request_info *request, const oid *value_oid, size_t value_oid_len)
{
    snmp_set_var_typed_value(request->requestvb, ASN_OBJECT_ID, (const u_char *)value_oid, value_oid_len * sizeof(oid));
}

static int oid_eq(const oid *a, size_t a_len, const oid *b, size_t b_len)
{
    return a_len == b_len && memcmp(a, b, a_len * sizeof(oid)) == 0;
}

static int scalar_handler(netsnmp_mib_handler *handler, netsnmp_handler_registration *reginfo,
                          netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests)
{
    (void)handler;
    if (!reginfo || !reqinfo || reqinfo->mode != MODE_GET)
    {
        return SNMP_ERR_NOERROR;
    }

    char hostname[256] = "netnexus";
    (void)gethostname(hostname, sizeof(hostname) - 1);
    hostname[sizeof(hostname) - 1] = '\0';

    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    (void)uname(&uts);

    char descr[512];
    snprintf(descr, sizeof(descr), "NetNexus SNMP agent on %s %s", uts.sysname[0] ? uts.sysname : "Linux",
             uts.release[0] ? uts.release : "");

    for (netsnmp_request_info *request = requests; request; request = request->next)
    {
        if (oid_eq(reginfo->rootoid, reginfo->rootoid_len, OID_SYS_DESCR, OID_LENGTH(OID_SYS_DESCR)))
        {
            set_string(request, descr);
        }
        else if (oid_eq(reginfo->rootoid, reginfo->rootoid_len, OID_SYS_OBJECT_ID, OID_LENGTH(OID_SYS_OBJECT_ID)))
        {
            set_object_id(request, OID_NETNEXUS_OBJECT, OID_LENGTH(OID_NETNEXUS_OBJECT));
        }
        else if (oid_eq(reginfo->rootoid, reginfo->rootoid_len, OID_SYS_UPTIME, OID_LENGTH(OID_SYS_UPTIME)))
        {
            unsigned long ticks = netsnmp_get_agent_uptime();
            snmp_set_var_typed_value(request->requestvb, ASN_TIMETICKS, (const u_char *)&ticks, sizeof(ticks));
        }
        else if (oid_eq(reginfo->rootoid, reginfo->rootoid_len, OID_SYS_NAME, OID_LENGTH(OID_SYS_NAME)))
        {
            set_string(request, hostname);
        }
        else
        {
            netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT);
        }
    }

    return SNMP_ERR_NOERROR;
}

static int register_scalar(const char *name, const oid *root, size_t root_len)
{
    return netsnmp_register_instance(
        netsnmp_create_handler_registration(name, scalar_handler, root, root_len, HANDLER_CAN_RONLY));
}

static void configure_access(void)
{
    char line[256];
    snprintf(line, sizeof(line), "agentAddress %s", g_agent_endpoint);
    (void)read_config_with_type("netnexus-snmp", line);
    (void)read_config_with_type("snmpd", line);

    snprintf(line, sizeof(line), "rocommunity %s", g_community);
    netsnmp_config_remember(g_strdup(line));
}

int snmp_agent_init(void)
{
    load_env_config();
    enable_debug_if_requested();
    SOCK_STARTUP;

    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DISABLE_PERSISTENT_LOAD, 1);
    netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DISABLE_PERSISTENT_SAVE, 1);
    netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_ROLE, 0);
    netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_APP_NO_AUTHORIZATION, 1);
    netsnmp_ds_set_string(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_PORTS, g_agent_endpoint);

    init_agent("netnexus-snmp");
    configure_access();
    init_snmp("netnexus-snmp");

    if (register_scalar("sysDescr", OID_SYS_DESCR, OID_LENGTH(OID_SYS_DESCR)) != SNMP_ERR_NOERROR ||
        register_scalar("sysObjectID", OID_SYS_OBJECT_ID, OID_LENGTH(OID_SYS_OBJECT_ID)) != SNMP_ERR_NOERROR ||
        register_scalar("sysUpTime", OID_SYS_UPTIME, OID_LENGTH(OID_SYS_UPTIME)) != SNMP_ERR_NOERROR ||
        register_scalar("sysName", OID_SYS_NAME, OID_LENGTH(OID_SYS_NAME)) != SNMP_ERR_NOERROR)
    {
        LOG_ERROR("SNMP: failed to register scalar handlers");
        return -1;
    }

    if (!g_values_by_oid)
    {
        g_values_by_oid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, snmp_value_entry_free);
    }

    if (init_master_agent() != 0)
    {
        LOG_ERROR("SNMP: init_master_agent failed on UDP/%s", g_agent_port);
        return -1;
    }

    LOG_INFO("SNMP: agent listening on UDP/%s community=%s trap=%s:%u", g_agent_port, g_community,
             g_trap_host[0] ? g_trap_host : "disabled", (unsigned)g_trap_port);
    return 0;
}

void snmp_agent_loop(volatile sig_atomic_t *shutdown_flag)
{
    while (!shutdown_flag || !*shutdown_flag)
    {
        agent_check_and_process(1);
    }
}

void snmp_agent_shutdown(void)
{
    g_mutex_lock(&g_snmp_agent_lock);
    if (g_values_by_oid)
    {
        g_hash_table_destroy(g_values_by_oid);
        g_values_by_oid = NULL;
    }
    g_mutex_unlock(&g_snmp_agent_lock);

    snmp_shutdown("netnexus-snmp");
    SOCK_CLEANUP;
}
