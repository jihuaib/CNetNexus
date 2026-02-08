/**
 * @file   cli_history.c
 * @brief  CLI 命令历史管理，会话级和全局历史
 * @author jhb
 * @date   2026/01/22
 */
#include "cli_history.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Session History Implementation
// ============================================================================

void cli_session_history_init(cli_session_history_t *history)
{
    if (!history)
    {
        return;
    }
    memset(history, 0, sizeof(cli_session_history_t));
    history->browse_idx = -1;
}

void cli_session_history_add(cli_session_history_t *history, const char *cmd, const char *client_ip)
{
    if (!history || !cmd || strlen(cmd) == 0)
    {
        return;
    }

    // Avoid consecutive duplicate commands
    if (history->count > 0)
    {
        uint32_t last_idx = (history->current_idx - 1 + CLI_SESSION_HISTORY_SIZE) % CLI_SESSION_HISTORY_SIZE;
        cli_history_entry_t *last_entry = &history->entries[last_idx];
        if (last_entry->command && strcmp(last_entry->command, cmd) == 0)
        {
            return;
        }
    }

    cli_history_entry_t *entry = &history->entries[history->current_idx];

    if (entry->command)
    {
        g_free(entry->command);
    }

    entry->command = g_strdup(cmd);
    entry->timestamp = time(NULL);
    if (client_ip)
    {
        strncpy(entry->client_ip, client_ip, MAX_CLIENT_IP_LEN - 1);
        entry->client_ip[MAX_CLIENT_IP_LEN - 1] = '\0';
    }
    else
    {
        strcpy(entry->client_ip, "unknown");
    }

    history->current_idx = (history->current_idx + 1) % CLI_SESSION_HISTORY_SIZE;
    if (history->count < CLI_SESSION_HISTORY_SIZE)
    {
        history->count++;
    }
}

const char *cli_session_history_get(cli_session_history_t *history, uint32_t relative_idx)
{
    const cli_history_entry_t *entry = cli_session_history_get_entry(history, relative_idx);
    return entry ? entry->command : NULL;
}

const cli_history_entry_t *cli_session_history_get_entry(cli_session_history_t *history, uint32_t relative_idx)
{
    if (!history || history->count == 0 || relative_idx >= history->count)
    {
        return NULL;
    }
    uint32_t actual_idx =
        (history->current_idx - 1 - relative_idx + CLI_SESSION_HISTORY_SIZE) % CLI_SESSION_HISTORY_SIZE;
    return &history->entries[actual_idx];
}

void cli_session_history_cleanup(cli_session_history_t *history)
{
    if (!history)
    {
        return;
    }
    for (uint32_t i = 0; i < CLI_SESSION_HISTORY_SIZE; i++)
    {
        if (history->entries[i].command)
        {
            g_free(history->entries[i].command);
            history->entries[i].command = NULL;
        }
    }
}

// ============================================================================
// Global History Implementation
// ============================================================================

void cli_global_history_init(cli_global_history_t *history)
{
    if (!history)
    {
        return;
    }
    memset(history, 0, sizeof(cli_global_history_t));
}

void cli_global_history_add(cli_global_history_t *history, const char *cmd, const char *client_ip)
{
    if (!history || !cmd || strlen(cmd) == 0)
    {
        return;
    }

    cli_history_entry_t *entry = &history->entries[history->current_idx];

    if (entry->command)
    {
        g_free(entry->command);
    }

    entry->command = g_strdup(cmd);
    entry->timestamp = time(NULL);
    if (client_ip)
    {
        strncpy(entry->client_ip, client_ip, MAX_CLIENT_IP_LEN - 1);
        entry->client_ip[MAX_CLIENT_IP_LEN - 1] = '\0';
    }
    else
    {
        strcpy(entry->client_ip, "unknown");
    }

    history->current_idx = (history->current_idx + 1) % CLI_GLOBAL_HISTORY_SIZE;
    if (history->count < CLI_GLOBAL_HISTORY_SIZE)
    {
        history->count++;
    }
}

const cli_history_entry_t *cli_global_history_get_entry(cli_global_history_t *history, uint32_t relative_idx)
{
    if (!history || history->count == 0 || relative_idx >= history->count)
    {
        return NULL;
    }
    uint32_t actual_idx =
        (history->current_idx - 1 - relative_idx + CLI_GLOBAL_HISTORY_SIZE) % CLI_GLOBAL_HISTORY_SIZE;
    return &history->entries[actual_idx];
}

void cli_global_history_cleanup(cli_global_history_t *history)
{
    if (!history)
    {
        return;
    }
    for (uint32_t i = 0; i < CLI_GLOBAL_HISTORY_SIZE; i++)
    {
        if (history->entries[i].command)
        {
            g_free(history->entries[i].command);
            history->entries[i].command = NULL;
        }
    }
}
