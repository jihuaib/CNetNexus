/**
 * @file   dev_fs_cli.c
 * @brief  DEV filesystem CLI commands (ls/cd/more)
 * @author jhb
 * @date   2026/06/06
 */

#include "dev_fs_cli.h"

#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev_main.h"
#include "errcode.h"

#define DEV_FS_DEFAULT_LINE_ID UINT32_MAX
#define DEV_FS_MORE_MAX_BYTES (1024u * 1024u)

typedef struct dev_fs_session
{
    char cwd[PATH_MAX];
} dev_fs_session_t;

static GHashTable *g_dev_fs_sessions = NULL;
static GMutex g_dev_fs_mutex;

static void dev_fs_sessions_ensure_locked(void)
{
    if (!g_dev_fs_sessions)
    {
        g_dev_fs_sessions = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }
}

static void dev_fs_send_cli_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, const char *text)
{
    const char *safe = text ? text : "";
    char *resp_data = g_strdup(safe);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_DEV, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(resp_data);
    }
}

static int dev_fs_resolve_root(char *root, size_t root_size)
{
    if (!root || root_size == 0)
    {
        return ERRCODE_FAIL;
    }

    char base[PATH_MAX] = {0};
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        strlcpy(base, work_dir, sizeof(base));
    }
    else if (!getcwd(base, sizeof(base)))
    {
        return ERRCODE_FAIL;
    }

    char real[PATH_MAX] = {0};
    if (!realpath(base, real))
    {
        return ERRCODE_FAIL;
    }

    strlcpy(root, real, root_size);
    return ERRCODE_SUCCESS;
}

static gboolean dev_fs_is_within_root(const char *root, const char *path)
{
    if (!root || !path)
    {
        return FALSE;
    }
    if (strcmp(root, "/") == 0)
    {
        return path[0] == '/';
    }

    size_t root_len = strlen(root);
    return strcmp(root, path) == 0 || (strncmp(root, path, root_len) == 0 && path[root_len] == '/');
}

static void dev_fs_virtual_path(const char *root, const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0)
    {
        return;
    }
    if (!root || !path || strcmp(root, path) == 0)
    {
        strlcpy(out, "/", out_size);
        return;
    }

    size_t root_len = strlen(root);
    if (strncmp(root, path, root_len) == 0 && path[root_len] == '/')
    {
        snprintf(out, out_size, "/%s", path + root_len + 1);
        return;
    }
    strlcpy(out, path, out_size);
}

