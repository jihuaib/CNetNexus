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
#include "cli_config_model.h"
#include "cli_config_plan.h"
#include "cli_handler.h"
#include "cli_main.h"
#include "cli_tree.h"
#include "errcode.h"
#include "log.h"
#include "path_utils.h"

#define CLI_RESTORE_MAX_ATTEMPTS 1
#define CLI_RESTORE_FAILURES_FILE "startup-replay-failures.log"
#define CLI_RESTORE_CFG_MAX_BYTES (16U * 1024U * 1024U)

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

static gboolean cli_restore_validate_cfg_integrity(const char *name, const char *content, gsize content_len, char **err)
{
    if (err)
    {
        *err = NULL;
    }
    if (!content || content_len == 0)
    {
        if (err)
        {
            *err = g_strdup("startup cfg is empty");
        }
        return FALSE;
    }
    if (content_len > CLI_RESTORE_CFG_MAX_BYTES)
    {
        if (err)
        {
            *err = g_strdup("startup cfg exceeds 16 MiB limit");
        }
        return FALSE;
    }

    char meta_path[700];
    cli_restore_meta_path(name, meta_path, sizeof(meta_path));
    gchar *meta = NULL;
    if (!g_file_get_contents(meta_path, &meta, NULL, NULL))
    {
        /* 兼容旧版本没有完整性元数据的快照。 */
        return TRUE;
    }

    char expected_sha[65] = "";
    char format[64] = "";
    gboolean capture_declared = FALSE;
    gboolean capture_complete = FALSE;
    gchar **lines = g_strsplit(meta, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++)
    {
        char *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "cfg_sha256="))
        {
            g_strlcpy(expected_sha, g_strstrip(line + strlen("cfg_sha256=")), sizeof(expected_sha));
        }
        else if (g_str_has_prefix(line, "capture_complete="))
        {
            capture_declared = TRUE;
            capture_complete = strcmp(g_strstrip(line + strlen("capture_complete=")), "1") == 0;
        }
        else if (g_str_has_prefix(line, "format="))
        {
            g_strlcpy(format, g_strstrip(line + strlen("format=")), sizeof(format));
        }
    }
    g_strfreev(lines);
    g_free(meta);

    if (capture_declared && !capture_complete)
    {
        if (err)
        {
            *err = g_strdup("startup cfg capture is marked incomplete");
        }
        return FALSE;
    }
    if (format[0] != '\0' && strcmp(format, "bdr-indent-v1") != 0)
    {
        if (err)
        {
            *err = g_strdup("unsupported startup cfg format");
        }
        return FALSE;
    }

    if (expected_sha[0] != '\0')
    {
        gchar *actual_sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)content, content_len);
        gboolean matches =
            actual_sha && strlen(expected_sha) == 64 && g_ascii_strcasecmp(actual_sha, expected_sha) == 0;
        g_free(actual_sha);
        if (!matches)
        {
            if (err)
            {
                *err = g_strdup("startup cfg checksum mismatch");
            }
            return FALSE;
        }
    }

    return TRUE;
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

/**
 * @brief 在下发任何命令前模拟 cfg 的视图栈变化
 *
 * 层级模型会忽略 exit/end/config 控制行，因此还需要单独验证原始回放流。
 * 否则诸如“先修改配置，再在根层级 exit”的文件会通过命令树预检，却在已经
 * 修改 running 后才发现后续命令无法进入预期视图。
 */
