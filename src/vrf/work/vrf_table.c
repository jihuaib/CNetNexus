/**
 * @file   vrf_table.c
 * @brief  VRF 内存表实现
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_table.h"

#include <string.h>

static gpointer af_key_make(uint16_t afi)
{
    return GUINT_TO_POINTER((guint32)afi);
}

static void af_state_destroy(gpointer data)
{
    vrf_af_state_t *af = (vrf_af_state_t *)data;
    if (!af)
    {
        return;
    }
    if (af->import_rts)
    {
        g_array_free(af->import_rts, TRUE);
    }
    if (af->export_rts)
    {
        g_array_free(af->export_rts, TRUE);
    }
    if (af->evpn_import_rts)
    {
        g_array_free(af->evpn_import_rts, TRUE);
    }
    if (af->evpn_export_rts)
    {
        g_array_free(af->evpn_export_rts, TRUE);
    }
    g_free(af);
}

static void vrf_entry_destroy(gpointer data)
{
    vrf_entry_t *e = (vrf_entry_t *)data;
    if (!e)
    {
        return;
    }
    if (e->afs)
    {
        g_hash_table_destroy(e->afs);
    }
    g_free(e);
}

static vrf_entry_t *vrf_entry_new(uint32_t vrf_id, const char *name, uint32_t l3vrf_table_id)
{
    vrf_entry_t *e = g_malloc0(sizeof(*e));
    e->vrf_id = vrf_id;
    g_strlcpy(e->name, name ? name : "", sizeof(e->name));
    e->l3vrf_table_id = l3vrf_table_id;
    e->os_state = VRF_OS_STATE_UNKNOWN;
    e->afs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, af_state_destroy);
    return e;
}

void vrf_table_init(vrf_table_t *t)
{
    if (!t)
    {
        return;
    }
    t->by_id = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, vrf_entry_destroy);
    t->by_name = g_hash_table_new(g_str_hash, g_str_equal);
    t->next_id = 1;
}

void vrf_table_destroy(vrf_table_t *t)
{
    if (!t)
    {
        return;
    }
    if (t->by_name)
    {
        g_hash_table_destroy(t->by_name);
        t->by_name = NULL;
    }
    if (t->by_id)
    {
        g_hash_table_destroy(t->by_id);
        t->by_id = NULL;
    }
    t->next_id = 1;
}

vrf_entry_t *vrf_table_create(vrf_table_t *t, const char *name)
{
    if (!t || !name || name[0] == '\0')
    {
        return NULL;
    }
    if (g_hash_table_lookup(t->by_name, name))
    {
        return NULL;
    }
    uint32_t vrf_id = t->next_id++;
    vrf_entry_t *e = vrf_entry_new(vrf_id, name, VRF_OS_TABLE_BASE + vrf_id);
    g_hash_table_insert(t->by_id, GUINT_TO_POINTER(vrf_id), e);
    g_hash_table_insert(t->by_name, e->name, e);
    return e;
}

vrf_entry_t *vrf_table_create_with_id(vrf_table_t *t, uint32_t vrf_id, const char *name, uint32_t l3vrf_table_id)
{
    if (!t || !name || name[0] == '\0')
    {
        return NULL;
    }
    if (g_hash_table_lookup(t->by_id, GUINT_TO_POINTER(vrf_id)) || g_hash_table_lookup(t->by_name, name))
    {
        return NULL;
    }
    vrf_entry_t *e = vrf_entry_new(vrf_id, name, l3vrf_table_id);
    g_hash_table_insert(t->by_id, GUINT_TO_POINTER(vrf_id), e);
    g_hash_table_insert(t->by_name, e->name, e);
    if (vrf_id >= t->next_id)
    {
        t->next_id = vrf_id + 1;
    }
    return e;
}

vrf_entry_t *vrf_table_find_by_id(vrf_table_t *t, uint32_t vrf_id)
{
    if (!t)
    {
        return NULL;
    }
    return g_hash_table_lookup(t->by_id, GUINT_TO_POINTER(vrf_id));
}

vrf_entry_t *vrf_table_find_by_name(vrf_table_t *t, const char *name)
{
    if (!t || !name)
    {
        return NULL;
    }
    return g_hash_table_lookup(t->by_name, name);
}

int vrf_table_delete(vrf_table_t *t, uint32_t vrf_id)
{
    vrf_entry_t *e = vrf_table_find_by_id(t, vrf_id);
    if (!e)
    {
        return -1;
    }
    g_hash_table_remove(t->by_name, e->name);
    g_hash_table_remove(t->by_id, GUINT_TO_POINTER(vrf_id));
    return 0;
}

vrf_af_state_t *vrf_af_find(const vrf_entry_t *e, uint16_t afi)
{
    if (!e || !e->afs)
    {
        return NULL;
    }
    return g_hash_table_lookup(e->afs, af_key_make(afi));
}

vrf_af_state_t *vrf_af_get_or_create(vrf_entry_t *e, uint16_t afi)
{
    vrf_af_state_t *af = vrf_af_find(e, afi);
    if (af)
    {
        return af;
    }
    af = g_malloc0(sizeof(*af));
    af->afi = afi;
    af->import_rts = g_array_new(FALSE, FALSE, sizeof(vrf_rt_t));
    af->export_rts = g_array_new(FALSE, FALSE, sizeof(vrf_rt_t));
    af->evpn_import_rts = g_array_new(FALSE, FALSE, sizeof(vrf_rt_t));
    af->evpn_export_rts = g_array_new(FALSE, FALSE, sizeof(vrf_rt_t));
    g_hash_table_insert(e->afs, af_key_make(afi), af);
    return af;
}

int vrf_af_delete(vrf_entry_t *e, uint16_t afi)
{
    if (!e || !e->afs)
    {
        return -1;
    }
    if (!g_hash_table_contains(e->afs, af_key_make(afi)))
    {
        return -1;
    }
    g_hash_table_remove(e->afs, af_key_make(afi));
    return 0;
}

int vrf_af_set_rd(vrf_af_state_t *af, const vrf_rd_t *rd)
{
    if (!af)
    {
        return -1;
    }
    if (rd)
    {
        af->has_rd = 1;
        af->rd = *rd;
    }
    else
    {
        af->has_rd = 0;
        memset(&af->rd, 0, sizeof(af->rd));
    }
    return 0;
}

static int rt_array_find(const GArray *arr, const vrf_rt_t *rt)
{
    if (!arr || !rt)
    {
        return -1;
    }
    for (guint i = 0; i < arr->len; i++)
    {
        const vrf_rt_t *cur = &g_array_index(arr, vrf_rt_t, i);
        if (memcmp(cur->bytes, rt->bytes, sizeof(rt->bytes)) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

int vrf_af_modify_rt(vrf_af_state_t *af, int direction, uint8_t rt_type, int add, const vrf_rt_t *rt)
{
    if (!af || !rt)
    {
        return -1;
    }
    GArray *arr = NULL;
    if (rt_type == VRF_RT_TYPE_EVPN)
    {
        arr = (direction == 0) ? af->evpn_import_rts : af->evpn_export_rts;
    }
    else
    {
        arr = (direction == 0) ? af->import_rts : af->export_rts;
    }
    if (!arr)
    {
        return -1;
    }
    int idx = rt_array_find(arr, rt);
    if (add)
    {
        if (idx >= 0)
        {
            return 0;
        }
        g_array_append_val(arr, *rt);
    }
    else
    {
        if (idx < 0)
        {
            return 0;
        }
        g_array_remove_index(arr, (guint)idx);
    }
    return 0;
}