static int dev_fs_parse_path_and_line(cli_tlv_parser_t *parser, char *path, size_t path_size, uint32_t *line_id)
{
    if (path && path_size > 0)
    {
        path[0] = '\0';
    }
    if (line_id)
    {
        *line_id = DEV_FS_DEFAULT_LINE_ID;
    }

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (line_id && entry.cfg_id == CLI_CTX_ID_ACCESS_LINE)
            {
                *line_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (path && path_size > 0 && entry.cfg_id == 1)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(path, text, path_size);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    return ERRCODE_SUCCESS;
}

static void dev_fs_get_session_cwd(uint32_t line_id, const char *root, char *cwd, size_t cwd_size)
{
    g_mutex_lock(&g_dev_fs_mutex);
    dev_fs_sessions_ensure_locked();

    dev_fs_session_t *session = g_hash_table_lookup(g_dev_fs_sessions, GUINT_TO_POINTER(line_id));
    if (!session)
    {
        session = g_new0(dev_fs_session_t, 1);
        strlcpy(session->cwd, root, sizeof(session->cwd));
        g_hash_table_insert(g_dev_fs_sessions, GUINT_TO_POINTER(line_id), session);
    }

    if (!dev_fs_is_within_root(root, session->cwd) || access(session->cwd, R_OK | X_OK) != 0)
    {
        strlcpy(session->cwd, root, sizeof(session->cwd));
    }

    strlcpy(cwd, session->cwd, cwd_size);
    g_mutex_unlock(&g_dev_fs_mutex);
}

static void dev_fs_set_session_cwd(uint32_t line_id, const char *cwd)
{
    g_mutex_lock(&g_dev_fs_mutex);
    dev_fs_sessions_ensure_locked();

    dev_fs_session_t *session = g_hash_table_lookup(g_dev_fs_sessions, GUINT_TO_POINTER(line_id));
    if (!session)
    {
        session = g_new0(dev_fs_session_t, 1);
        g_hash_table_insert(g_dev_fs_sessions, GUINT_TO_POINTER(line_id), session);
    }
    strlcpy(session->cwd, cwd, sizeof(session->cwd));
    g_mutex_unlock(&g_dev_fs_mutex);
}

void dev_fs_cli_cleanup_line(uint32_t line_id)
{
    g_mutex_lock(&g_dev_fs_mutex);
    if (g_dev_fs_sessions)
    {
        g_hash_table_remove(g_dev_fs_sessions, GUINT_TO_POINTER(line_id));
    }
    g_mutex_unlock(&g_dev_fs_mutex);
}

void dev_fs_cli_cleanup_all(void)
{
    g_mutex_lock(&g_dev_fs_mutex);
    if (g_dev_fs_sessions)
    {
        g_hash_table_destroy(g_dev_fs_sessions);
        g_dev_fs_sessions = NULL;
    }
    g_mutex_unlock(&g_dev_fs_mutex);
}

static int dev_fs_resolve_target(const char *root, const char *cwd, const char *input, char *resolved,
                                 size_t resolved_size, char *err, size_t err_size)
{
    char *candidate = NULL;
    if (!input || input[0] == '\0')
    {
        candidate = g_strdup(cwd);
    }
    else if (input[0] == '/')
    {
        candidate = (input[1] == '\0') ? g_strdup(root) : g_build_filename(root, input + 1, NULL);
    }
    else
    {
        candidate = g_build_filename(cwd, input, NULL);
    }

    if (!candidate)
    {
        snprintf(err, err_size, "Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    char real[PATH_MAX] = {0};
    if (!realpath(candidate, real))
    {
        snprintf(err, err_size, "Error: unable to resolve path '%s': %s.\r\n", input ? input : "", strerror(errno));
        g_free(candidate);
        return ERRCODE_FAIL;
    }
    g_free(candidate);

    if (!dev_fs_is_within_root(root, real))
    {
        snprintf(err, err_size, "Error: path escapes work directory.\r\n");
        return ERRCODE_FAIL;
    }

    strlcpy(resolved, real, resolved_size);
    return ERRCODE_SUCCESS;
}

static gint dev_fs_name_compare(gconstpointer a, gconstpointer b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return g_strcmp0(sa, sb);
}

int dev_fs_cli_handle_ls(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t line_id = DEV_FS_DEFAULT_LINE_ID;
    (void)dev_fs_parse_path_and_line(parser, NULL, 0, &line_id);

    char root[PATH_MAX] = {0};
    if (dev_fs_resolve_root(root, sizeof(root)) != ERRCODE_SUCCESS)
    {
        dev_fs_send_cli_response(ctx, msg, "Error: unable to resolve work directory.\r\n");
        return ERRCODE_FAIL;
    }

    char cwd[PATH_MAX] = {0};
    dev_fs_get_session_cwd(line_id, root, cwd, sizeof(cwd));

    DIR *dir = opendir(cwd);
    if (!dir)
    {
        char out[256];
        snprintf(out, sizeof(out), "Error: unable to open directory: %s.\r\n", strerror(errno));
        dev_fs_send_cli_response(ctx, msg, out);
        return ERRCODE_FAIL;
    }

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    struct dirent *de;
    while ((de = readdir(dir)) != NULL)
    {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
        {
            g_ptr_array_add(names, g_strdup(de->d_name));
        }
    }
    closedir(dir);
    g_ptr_array_sort(names, dev_fs_name_compare);

    char virt[PATH_MAX] = {0};
    dev_fs_virtual_path(root, cwd, virt, sizeof(virt));
    GString *buf = g_string_new("");
    if (!buf)
    {
        g_ptr_array_free(names, TRUE);
        dev_fs_send_cli_response(ctx, msg, "Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    g_string_append_printf(buf, "\r\nDirectory: %s\r\n", virt);
    if (names->len == 0)
    {
        g_string_append(buf, "  (empty)\r\n\r\n");
        g_ptr_array_free(names, TRUE);
        return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, buf);
    }

    g_string_append_printf(buf, "  %-4s %10s  %s\r\n", "Type", "Size", "Name");
    g_string_append(buf, "  -----------------------------------------------\r\n");
    for (guint i = 0; i < names->len; i++)
    {
        const char *name = g_ptr_array_index(names, i);
        char *path = g_build_filename(cwd, name, NULL);
        struct stat st;
        char type = '?';
        long long size = 0;
        const char *suffix = "";
        if (path && lstat(path, &st) == 0)
        {
            size = (long long)st.st_size;
            if (S_ISDIR(st.st_mode))
            {
                type = 'd';
                suffix = "/";
            }
            else if (S_ISLNK(st.st_mode))
            {
                type = 'l';
            }
            else if (S_ISREG(st.st_mode))
            {
                type = '-';
            }
            else
            {
                type = 'o';
            }
        }
        g_string_append_printf(buf, "  %-4c %10lld  %s%s\r\n", type, size, name, suffix);
        g_free(path);
    }
    g_string_append(buf, "\r\n");
    g_ptr_array_free(names, TRUE);
    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, buf);
}

int dev_fs_cli_handle_pwd(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t line_id = DEV_FS_DEFAULT_LINE_ID;
    (void)dev_fs_parse_path_and_line(parser, NULL, 0, &line_id);

    char root[PATH_MAX] = {0};
    if (dev_fs_resolve_root(root, sizeof(root)) != ERRCODE_SUCCESS)
    {
        dev_fs_send_cli_response(ctx, msg, "Error: unable to resolve work directory.\r\n");
        return ERRCODE_FAIL;
    }

    char cwd[PATH_MAX] = {0};
    dev_fs_get_session_cwd(line_id, root, cwd, sizeof(cwd));

    char virt[PATH_MAX] = {0};
    dev_fs_virtual_path(root, cwd, virt, sizeof(virt));

    char out[PATH_MAX + 8];
    snprintf(out, sizeof(out), "%s\r\n", virt);
    dev_fs_send_cli_response(ctx, msg, out);
    return ERRCODE_SUCCESS;
}

int dev_fs_cli_handle_cd(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char input[256] = {0};
    uint32_t line_id = DEV_FS_DEFAULT_LINE_ID;
    (void)dev_fs_parse_path_and_line(parser, input, sizeof(input), &line_id);

    char root[PATH_MAX] = {0};
    if (dev_fs_resolve_root(root, sizeof(root)) != ERRCODE_SUCCESS)
    {
        dev_fs_send_cli_response(ctx, msg, "Error: unable to resolve work directory.\r\n");
        return ERRCODE_FAIL;
    }

    char cwd[PATH_MAX] = {0};
    dev_fs_get_session_cwd(line_id, root, cwd, sizeof(cwd));

    char err[320] = {0};
    char target[PATH_MAX] = {0};
    const char *target_input = (input[0] == '\0') ? "/" : input;
    if (dev_fs_resolve_target(root, cwd, target_input, target, sizeof(target), err, sizeof(err)) != ERRCODE_SUCCESS)
    {
        dev_fs_send_cli_response(ctx, msg, err[0] ? err : "Error: invalid path.\r\n");
        return ERRCODE_FAIL;
    }

    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        dev_fs_send_cli_response(ctx, msg, "Error: target is not a directory.\r\n");
        return ERRCODE_FAIL;
    }

    dev_fs_set_session_cwd(line_id, target);

    char virt[PATH_MAX] = {0};
    dev_fs_virtual_path(root, target, virt, sizeof(virt));
    char out[PATH_MAX + 64];
    snprintf(out, sizeof(out), "Current directory: %s\r\n", virt);
    dev_fs_send_cli_response(ctx, msg, out);
    return ERRCODE_SUCCESS;
}

static void dev_fs_append_more_bytes(GString *buf, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\0')
        {
            g_string_append_c(buf, ' ');
        }
        else if (ch == '\n')
        {
            g_string_append(buf, "\r\n");
        }
        else if (ch != '\r')
        {
            g_string_append_c(buf, (char)ch);
        }
    }
}

int dev_fs_cli_handle_more(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char input[256] = {0};
    uint32_t line_id = DEV_FS_DEFAULT_LINE_ID;
    (void)dev_fs_parse_path_and_line(parser, input, sizeof(input), &line_id);

    if (input[0] == '\0')
    {
        dev_fs_send_cli_response(ctx, msg, "Error: missing file path.\r\n");
        return ERRCODE_FAIL;
    }

    char root[PATH_MAX] = {0};
    if (dev_fs_resolve_root(root, sizeof(root)) != ERRCODE_SUCCESS)
    {
        dev_fs_send_cli_response(ctx, msg, "Error: unable to resolve work directory.\r\n");
        return ERRCODE_FAIL;
    }

    char cwd[PATH_MAX] = {0};
    dev_fs_get_session_cwd(line_id, root, cwd, sizeof(cwd));

    char err[320] = {0};
    char target[PATH_MAX] = {0};
    if (dev_fs_resolve_target(root, cwd, input, target, sizeof(target), err, sizeof(err)) != ERRCODE_SUCCESS)
    {
        dev_fs_send_cli_response(ctx, msg, err[0] ? err : "Error: invalid path.\r\n");
        return ERRCODE_FAIL;
    }

    struct stat st;
    if (stat(target, &st) != 0)
    {
        char out[256];
        snprintf(out, sizeof(out), "Error: unable to stat file: %s.\r\n", strerror(errno));
        dev_fs_send_cli_response(ctx, msg, out);
        return ERRCODE_FAIL;
    }
    if (!S_ISREG(st.st_mode))
    {
        dev_fs_send_cli_response(ctx, msg, "Error: target is not a regular file.\r\n");
        return ERRCODE_FAIL;
    }

    FILE *fp = fopen(target, "rb");
    if (!fp)
    {
        char out[256];
        snprintf(out, sizeof(out), "Error: unable to open file: %s.\r\n", strerror(errno));
        dev_fs_send_cli_response(ctx, msg, out);
        return ERRCODE_FAIL;
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        fclose(fp);
        dev_fs_send_cli_response(ctx, msg, "Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    char tmp[1024];
    size_t total = 0;
    while (!feof(fp) && total < DEV_FS_MORE_MAX_BYTES)
    {
        size_t want = sizeof(tmp);
        if (DEV_FS_MORE_MAX_BYTES - total < want)
        {
            want = DEV_FS_MORE_MAX_BYTES - total;
        }
        size_t n = fread(tmp, 1, want, fp);
        if (n > 0)
        {
            dev_fs_append_more_bytes(buf, tmp, n);
            total += n;
        }
        if (n < want)
        {
            break;
        }
    }
    if (!feof(fp))
    {
        g_string_append_printf(buf, "\r\n[truncated after %u bytes]\r\n", DEV_FS_MORE_MAX_BYTES);
    }
    if (buf->len == 0 || buf->str[buf->len - 1] != '\n')
    {
        g_string_append(buf, "\r\n");
    }
    fclose(fp);

    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, buf);
}
