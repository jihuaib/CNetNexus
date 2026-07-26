#include "rpm_db.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "rpm_main.h"

static const db_column_def_t RPM_POLICY_COLS[] = {
    {"name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"type_mask", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"revision", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
};

static const db_table_def_t RPM_POLICY_TABLE = {
    .table_name = RPM_TABLE_POLICY,
    .cols = RPM_POLICY_COLS,
    .num_cols = G_N_ELEMENTS(RPM_POLICY_COLS),
};

static const db_column_def_t RPM_NODE_COLS[] = {
    {"node_key", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"policy_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"sequence", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"permit", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"match_mask", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"prefix_addr", DB_TYPE_TEXT, 0, NULL},
    {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"apply_mask", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"local_pref", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"med", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"community", DB_TYPE_TEXT, 0, NULL},
};

static const db_table_def_t RPM_NODE_TABLE = {
    .table_name = RPM_TABLE_NODE,
    .cols = RPM_NODE_COLS,
    .num_cols = G_N_ELEMENTS(RPM_NODE_COLS),
};

static void rpm_db_name_filter(db_filter_builder_t *fb, const char *name)
{
    db_filter_init(fb);
    db_filter_add_text(fb, "name", name);
}

static void rpm_db_node_filter(db_filter_builder_t *fb, const char *name, uint32_t sequence)
{
    char key[RPM_POLICY_NAME_MAX + 24];
    snprintf(key, sizeof(key), "%s:%u", name, sequence);
    db_filter_init(fb);
    db_filter_add_text(fb, "node_key", key);
}

static int rpm_node_compare(gconstpointer a, gconstpointer b)
{
    const rpm_policy_node_t *na = a;
    const rpm_policy_node_t *nb = b;
    return na->sequence < nb->sequence ? -1 : na->sequence > nb->sequence ? 1 : 0;
}

int rpm_db_init(void)
{
    dev_ipc_context_t *ctx = rpm_local_ipc_ctx();
    if (db_rpc_create_table_from_def(ctx, &RPM_POLICY_TABLE) != ERRCODE_SUCCESS ||
        db_rpc_create_table_from_def(ctx, &RPM_NODE_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("RPM: failed to initialize policy tables");
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int rpm_db_get_policy(const char *name, rpm_policy_t *policy)
{
    if (!name || !policy)
    {
        return ERRCODE_FAIL;
    }
    memset(policy, 0, sizeof(*policy));

    db_filter_builder_t fb;
    rpm_db_name_filter(&fb, name);
    db_result_t *result = NULL;
    int rc = db_rpc_query(rpm_local_ipc_ctx(), RPM_TABLE_POLICY, NULL, 0, &fb.filter, &result);
    db_filter_clear(&fb);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return 1;
    }

    g_strlcpy(policy->name, db_row_get_text(result->rows[0], "name", ""), sizeof(policy->name));
    policy->type_mask = (uint32_t)db_row_get_int(result->rows[0], "type_mask", 0);
    policy->revision = (uint32_t)db_row_get_int(result->rows[0], "revision", 1);
    db_result_free(result);

    db_filter_init(&fb);
    db_filter_add_text(&fb, "policy_name", name);
    result = NULL;
    rc = db_rpc_query(rpm_local_ipc_ctx(), RPM_TABLE_NODE, NULL, 0, &fb.filter, &result);
    db_filter_clear(&fb);
    if (rc != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    GArray *nodes = g_array_new(FALSE, FALSE, sizeof(rpm_policy_node_t));
    if (result)
    {
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            rpm_policy_node_t node;
            memset(&node, 0, sizeof(node));
            db_row_t *row = result->rows[i];
            node.sequence = (uint32_t)db_row_get_int(row, "sequence", 0);
            node.permit = db_row_get_int(row, "permit", 1) != 0;
            node.match_mask = (uint32_t)db_row_get_int(row, "match_mask", 0);
            node.apply_mask = (uint32_t)db_row_get_int(row, "apply_mask", 0);
            node.local_pref = (uint32_t)db_row_get_int(row, "local_pref", 0);
            node.med = (uint32_t)db_row_get_int(row, "med", 0);
            g_strlcpy(node.community, db_row_get_text(row, "community", ""), sizeof(node.community));
            const char *addr = db_row_get_text(row, "prefix_addr", "");
            if ((node.match_mask & RPM_MATCH_PREFIX) != 0u && (net_addr_from_str(addr, &node.prefix.addr) != 0))
            {
                LOG_WARN("RPM: invalid stored prefix address policy=%s node=%u", name, node.sequence);
                node.match_mask &= ~RPM_MATCH_PREFIX;
            }
            node.prefix.prefix_len = (uint8_t)db_row_get_int(row, "prefix_len", 0);
            g_array_append_val(nodes, node);
        }
        db_result_free(result);
    }
    g_array_sort(nodes, rpm_node_compare);
    policy->node_count = MIN((uint32_t)nodes->len, (uint32_t)RPM_POLICY_MAX_NODES);
    if (policy->node_count > 0)
    {
        memcpy(policy->nodes, nodes->data, policy->node_count * sizeof(rpm_policy_node_t));
    }
    g_array_free(nodes, TRUE);
    return ERRCODE_SUCCESS;
}

static int rpm_db_bump_revision(const char *name, uint32_t type_mask)
{
    rpm_policy_t current;
    int found = rpm_db_get_policy(name, &current);
    db_filter_builder_t fb;
    rpm_db_name_filter(&fb, name);
    if (found == ERRCODE_SUCCESS)
    {
        db_col_t cols[] = {
            DB_COL_INT("type_mask", type_mask),
            DB_COL_INT("revision", current.revision + 1u),
        };
        int rows = db_rpc_update_cols(rpm_local_ipc_ctx(), RPM_TABLE_POLICY, &fb.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&fb);
        return rows < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
    }
    db_filter_clear(&fb);
    db_col_t cols[] = {
        DB_COL_TEXT("name", name),
        DB_COL_INT("type_mask", type_mask),
        DB_COL_INT("revision", 1),
    };
    return db_rpc_insert_cols(rpm_local_ipc_ctx(), RPM_TABLE_POLICY, cols, G_N_ELEMENTS(cols));
}

int rpm_db_upsert_node(const char *name, uint32_t type_mask, const rpm_policy_node_t *node)
{
    if (!name || !node || rpm_db_bump_revision(name, type_mask) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    db_filter_builder_t fb;
    rpm_db_node_filter(&fb, name, node->sequence);
    gboolean exists = FALSE;
    if (db_rpc_exists(rpm_local_ipc_ctx(), RPM_TABLE_NODE, &fb.filter, &exists) != ERRCODE_SUCCESS)
    {
        db_filter_clear(&fb);
        return ERRCODE_FAIL;
    }
    if (exists)
    {
        db_col_t col = DB_COL_INT("permit", node->permit ? 1 : 0);
        int rows = db_rpc_update_cols(rpm_local_ipc_ctx(), RPM_TABLE_NODE, &fb.filter, &col, 1);
        db_filter_clear(&fb);
        return rows < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
    }
    db_filter_clear(&fb);

    char key[RPM_POLICY_NAME_MAX + 24];
    snprintf(key, sizeof(key), "%s:%u", name, node->sequence);
    db_col_t cols[] = {
        DB_COL_TEXT("node_key", key),
        DB_COL_TEXT("policy_name", name),
        DB_COL_INT("sequence", node->sequence),
        DB_COL_INT("permit", node->permit ? 1 : 0),
    };
    return db_rpc_insert_cols(rpm_local_ipc_ctx(), RPM_TABLE_NODE, cols, G_N_ELEMENTS(cols));
}

int rpm_db_update_node(const char *name, const rpm_policy_node_t *node)
{
    if (!name || !node)
    {
        return ERRCODE_FAIL;
    }
    rpm_policy_t policy;
    if (rpm_db_get_policy(name, &policy) != ERRCODE_SUCCESS ||
        rpm_db_bump_revision(name, policy.type_mask) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    db_filter_builder_t fb;
    rpm_db_node_filter(&fb, name, node->sequence);
    char addr[64] = "";
    if ((node->match_mask & RPM_MATCH_PREFIX) != 0u)
    {
        net_addr_to_str(&node->prefix.addr, addr, sizeof(addr));
    }
    db_col_t cols[] = {
        DB_COL_INT("permit", node->permit ? 1 : 0),
        DB_COL_INT("match_mask", node->match_mask),
        DB_COL_TEXT("prefix_addr", addr),
        DB_COL_INT("prefix_len", node->prefix.prefix_len),
        DB_COL_INT("apply_mask", node->apply_mask),
        DB_COL_INT("local_pref", node->local_pref),
        DB_COL_INT("med", node->med),
        DB_COL_TEXT("community", node->community),
    };
    int rows = db_rpc_update_cols(rpm_local_ipc_ctx(), RPM_TABLE_NODE, &fb.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&fb);
    return rows <= 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
}

int rpm_db_delete_node(const char *name, uint32_t sequence)
{
    rpm_policy_t policy;
    if (rpm_db_get_policy(name, &policy) != ERRCODE_SUCCESS)
    {
        return 0;
    }
    db_filter_builder_t fb;
    rpm_db_node_filter(&fb, name, sequence);
    int rows = db_rpc_delete(rpm_local_ipc_ctx(), RPM_TABLE_NODE, &fb.filter);
    db_filter_clear(&fb);
    if (rows > 0)
    {
        (void)rpm_db_bump_revision(name, policy.type_mask);
    }
    return rows;
}

int rpm_db_delete_policy(const char *name)
{
    db_filter_builder_t fb;
    db_filter_init(&fb);
    db_filter_add_text(&fb, "policy_name", name);
    int node_rows = db_rpc_delete(rpm_local_ipc_ctx(), RPM_TABLE_NODE, &fb.filter);
    db_filter_clear(&fb);
    rpm_db_name_filter(&fb, name);
    int policy_rows = db_rpc_delete(rpm_local_ipc_ctx(), RPM_TABLE_POLICY, &fb.filter);
    db_filter_clear(&fb);
    return node_rows < 0 || policy_rows < 0 ? ERRCODE_FAIL : policy_rows;
}

int rpm_db_list_policies(GPtrArray **out)
{
    if (!out)
    {
        return ERRCODE_FAIL;
    }
    *out = g_ptr_array_new_with_free_func(g_free);
    db_result_t *result = NULL;
    if (db_rpc_query(rpm_local_ipc_ctx(), RPM_TABLE_POLICY, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        g_ptr_array_free(*out, TRUE);
        *out = NULL;
        return ERRCODE_FAIL;
    }
    if (result)
    {
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            const char *name = db_row_get_text(result->rows[i], "name", "");
            rpm_policy_t *policy = g_new0(rpm_policy_t, 1);
            if (rpm_db_get_policy(name, policy) == ERRCODE_SUCCESS)
            {
                g_ptr_array_add(*out, policy);
            }
            else
            {
                g_free(policy);
            }
        }
        db_result_free(result);
    }
    return ERRCODE_SUCCESS;
}

int rpm_db_restore(void)
{
    GPtrArray *policies = NULL;
    if (rpm_db_list_policies(&policies) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    for (guint i = 0; i < policies->len; i++)
    {
        rpm_policy_store(g_ptr_array_index(policies, i));
    }
    g_ptr_array_free(policies, TRUE);
    return ERRCODE_SUCCESS;
}
