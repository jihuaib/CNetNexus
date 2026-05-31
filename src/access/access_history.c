/**
 * @file   access_history.c
 * @brief  ACCESS 线（终端）命令历史管理实现
 * @author jhb
 * @date   2026/05/30
 */
#include "access_history.h"

#include <glib.h>
#include <string.h>

// ============================================================================
// 会话级历史
// ============================================================================

void access_session_history_init(access_session_history_t *history)
{
    if (!history)
    {
        return;
    }
    memset(history, 0, sizeof(access_session_history_t));
    history->browse_idx = -1;
}

void access_session_history_add(access_session_history_t *history, const char *cmd, const char *client_ip)
{
    if (!history || !cmd || strlen(cmd) == 0)
    {
        return;
    }

    // 避免相邻重复命令
    if (history->count > 0)
    {
        uint32_t last_idx = (history->current_idx - 1 + ACCESS_SESSION_HISTORY_SIZE) % ACCESS_SESSION_HISTORY_SIZE;
        access_history_entry_t *last_entry = &history->entries[last_idx];
        if (last_entry->command && strcmp(last_entry->command, cmd) == 0)
        {
            return;
        }
    }

    access_history_entry_t *entry = &history->entries[history->current_idx];

    if (entry->command)
    {
        g_free(entry->command);
    }

    entry->command = g_strdup(cmd);
    entry->timestamp = time(NULL);
    if (client_ip)
    {
        strncpy(entry->client_ip, client_ip, ACCESS_MAX_CLIENT_IP_LEN - 1);
        entry->client_ip[ACCESS_MAX_CLIENT_IP_LEN - 1] = '\0';
    }
    else
    {
        strcpy(entry->client_ip, "unknown");
    }

    history->current_idx = (history->current_idx + 1) % ACCESS_SESSION_HISTORY_SIZE;
    if (history->count < ACCESS_SESSION_HISTORY_SIZE)
    {
        history->count++;
    }
}

const char *access_session_history_get(access_session_history_t *history, uint32_t relative_idx)
{
    const access_history_entry_t *entry = access_session_history_get_entry(history, relative_idx);
    return entry ? entry->command : NULL;
}

const access_history_entry_t *access_session_history_get_entry(access_session_history_t *history, uint32_t relative_idx)
{
    if (!history || history->count == 0 || relative_idx >= history->count)
    {
        return NULL;
    }
    uint32_t actual_idx =
        (history->current_idx - 1 - relative_idx + ACCESS_SESSION_HISTORY_SIZE) % ACCESS_SESSION_HISTORY_SIZE;
    return &history->entries[actual_idx];
}

void access_session_history_cleanup(access_session_history_t *history)
{
    if (!history)
    {
        return;
    }
    for (uint32_t i = 0; i < ACCESS_SESSION_HISTORY_SIZE; i++)
    {
        if (history->entries[i].command)
        {
            g_free(history->entries[i].command);
            history->entries[i].command = NULL;
        }
    }
}

// ============================================================================
// 全局历史
// ============================================================================

void access_global_history_init(access_global_history_t *history)
{
    if (!history)
    {
        return;
    }
    memset(history, 0, sizeof(access_global_history_t));
}

void access_global_history_add(access_global_history_t *history, const char *cmd, const char *client_ip)
{
    if (!history || !cmd || strlen(cmd) == 0)
    {
        return;
    }

    access_history_entry_t *entry = &history->entries[history->current_idx];

    if (entry->command)
    {
        g_free(entry->command);
    }

    entry->command = g_strdup(cmd);
    entry->timestamp = time(NULL);
    if (client_ip)
    {
        strncpy(entry->client_ip, client_ip, ACCESS_MAX_CLIENT_IP_LEN - 1);
        entry->client_ip[ACCESS_MAX_CLIENT_IP_LEN - 1] = '\0';
    }
    else
    {
        strcpy(entry->client_ip, "unknown");
    }

    history->current_idx = (history->current_idx + 1) % ACCESS_GLOBAL_HISTORY_SIZE;
    if (history->count < ACCESS_GLOBAL_HISTORY_SIZE)
    {
        history->count++;
    }
}

const access_history_entry_t *access_global_history_get_entry(access_global_history_t *history, uint32_t relative_idx)
{
    if (!history || history->count == 0 || relative_idx >= history->count)
    {
        return NULL;
    }
    uint32_t actual_idx =
        (history->current_idx - 1 - relative_idx + ACCESS_GLOBAL_HISTORY_SIZE) % ACCESS_GLOBAL_HISTORY_SIZE;
    return &history->entries[actual_idx];
}

void access_global_history_cleanup(access_global_history_t *history)
{
    if (!history)
    {
        return;
    }
    for (uint32_t i = 0; i < ACCESS_GLOBAL_HISTORY_SIZE; i++)
    {
        if (history->entries[i].command)
        {
            g_free(history->entries[i].command);
            history->entries[i].command = NULL;
        }
    }
}
