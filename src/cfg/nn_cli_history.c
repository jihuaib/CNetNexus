#include "nn_cli_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Session History Implementation
// ============================================================================

void nn_cli_session_history_init(nn_cli_session_history_t *history)
{
    if (!history)
    {
        return;
    }
    memset(history, 0, sizeof(nn_cli_session_history_t));
    history->browse_idx = -1;
}

void nn_cli_session_history_add(nn_cli_session_history_t *history, const char *cmd, const char *client_ip)
{
    if (!history || !cmd || strlen(cmd) == 0)
    {
        return;
    }

    // Avoid consecutive duplicate commands
    if (history->count > 0)
    {
        uint32_t last_idx = (history->current_idx - 1 + NN_CLI_SESSION_HISTORY_SIZE) % NN_CLI_SESSION_HISTORY_SIZE;
        nn_cli_history_entry_t *last_entry = &history->entries[last_idx];
        if (last_entry->command && strcmp(last_entry->command, cmd) == 0)
        {
            return;
        }
    }

    nn_cli_history_entry_t *entry = &history->entries[history->current_idx];

    if (entry->command)
    {
        free(entry->command);
    }

    entry->command = strdup(cmd);
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

    history->current_idx = (history->current_idx + 1) % NN_CLI_SESSION_HISTORY_SIZE;
    if (history->count < NN_CLI_SESSION_HISTORY_SIZE)
    {
        history->count++;
    }
}

const char *nn_cli_session_history_get(nn_cli_session_history_t *history, uint32_t relative_idx)
{
    const nn_cli_history_entry_t *entry = nn_cli_session_history_get_entry(history, relative_idx);
    return entry ? entry->command : NULL;
}

const nn_cli_history_entry_t *nn_cli_session_history_get_entry(nn_cli_session_history_t *history, uint32_t relative_idx)
{
    if (!history || history->count == 0 || relative_idx >= history->count)
    {
        return NULL;
    }
    uint32_t actual_idx =
        (history->current_idx - 1 - relative_idx + NN_CLI_SESSION_HISTORY_SIZE) % NN_CLI_SESSION_HISTORY_SIZE;
    return &history->entries[actual_idx];
}

void nn_cli_session_history_cleanup(nn_cli_session_history_t *history)
{
    if (!history)
    {
        return;
    }
    for (uint32_t i = 0; i < NN_CLI_SESSION_HISTORY_SIZE; i++)
    {
        if (history->entries[i].command)
        {
            free(history->entries[i].command);
            history->entries[i].command = NULL;
        }
    }
}

// ============================================================================
// Global History Implementation
// ============================================================================

void nn_cli_global_history_init(nn_cli_global_history_t *history)
{
    if (!history)
    {
        return;
    }
    memset(history, 0, sizeof(nn_cli_global_history_t));
}

void nn_cli_global_history_add(nn_cli_global_history_t *history, const char *cmd, const char *client_ip)
{
    if (!history || !cmd || strlen(cmd) == 0)
    {
        return;
    }

    nn_cli_history_entry_t *entry = &history->entries[history->current_idx];

    if (entry->command)
    {
        free(entry->command);
    }

    entry->command = strdup(cmd);
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

    history->current_idx = (history->current_idx + 1) % NN_CLI_GLOBAL_HISTORY_SIZE;
    if (history->count < NN_CLI_GLOBAL_HISTORY_SIZE)
    {
        history->count++;
    }
}

const nn_cli_history_entry_t *nn_cli_global_history_get_entry(nn_cli_global_history_t *history, uint32_t relative_idx)
{
    if (!history || history->count == 0 || relative_idx >= history->count)
    {
        return NULL;
    }
    uint32_t actual_idx =
        (history->current_idx - 1 - relative_idx + NN_CLI_GLOBAL_HISTORY_SIZE) % NN_CLI_GLOBAL_HISTORY_SIZE;
    return &history->entries[actual_idx];
}

void nn_cli_global_history_cleanup(nn_cli_global_history_t *history)
{
    if (!history)
    {
        return;
    }
    for (uint32_t i = 0; i < NN_CLI_GLOBAL_HISTORY_SIZE; i++)
    {
        if (history->entries[i].command)
        {
            free(history->entries[i].command);
            history->entries[i].command = NULL;
        }
    }
}