static gboolean cli_restore_preflight_view_flow(const char *cfg_path, const char *text, char **err)
{
    cli_view_node_t *config_view = cli_view_find_by_name(g_cli_local->view_tree.root, CLI_VIEW_CONFIG);
    if (!config_view)
    {
        if (err)
        {
            *err = g_strdup("config view unavailable during cfg flow preflight");
        }
        return FALSE;
    }

    cli_view_node_t *view_stack[CLI_PROMPT_STACK_DEPTH] = {NULL};
    guint stack_depth = 1;
    view_stack[0] = config_view;

    gchar **lines = g_strsplit(text, "\n", -1);
    gboolean valid = TRUE;
    for (guint i = 0; lines && lines[i]; i++)
    {
        const char *raw = lines[i];
        uint32_t bdr_depth = cli_restore_indent_depth(raw);
        char *line = g_strdup(raw);
        char *trimmed = g_strstrip(line);

        if (trimmed[0] == '\0')
        {
            g_free(line);
            continue;
        }
        if (bdr_depth >= CLI_PROMPT_STACK_DEPTH)
        {
            if (err)
            {
                *err = g_strdup_printf("%s:%u: cfg indentation exceeds maximum view depth", cfg_path, i + 1);
            }
            valid = FALSE;
            g_free(line);
            break;
        }

        guint expected_depth = bdr_depth + 1; /* config view == 1 */
        if (stack_depth < expected_depth)
        {
            if (err)
            {
                *err = g_strdup_printf("%s:%u: indentation requires view depth %u, but previous command left depth %u",
                                       cfg_path, i + 1, expected_depth, stack_depth);
            }
            valid = FALSE;
            g_free(line);
            break;
        }
        while (stack_depth > expected_depth)
        {
            view_stack[--stack_depth] = NULL;
        }

        if (trimmed[0] == '!')
        {
            g_free(line);
            continue;
        }
        if (strcmp(trimmed, "exit") == 0)
        {
            if (stack_depth <= 1)
            {
                if (err)
                {
                    *err = g_strdup_printf("%s:%u: explicit exit would leave the config view", cfg_path, i + 1);
                }
                valid = FALSE;
                g_free(line);
                break;
            }
            view_stack[--stack_depth] = NULL;
            g_free(line);
            continue;
        }
        if (strcmp(trimmed, "config") == 0 || strcmp(trimmed, "end") == 0)
        {
            if (err)
            {
                *err = g_strdup_printf("%s:%u: control command '%s' is not allowed inside a startup cfg", cfg_path,
                                       i + 1, trimmed);
            }
            valid = FALSE;
            g_free(line);
            break;
        }

        cli_view_node_t *current_view = view_stack[stack_depth - 1];
        /* Startup cfg 只能包含当前配置视图自身的命令。全局命令树还包含
         * reboot/show/terminal 等运维命令，回放它们既不属于配置恢复，甚至可能
         * 造成永久重启循环。 */
        cli_match_result_t *match = cli_tree_match_command_full(current_view->cmd_tree, trimmed);
        if (!match || !match->final_node || !match->final_node->is_end_node)
        {
            if (err)
            {
                *err = g_strdup_printf("%s:%u: command '%s' is invalid in view '%s'", cfg_path, i + 1, trimmed,
                                       current_view->view_name);
            }
            if (match)
            {
                cli_match_result_free(match);
            }
            valid = FALSE;
            g_free(line);
            break;
        }

        const char *target_view_name = match->final_node->target_view_name;
        if (target_view_name)
        {
            cli_view_node_t *target_view = cli_view_find_by_name(g_cli_local->view_tree.root, target_view_name);
            if (!target_view || stack_depth >= CLI_PROMPT_STACK_DEPTH)
            {
                if (err)
                {
                    *err = g_strdup_printf("%s:%u: command '%s' enters an unavailable or over-depth view '%s'",
                                           cfg_path, i + 1, trimmed, target_view_name);
                }
                cli_match_result_free(match);
                valid = FALSE;
                g_free(line);
                break;
            }
            view_stack[stack_depth++] = target_view;
        }

        cli_match_result_free(match);
        g_free(line);
    }

    g_strfreev(lines);
    return valid;
}

