/**
 * @file   cli_restore.c
 * @brief  CLI 内部 BDR 配置回放
 */
#include "cli_restore.h"

#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "cli_handler.h"
#include "cli_main.h"
#include "errcode.h"
#include "log.h"
#include "path_utils.h"

#define CLI_RESTORE_MAX_ATTEMPTS 3
#define CLI_RESTORE_FAILURES_FILE "startup-replay-failures.log"

typedef enum cli_restore_startup_mode
{
    CLI_RESTORE_STARTUP_MODE_NONE = 0,
    CLI_RESTORE_STARTUP_MODE_DB,
    CLI_RESTORE_STARTUP_MODE_CFG,
} cli_restore_startup_mode_t;

static void cli_restore_data_dir(char *buf, size_t size)
{
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        snprintf(buf, size, "%s/data", work_dir);
    }
    else
    {
        snprintf(buf, size, "./data");
    }
}

static void cli_restore_failures_path(char *path, size_t path_size)
{
    char data_dir[512];
    cli_restore_data_dir(data_dir, sizeof(data_dir));
    snprintf(path, path_size, "%s/%s", data_dir, CLI_RESTORE_FAILURES_FILE);
}

static void cli_restore_clear_failures(void)
{
    char path[700];
    cli_restore_failures_path(path, sizeof(path));
    unlink(path);
}

static void cli_restore_write_failures(const char *name, const char *cfg_path, const char *details)
{
    char path[700];
    cli_restore_failures_path(path, sizeof(path));

    GString *out = g_string_new("");
    g_string_append_printf(out, "Startup cfg replay failures for '%s'\n", name ? name : "");
    if (cfg_path && cfg_path[0] != '\0')
    {
        g_string_append_printf(out, "Source: %s\n", cfg_path);
    }
    if (details && details[0] != '\0')
    {
        g_string_append(out, details);
        if (details[strlen(details) - 1] != '\n')
        {
            g_string_append_c(out, '\n');
        }
    }
    else
    {
        g_string_append(out, "No failed command details recorded.\n");
    }

    if (!g_file_set_contents(path, out->str, -1, NULL))
    {
        LOG_WARN("CLI restore: failed to write replay failure report %s", path);
    }
    g_string_free(out, TRUE);
}

static gboolean cli_restore_name_valid(const char *name)
{
    if (!name || name[0] == '\0' || strlen(name) > 63)
    {
        return FALSE;
    }

    for (const char *p = name; *p; p++)
    {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean cli_restore_parse_startup_mode(const char *text, cli_restore_startup_mode_t *mode)
{
    if (!text || !mode)
    {
        return FALSE;
    }
    if (strcmp(text, "db") == 0)
    {
        *mode = CLI_RESTORE_STARTUP_MODE_DB;
        return TRUE;
    }
    if (strcmp(text, "cfg") == 0)
    {
        *mode = CLI_RESTORE_STARTUP_MODE_CFG;
        return TRUE;
    }
    return FALSE;
}

static gboolean cli_restore_read_startup(char *name, size_t name_size, cli_restore_startup_mode_t *mode)
{
    name[0] = '\0';
    if (mode)
    {
        *mode = CLI_RESTORE_STARTUP_MODE_NONE;
    }

    char data_dir[512];
    char ptr_path[600];
    cli_restore_data_dir(data_dir, sizeof(data_dir));
    snprintf(ptr_path, sizeof(ptr_path), "%s/startup.cfg", data_dir);

    gchar *content = NULL;
    if (!g_file_get_contents(ptr_path, &content, NULL, NULL))
    {
        return FALSE;
    }

    char mode_text[16] = "";
    char parsed_name[64] = "";
    char extra[2] = "";
    char *trimmed = g_strstrip(content);
    int fields = sscanf(trimmed, "%15s %63s %1s", mode_text, parsed_name, extra);

    cli_restore_startup_mode_t parsed_mode = CLI_RESTORE_STARTUP_MODE_NONE;
    if (fields == 2 && cli_restore_parse_startup_mode(mode_text, &parsed_mode) && cli_restore_name_valid(parsed_name))
    {
        g_strlcpy(name, parsed_name, name_size);
        if (mode)
        {
            *mode = parsed_mode;
        }
    }
    else if (trimmed[0] != '\0')
    {
        LOG_WARN("CLI restore: invalid startup pointer content, skip startup replay");
    }

    g_free(content);
    return name[0] != '\0';
}

static void cli_restore_cfg_path(const char *name, char *path, size_t path_size)
{
    char data_dir[512];
    cli_restore_data_dir(data_dir, sizeof(data_dir));
    snprintf(path, path_size, "%s/configs/%s.cfg", data_dir, name);
}

static void cli_restore_meta_path(const char *name, char *path, size_t path_size)
{
    char data_dir[512];
    cli_restore_data_dir(data_dir, sizeof(data_dir));
    snprintf(path, path_size, "%s/configs/%s.meta", data_dir, name);
}

static gboolean cli_restore_parse_major_version(const char *version, int *major)
{
    if (!version || !major)
    {
        return FALSE;
    }

    while (*version == ' ' || *version == '\t')
    {
        version++;
    }
    if (*version < '0' || *version > '9')
    {
        return FALSE;
    }

    char *end = NULL;
    long parsed = strtol(version, &end, 10);
    if (end == version || parsed < 0 || parsed > 255)
    {
        return FALSE;
    }

    *major = (int)parsed;
    return TRUE;
}

static gboolean cli_restore_read_saved_version(const char *name, char *version, size_t version_size)
{
    if (version && version_size > 0)
    {
        version[0] = '\0';
    }

    char meta_path[700];
    cli_restore_meta_path(name, meta_path, sizeof(meta_path));

    gchar *content = NULL;
    if (!g_file_get_contents(meta_path, &content, NULL, NULL))
    {
        return FALSE;
    }

    gboolean found = FALSE;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++)
    {
        char *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "version="))
        {
            char *value = g_strstrip(line + strlen("version="));
            if (value[0] != '\0')
            {
                g_strlcpy(version, value, version_size);
                found = TRUE;
                break;
            }
        }
    }

    g_strfreev(lines);
    g_free(content);
    return found;
}

static gboolean cli_restore_startup_major_mismatch(const char *name, char *saved_version, size_t saved_size,
                                                   char *current_version, size_t current_size)
{
    if (saved_version && saved_size > 0)
    {
        saved_version[0] = '\0';
    }
    if (current_version && current_size > 0)
    {
        current_version[0] = '\0';
    }

    char saved[64] = "";
    char current[64] = "";
    if (!cli_restore_read_saved_version(name, saved, sizeof(saved)))
    {
        LOG_WARN("CLI restore: startup config '%s' has no version metadata, keep requested startup mode", name);
        return FALSE;
    }
    if (read_current_version(current, sizeof(current)) != 0)
    {
        LOG_WARN("CLI restore: current version unavailable, keep requested startup mode for '%s'", name);
        return FALSE;
    }

    if (saved_version && saved_size > 0)
    {
        g_strlcpy(saved_version, saved, saved_size);
    }
    if (current_version && current_size > 0)
    {
        g_strlcpy(current_version, current, current_size);
    }

    int saved_major = 0;
    int current_major = 0;
    if (!cli_restore_parse_major_version(saved, &saved_major) ||
        !cli_restore_parse_major_version(current, &current_major))
    {
        LOG_WARN("CLI restore: version metadata not parseable (saved='%s', current='%s'), keep requested startup mode",
                 saved, current);
        return FALSE;
    }

    return saved_major != current_major;
}

static uint32_t cli_restore_indent_depth(const char *line)
{
    uint32_t depth = 0;
    for (const char *p = line; *p; p++)
    {
        if (*p == ' ' || *p == '\t')
        {
            depth++;
            continue;
        }
        break;
    }
    return depth;
}

static void cli_restore_pop_to_depth(cli_session_t *session, uint32_t bdr_depth)
{
    if (!session)
    {
        return;
    }

    uint32_t target_stack_depth = bdr_depth + 1; /* stack depth 1 == config view */
    while (session->prompt_stack_depth > target_stack_depth && session->current_view && session->current_view->parent)
    {
        session->current_view = session->current_view->parent;
        cli_prompt_pop(session);
    }
}

static gboolean cli_restore_output_has_error(const char *text)
{
    return text && (strstr(text, "Error:") || strstr(text, "DB Error:") || strstr(text, "Dev Error:"));
}

static int cli_restore_replay_text(const char *cfg_path, const char *text, char **err)
{
    if (err)
    {
        *err = NULL;
    }
    if (!text)
    {
        if (err)
        {
            *err = g_strdup("empty cfg content");
        }
        return ERRCODE_FAIL;
    }

    cli_session_t *session = g_malloc0(sizeof(*session));
    session->current_view = g_cli_local->view_tree.root;
    session->out = g_string_new("");
    update_prompt_from_template(session, session->current_view->prompt_template);

    if (process_command("config", session) == 0)
    {
        if (err)
        {
            *err = g_strdup("failed to enter config view");
        }
        cli_session_destroy(session);
        return ERRCODE_FAIL;
    }
    g_string_truncate(session->out, 0);

    gchar **lines = g_strsplit(text, "\n", -1);
    int ret = ERRCODE_SUCCESS;

    for (guint i = 0; lines && lines[i]; i++)
    {
        const char *raw = lines[i];
        uint32_t depth = cli_restore_indent_depth(raw);
        char *line = g_strdup(raw);
        char *trimmed = g_strstrip(line);

        if (trimmed[0] == '\0')
        {
            g_free(line);
            continue;
        }

        if (trimmed[0] == '!')
        {
            cli_restore_pop_to_depth(session, depth);
            g_free(line);
            continue;
        }

        cli_restore_pop_to_depth(session, depth);
        g_string_truncate(session->out, 0);

        int ok = process_command(trimmed, session);
        if (!ok || cli_restore_output_has_error(session->out->str))
        {
            if (err)
            {
                const char *out = session->out->str ? session->out->str : "";
                *err = g_strdup_printf("%s:%u: command '%s' failed%s%s", cfg_path, i + 1, trimmed, out[0] ? ": " : "",
                                       out);
            }
            ret = ERRCODE_FAIL;
            g_free(line);
            break;
        }

        g_free(line);
    }

    g_strfreev(lines);
    cli_session_destroy(session);
    return ret;
}