static gboolean cli_restore_pop_to_depth(cli_session_t *session, uint32_t bdr_depth)
{
    if (!session)
    {
        return FALSE;
    }

    uint32_t target_stack_depth = bdr_depth + 1; /* stack depth 1 == config view */
    while (session->prompt_stack_depth > target_stack_depth && session->current_view && session->current_view->parent)
    {
        uint32_t before = session->prompt_stack_depth;
        g_string_truncate(session->out, 0);
        if (!process_command("exit", session) || session->prompt_stack_depth >= before)
        {
            return FALSE;
        }
    }
    return session->prompt_stack_depth == target_stack_depth;
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

    if (!cli_restore_preflight_view_flow(cfg_path, text, err))
    {
        return ERRCODE_FAIL;
    }

    /*
     * 在任何模块配置发生变化前，先完成层级解析和整棵命令树预检。这里从空配置
     * 生成 ADD 计划只用于验证，不执行计划；实际回放仍保留源文件行号用于诊断。
     */
    cli_config_model_t *model = NULL;
    GError *model_error = NULL;
    if (!cli_config_model_parse(text, &model, &model_error))
    {
        if (err)
        {
            *err = g_strdup_printf("%s: invalid hierarchical cfg: %s", cfg_path,
                                   model_error ? model_error->message : "parse failed");
        }
        g_clear_error(&model_error);
        return ERRCODE_FAIL;
    }

    cli_view_node_t *config_view = cli_view_find_by_name(g_cli_local->view_tree.root, CLI_VIEW_CONFIG);
    cli_config_plan_t *validation_plan = NULL;
    GError *plan_error = NULL;
    if (!config_view || !cli_config_plan_build(NULL, model, config_view, g_cli_local->view_tree.root,
                                               g_cli_local->view_tree.global_cmd_tree, &validation_plan, &plan_error))
    {
        if (err)
        {
            *err = g_strdup_printf("%s: cfg preflight failed: %s", cfg_path,
                                   plan_error ? plan_error->message : "config view unavailable");
        }
        g_clear_error(&plan_error);
        cli_config_plan_free(validation_plan);
        cli_config_model_free(model);
        return ERRCODE_FAIL;
    }
    cli_config_plan_free(validation_plan);
    cli_config_model_free(model);

    cli_session_t *session = g_malloc0(sizeof(*session));
    session->internal_session = 1;
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
            if (!cli_restore_pop_to_depth(session, depth))
            {
                if (err)
                {
                    *err = g_strdup_printf("%s:%u: failed to leave configuration view", cfg_path, i + 1);
                }
                ret = ERRCODE_FAIL;
                g_free(line);
                break;
            }
            g_free(line);
            continue;
        }

        if (!cli_restore_pop_to_depth(session, depth))
        {
            if (err)
            {
                *err = g_strdup_printf("%s:%u: invalid view depth before command '%s'", cfg_path, i + 1, trimmed);
            }
            ret = ERRCODE_FAIL;
            g_free(line);
            break;
        }
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

    if (ret == ERRCODE_SUCCESS && !cli_restore_pop_to_depth(session, 0))
    {
        if (err)
        {
            *err = g_strdup_printf("%s: failed to leave final configuration view", cfg_path);
        }
        ret = ERRCODE_FAIL;
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
    gsize content_len = 0;
    GError *gerr = NULL;
    if (!g_file_get_contents(cfg_path, &content, &content_len, &gerr))
    {
        LOG_WARN("CLI restore: failed to read %s: %s", cfg_path, gerr ? gerr->message : strerror(errno));
        if (gerr)
        {
            g_error_free(gerr);
        }
        cli_restore_write_failures(name, cfg_path, "Startup cfg text read failed.\n");
        return;
    }

    char *integrity_err = NULL;
    if (!cli_restore_validate_cfg_integrity(name, content, content_len, &integrity_err))
    {
        LOG_ERROR("CLI restore: refusing startup cfg '%s': %s", name,
                  integrity_err ? integrity_err : "integrity validation failed");
        cli_restore_write_failures(name, cfg_path,
                                   integrity_err ? integrity_err : "Startup cfg integrity validation failed.\n");
        g_free(integrity_err);
        g_free(content);
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