void cli_restore_startup_if_needed(void)
{
    if (!g_cli_local || !g_cli_local->dev_ipc_ctx)
    {
        return;
    }

    if (getenv("NN_WARM_RESTART") != NULL)
    {
        LOG_INFO("CLI restore: warm restart, skip startup cfg replay");
        return;
    }

    if (dev_ipc_wait_module_ready(g_cli_local->dev_ipc_ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_READY_MS) !=
        ERRCODE_SUCCESS)
    {
        LOG_WARN("CLI restore: DEV did not reach READY, skip startup cfg replay");
        return;
    }

    char name[64];
    cli_restore_startup_mode_t mode = CLI_RESTORE_STARTUP_MODE_NONE;
    if (!cli_restore_read_startup(name, sizeof(name), &mode))
    {
        LOG_INFO("CLI restore: no startup pointer, nothing to replay");
        cli_restore_clear_failures();
        return;
    }

    gboolean should_replay_cfg = (mode == CLI_RESTORE_STARTUP_MODE_CFG);
    if (!should_replay_cfg && mode == CLI_RESTORE_STARTUP_MODE_DB)
    {
        char saved_version[64] = "";
        char current_version[64] = "";
        if (cli_restore_startup_major_mismatch(name, saved_version, sizeof(saved_version), current_version,
                                               sizeof(current_version)))
        {
            LOG_WARN("CLI restore: startup db '%s' saved by version '%s' requires cfg replay on current version '%s' "
                     "(major mismatch)",
                     name, saved_version, current_version);
            should_replay_cfg = TRUE;
        }
    }

    if (!should_replay_cfg)
    {
        LOG_INFO("CLI restore: startup '%s' uses db mode, skip cfg replay", name);
        cli_restore_clear_failures();
        return;
    }

    char cfg_path[700];
    cli_restore_cfg_path(name, cfg_path, sizeof(cfg_path));
    if (access(cfg_path, R_OK) != 0)
    {
        LOG_WARN("CLI restore: startup cfg text not found (%s), skip cfg replay", cfg_path);
        cli_restore_write_failures(name, cfg_path, "Startup cfg text not found.\n");
        return;
    }

    gchar *content = NULL;
    GError *gerr = NULL;
    if (!g_file_get_contents(cfg_path, &content, NULL, &gerr))
    {
        LOG_WARN("CLI restore: failed to read %s: %s", cfg_path, gerr ? gerr->message : strerror(errno));
        if (gerr)
        {
            g_error_free(gerr);
        }
        cli_restore_write_failures(name, cfg_path, "Startup cfg text read failed.\n");
        return;
    }

    GString *failures = g_string_new("");
    for (int attempt = 1; attempt <= CLI_RESTORE_MAX_ATTEMPTS; attempt++)
    {
        char *err = NULL;
        if (cli_restore_replay_text(cfg_path, content, &err) == ERRCODE_SUCCESS)
        {
            LOG_INFO("CLI restore: replayed startup cfg '%s' from %s", name, cfg_path);
            cli_restore_clear_failures();
            g_string_free(failures, TRUE);
            g_free(err);
            g_free(content);
            return;
        }

        LOG_WARN("CLI restore: replay attempt %d/%d failed: %s", attempt, CLI_RESTORE_MAX_ATTEMPTS,
                 err ? err : "unknown error");
        g_string_append_printf(failures, "Attempt %d/%d: %s\n", attempt, CLI_RESTORE_MAX_ATTEMPTS,
                               err ? err : "unknown error");
        g_free(err);
        if (attempt < CLI_RESTORE_MAX_ATTEMPTS)
        {
            g_usleep((gulong)attempt * G_USEC_PER_SEC);
        }
    }

    LOG_ERROR("CLI restore: startup cfg '%s' replay failed after %d attempts", name, CLI_RESTORE_MAX_ATTEMPTS);
    cli_restore_write_failures(name, cfg_path, failures->str);
    g_string_free(failures, TRUE);
    g_free(content);
}
