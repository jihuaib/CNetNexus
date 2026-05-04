/**
 * @file   dev_cli.c
 * @brief  Dev 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */

#include "dev_cli.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli.h"
#include "dev_db.h"
#include "dev_main.h"
#include "dev_module.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "path_utils.h"

static gint g_reboot_in_progress = 0;
static FILE *g_ping_stream_fp = NULL;
static int g_ping_stream_prefixed = 0;

/* swap-image 改为后台线程模型,无需共享状态(参数全部下到 swap_async_args_t) */

// ============================================================================
// 内部辅助函数：show 命令
// ============================================================================

typedef struct show_module_ctx
{
    GString *resp;
    dev_ipc_context_t *dev_ipc_ctx;
} show_module_ctx_t;

static const char *log_level_to_string(log_level_t level)
{
    switch (level)
    {
        case LOG_LEVEL_DEBUG:
            return "debug";
        case LOG_LEVEL_INFO:
            return "info";
        case LOG_LEVEL_WARN:
            return "warn";
        case LOG_LEVEL_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static const char *build_profile_string(void)
{
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

static const char *asan_enabled_string(void)
{
#if defined(__has_feature)
#    if __has_feature(address_sanitizer)
    return "yes";
#    else
    return "no";
#    endif
#elif defined(__SANITIZE_ADDRESS__)
    return "yes";
#else
    return "no";
#endif
}

static int file_read_first_line(const char *path, char *out, size_t out_size)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        return ERRCODE_FAIL;
    }

    if (!fgets(out, (int)out_size, fp))
    {
        fclose(fp);
        return ERRCODE_FAIL;
    }
    fclose(fp);

    size_t n = strcspn(out, "\r\n");
    out[n] = '\0';
    return (out[0] != '\0') ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int resolve_version_file(char *path, size_t path_size)
{
    char *p = NULL;

    /* 生产环境 1: 环境变量 NN_WORK_DIR */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        p = g_build_filename(work_dir, "VERSION", NULL);
        if (access(p, R_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return ERRCODE_SUCCESS;
        }
        g_free(p);
    }

    /* 开发环境: 相对于可执行文件的路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        p = g_build_filename(exe_dir, "..", "..", "VERSION", NULL);
        if (access(p, R_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return ERRCODE_SUCCESS;
        }
        g_free(p);
    }

    /* 回退: 当前目录 */
    strlcpy(path, "VERSION", path_size);
    if (access(path, R_OK) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

/* 解析 image_tag 文件路径(由 swap-image.sh 写入)：
 * 1) ${NN_WORK_DIR}/.image_tag
 * 2) /opt/netnexus/.image_tag
 */
static int resolve_image_tag_file(char *path, size_t path_size)
{
    char *p = NULL;

    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        p = g_build_filename(work_dir, ".image_tag", NULL);
        if (access(p, R_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return ERRCODE_SUCCESS;
        }
        g_free(p);
    }

    if (access("/opt/netnexus/.image_tag", R_OK) == 0)
    {
        strlcpy(path, "/opt/netnexus/.image_tag", path_size);
        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

static const char *dev_phase_to_string(uint8_t phase)
{
    switch (phase)
    {
        case DEV_PHASE_REGISTERED:
            return "REGISTERED";
        case DEV_PHASE_LOADED:
            return "LOADED";
        case DEV_PHASE_IPC_READY:
            return "DEV_IPC_READY";
        case DEV_PHASE_DB_RECOVERED:
            return "DB_RECOVERED";
        case DEV_PHASE_READY:
            return "READY";
        default:
            return "UNKNOWN";
    }
}

static gboolean show_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    show_module_ctx_t *ctx = (show_module_ctx_t *)data;
    dev_module_t *module = (dev_module_t *)value;
    const char *phase = dev_phase_to_string(module->phase);
    const char *dev_ipc_state =
        (module->module_id == DEV_MODULE_ID_DEV || dev_ipc_is_connected(ctx->dev_ipc_ctx, module->module_id)) ? "up"
                                                                                                              : "down";
    g_string_append_printf(ctx->resp, "  %-10u %-14s %-12s %-6u %s\r\n", module->module_id, module->name, phase,
                           module->port, dev_ipc_state);

    return FALSE;
}

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void dev_send_cli_response_with_type(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, uint32_t msg_type,
                                            const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp_msg = dev_ipc_message_create(msg_type, DEV_MODULE_ID_DEV, msg->src_module_id,
                                                         msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        dev_ipc_send_response(ctx, resp_msg);
        dev_ipc_message_free(resp_msg);
    }
}

static void dev_send_cli_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, const char *text)
{
    dev_send_cli_response_with_type(ctx, msg, CLI_MSG_TYPE_RESP, text);
}

static int ping_stream_send_next(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!g_ping_stream_fp)
    {
        dev_send_cli_response(ctx, msg, "");
        return ERRCODE_SUCCESS;
    }

    char line[256];
    if (!fgets(line, sizeof(line), g_ping_stream_fp))
    {
        pclose(g_ping_stream_fp);
        g_ping_stream_fp = NULL;
        g_ping_stream_prefixed = 0;
        dev_send_cli_response_with_type(ctx, msg, CLI_MSG_TYPE_RESP, "");
        return ERRCODE_SUCCESS;
    }

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
    {
        line[--len] = '\0';
    }

    char out[320];
    if (!g_ping_stream_prefixed)
    {
        g_ping_stream_prefixed = 1;
        snprintf(out, sizeof(out), "\r\n%s\r\n", line);
    }
    else
    {
        snprintf(out, sizeof(out), "%s\r\n", line);
    }

    dev_send_cli_response_with_type(ctx, msg, CLI_MSG_TYPE_RESP_MORE, out);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 命令处理函数（按 group_id 分发）
// ============================================================================

static int handle_show_module(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    show_module_ctx_t show_ctx;
    memset(&show_ctx, 0, sizeof(show_ctx));

    show_ctx.resp = g_string_new("");
    if (!show_ctx.resp)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }
    show_ctx.dev_ipc_ctx = ctx;

    g_string_append_printf(show_ctx.resp,
                           "\r\nRegistered Modules:\r\n"
                           "  %-10s %-14s %-12s %-6s %s\r\n"
                           "  --------------------------------------------------------\r\n",
                           "ID", "Name", "Phase", "Port", "IPC");

    dev_module_foreach(show_module_callback, &show_ctx);
    g_string_append(show_ctx.resp, "\r\n");
    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, show_ctx.resp);
}

#ifndef NN_AUTHOR
#    define NN_AUTHOR "jihuaibin"
#endif
#ifndef NN_AUTHOR_EMAIL
#    define NN_AUTHOR_EMAIL "jihuaib@gmail.com"
#endif

static int handle_show_version(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    char version[64] = "unknown";
    char version_path[PATH_MAX];
    GString *buf = g_string_new("");
    if (!buf)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if (resolve_version_file(version_path, sizeof(version_path)) == ERRCODE_SUCCESS)
    {
        if (file_read_first_line(version_path, version, sizeof(version)) != ERRCODE_SUCCESS)
        {
            strlcpy(version, "unknown", sizeof(version));
        }
    }

    /* 镜像 tag 由 swap-image.sh 在替换镜像后写入,首次部署可能不存在 */
    char image_tag[160] = "n/a";
    char image_tag_path[PATH_MAX];
    if (resolve_image_tag_file(image_tag_path, sizeof(image_tag_path)) == ERRCODE_SUCCESS)
    {
        if (file_read_first_line(image_tag_path, image_tag, sizeof(image_tag)) != ERRCODE_SUCCESS)
        {
            strlcpy(image_tag, "n/a", sizeof(image_tag));
        }
    }

    /* 系统架构 / 内核信息（来自 uname(2)） */
    struct utsname uts;
    int has_uts = (uname(&uts) == 0) ? 1 : 0;

    g_string_append(buf, "\r\nNetNexus Version Information:\r\n");
    g_string_append_printf(buf, "  Version      : %s\r\n", version);
    g_string_append_printf(buf, "  Image        : %s\r\n", image_tag);
    g_string_append_printf(buf, "  Author       : %s\r\n", NN_AUTHOR);
    g_string_append_printf(buf, "  Email        : %s\r\n", NN_AUTHOR_EMAIL);
    g_string_append_printf(buf, "  Build Time   : %s %s\r\n", __DATE__, __TIME__);
    g_string_append_printf(buf, "  Build Profile: %s\r\n", build_profile_string());
    g_string_append_printf(buf, "  Compiler     : %s\r\n", __VERSION__);
    g_string_append_printf(buf, "  ASAN         : %s\r\n", asan_enabled_string());
    g_string_append_printf(buf, "  Log Level    : %s\r\n", log_level_to_string(log_get_level()));
    g_string_append_printf(buf, "  Architecture : %s\r\n", has_uts ? uts.machine : "unknown");
    g_string_append_printf(buf, "  OS           : %s %s\r\n", has_uts ? uts.sysname : "unknown",
                           has_uts ? uts.release : "");
    g_string_append_printf(buf, "  Hostname     : %s\r\n", has_uts ? uts.nodename : "unknown");
    g_string_append_printf(buf, "  PID          : %d\r\n\r\n", (int)getpid());

    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, buf);
}

/**
 * @brief 把 sysname 推送给 CLI 模块（CLI 收到后立即生效）
 *
 * sysname=NULL 或 "" 表示恢复默认 "NetNexus"。
 */
static void dev_push_sysname_to_cli(dev_ipc_context_t *ctx, const char *sysname)
{
    if (!ctx)
    {
        return;
    }
    const char *v = (sysname && sysname[0] != '\0') ? sysname : "";
    uint32_t payload_len = (uint32_t)(strlen(v) + 1);
    char *payload = g_strdup(v);
    if (!payload)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(CLI_MSG_TYPE_SYSNAME_UPDATE, DEV_MODULE_ID_DEV, DEV_MODULE_ID_CLI, 0,
                                                  payload, payload_len, g_free);
    if (!m)
    {
        g_free(payload);
        return;
    }
    if (dev_ipc_send(ctx, DEV_MODULE_ID_CLI, m) != ERRCODE_SUCCESS)
    {
        LOG_WARN("DEV: failed to push sysname to CLI");
    }
    dev_ipc_message_free(m);
}

static int handle_sysname(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: failed to parse sysname command\r\n");
        return ERRCODE_FAIL;
    }

    gboolean is_no = (parser.flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char hostname[CLI_SYSNAME_MAX_LEN] = {0};
    cli_tlv_entry_t entry;
    while (cli_tlv_next(&parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == 1)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(hostname, sizeof(hostname), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_cleanup(&parser);

    if (is_no)
    {
        /* no sysname：清空 DB + 通知 CLI 恢复默认 */
        if (dev_db_set_sysname("") != 0)
        {
            dev_send_cli_response(ctx, msg, "Dev Error: failed to clear sysname\r\n");
            return ERRCODE_FAIL;
        }
        dev_push_sysname_to_cli(ctx, "");
        dev_send_cli_response(ctx, msg, "");
        return ERRCODE_SUCCESS;
    }

    if (hostname[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Dev Error: hostname required\r\n");
        return ERRCODE_FAIL;
    }

    if (dev_db_set_sysname(hostname) != 0)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: failed to persist sysname\r\n");
        return ERRCODE_FAIL;
    }
    dev_push_sysname_to_cli(ctx, hostname);
    dev_send_cli_response(ctx, msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_reboot(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!g_atomic_int_compare_and_exchange(&g_reboot_in_progress, 0, 1))
    {
        dev_send_cli_response(ctx, msg, "Dev: reboot already in progress.\r\n");
        return ERRCODE_FAIL;
    }

    dev_send_cli_response(ctx, msg, "Dev: reboot accepted, reconnect later.\r\n");
    /* 短暂让出，提升 ACK 送达 CLI 客户端的概率。 */
    g_usleep(100 * 1000);

    int ret = dev_reboot_software();
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Software reboot failed");
    }

    g_atomic_int_set(&g_reboot_in_progress, 0);
    return ret;
}

/* 校验镜像名：仅允许 [A-Za-z0-9._:/-]，长度 1~128，防止命令注入 */
static int dev_validate_image_name(const char *s)
{
    if (!s || s[0] == '\0')
    {
        return ERRCODE_FAIL;
    }
    size_t n = strlen(s);
    if (n > 128)
    {
        return ERRCODE_FAIL;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == ':' || c == '/' || c == '-'))
        {
            return ERRCODE_FAIL;
        }
    }
    return ERRCODE_SUCCESS;
}

/* 解析 swap-image helper 脚本路径：
 * 1) ${NN_WORK_DIR}/scripts/swap-image.sh   （生产部署，由 package/deploy 安装）
 * 2) /opt/netnexus/scripts/swap-image.sh    （生产默认路径）
 * 3) <repo>/scripts/prod/swap-image.sh      （开发环境，可执行文件在 build/bin/）
 */
static int dev_resolve_swap_image_script(char *path, size_t path_size)
{
    char *p = NULL;

    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        p = g_build_filename(work_dir, "scripts", "swap-image.sh", NULL);
        if (access(p, X_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return ERRCODE_SUCCESS;
        }
        g_free(p);
    }

    if (access("/opt/netnexus/scripts/swap-image.sh", X_OK) == 0)
    {
        strlcpy(path, "/opt/netnexus/scripts/swap-image.sh", path_size);
        return ERRCODE_SUCCESS;
    }

    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        p = g_build_filename(exe_dir, "..", "..", "scripts", "prod", "swap-image.sh", NULL);
        if (access(p, X_OK) == 0)
        {
            strlcpy(path, p, path_size);
            g_free(p);
            return ERRCODE_SUCCESS;
        }
        g_free(p);
    }

    return ERRCODE_FAIL;
}

/* 后台 swap 工作线程(前向声明,实现见下方)。
 *
 * dev 的 ipc-wk-dev 是单线程串行,绝不能 block。CFG → DEV 的 IPC 查询有
 * 5s 超时,RESP_MORE 流式方案在 docker create/export 期间会撞超时。所以
 * 改成后台线程跑:worker 立刻发 ACK 返回,后台线程独立完成脚本和 execv。
 *
 * 后台线程的 execv 会替换整个进程映像,杀掉所有线程(包括 ipc-wk-dev),
 * 容器内 telnet 连接随之被关闭。客户端重连后看到的是新版 netnexus。
 */
static gpointer swap_async_worker(gpointer data);

/* 后台线程参数(分配在堆上,线程结束前自释放) */
typedef struct
{
    char script[PATH_MAX];
    char image[160];
    char netnexus_path[PATH_MAX];
} swap_async_args_t;

static gpointer swap_async_worker(gpointer data)
{
    swap_async_args_t *args = (swap_async_args_t *)data;

    /* 日志路径:跟其它模块对齐,统一放 ${NN_WORK_DIR}/log/swap-image-{pid}.log。
     * 回退 /opt/netnexus/log/...,最后 /tmp/...(NN_WORK_DIR 不可写时) */
    char *log_dir = NULL;
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        log_dir = g_build_filename(work_dir, "log", NULL);
    }
    else
    {
        log_dir = g_strdup("/opt/netnexus/log");
    }
    if (g_mkdir_with_parents(log_dir, 0755) != 0)
    {
        LOG_WARN("swap-image: cannot create %s (%s), falling back to /tmp", log_dir, strerror(errno));
        g_free(log_dir);
        log_dir = g_strdup("/tmp");
    }
    char *log_path = g_strdup_printf("%s/swap-image-%d.log", log_dir, (int)getpid());
    g_free(log_dir);

    /* 把脚本输出重定向到日志文件,失败时方便排错;不再串流给客户端 */
    char cmd[PATH_MAX + 512];
    snprintf(cmd, sizeof(cmd), "'%s' '%s' > '%s' 2>&1", args->script, args->image, log_path);

    LOG_WARN("swap-image: launching helper in background: %s", cmd);

    int sysrc = system(cmd);
    int script_ok = (sysrc != -1 && WIFEXITED(sysrc) && WEXITSTATUS(sysrc) == 0);

    /* 失败时同样查 .image_tag 兜底:多进程 SIGCHLD race 仍可能让 system() 返回非 0 */
    if (!script_ok)
    {
        char tag_path[PATH_MAX];
        char tag_content[160] = {0};
        if (resolve_image_tag_file(tag_path, sizeof(tag_path)) == ERRCODE_SUCCESS &&
            file_read_first_line(tag_path, tag_content, sizeof(tag_content)) == ERRCODE_SUCCESS &&
            strcmp(tag_content, args->image) == 0)
        {
            LOG_WARN("swap-image: system() rc=%d but .image_tag matches '%s', accept as success", sysrc, args->image);
            script_ok = 1;
        }
    }

    if (!script_ok)
    {
        LOG_ERROR("swap-image: script failed (rc=%d). See %s for details", sysrc, log_path);
        g_atomic_int_set(&g_reboot_in_progress, 0);
        g_free(log_path);
        g_free(args);
        return NULL;
    }

    LOG_WARN("swap-image: script succeeded, cleaning up child modules before execv");
    cleanup_all_modules();

    if (access(args->netnexus_path, X_OK) != 0)
    {
        LOG_ERROR("netnexus binary not executable at %s: %s", args->netnexus_path, strerror(errno));
        g_atomic_int_set(&g_reboot_in_progress, 0);
        g_free(log_path);
        g_free(args);
        return NULL;
    }

    LOG_WARN("swap-image: execv %s (image=%s)", args->netnexus_path, args->image);
    fflush(NULL);

    /* execv 前把 CWD 切到根目录:Dockerfile WORKDIR 让 netnexus 默认 CWD 是
     * /opt/netnexus/bin,而 swap-image.sh 会把 bin → bin.old。execv 继承 CWD
     * inode,下次 swap 触发的 `rm -rf bin.old` 会 unlink 这个 inode → 进程
     * 进入"deleted CWD"状态,后续 bash/getcwd 全部报错。chdir("/") 让新进程
     * 从根起步,bin/lib 无论怎么动都不影响 CWD。 */
    if (chdir("/") != 0)
    {
        LOG_WARN("swap-image: chdir(/) failed: %s (continuing)", strerror(errno));
    }

    /* execv 前关 fd ≥ 3,避免新进程继承监听 socket 撞 EADDRINUSE */
    {
        int closed = 0;
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 3, ~0U, 0) == 0)
        {
            closed = 1;
        }
#endif
        if (!closed)
        {
            DIR *d = opendir("/proc/self/fd");
            if (d)
            {
                int dfd = dirfd(d);
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL)
                {
                    if (ent->d_name[0] == '.')
                    {
                        continue;
                    }
                    int fd = atoi(ent->d_name);
                    if (fd >= 3 && fd != dfd)
                    {
                        close(fd);
                    }
                }
                closedir(d);
            }
            else
            {
                long max_fd = sysconf(_SC_OPEN_MAX);
                if (max_fd <= 0 || max_fd > 65536)
                {
                    max_fd = 1024;
                }
                for (int fd = 3; fd < (int)max_fd; fd++)
                {
                    close(fd);
                }
            }
        }
    }

    char *new_argv[] = {args->netnexus_path, NULL};
    execv(args->netnexus_path, new_argv);

    /* execv 失败才会到这里 */
    LOG_ERROR("execv(%s) failed: %s", args->netnexus_path, strerror(errno));
    g_atomic_int_set(&g_reboot_in_progress, 0);
    g_free(log_path);
    g_free(args);
    return NULL;
}

static int handle_dev_swap_image(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char image[160] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(image, text, sizeof(image));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (dev_validate_image_name(image) != ERRCODE_SUCCESS)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: invalid image name (allowed [A-Za-z0-9._:/-], <=128).\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_atomic_int_compare_and_exchange(&g_reboot_in_progress, 0, 1))
    {
        dev_send_cli_response(ctx, msg, "Dev: reboot/swap already in progress.\r\n");
        return ERRCODE_FAIL;
    }

    char script[PATH_MAX];
    if (dev_resolve_swap_image_script(script, sizeof(script)) != ERRCODE_SUCCESS)
    {
        g_atomic_int_set(&g_reboot_in_progress, 0);
        dev_send_cli_response(ctx, msg, "Dev Error: swap-image.sh not found in scripts dir.\r\n");
        return ERRCODE_FAIL;
    }

    /* 准备后台线程参数 */
    swap_async_args_t *args = g_new0(swap_async_args_t, 1);
    strlcpy(args->script, script, sizeof(args->script));
    strlcpy(args->image, image, sizeof(args->image));
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir && work_dir[0] != '\0')
    {
        snprintf(args->netnexus_path, sizeof(args->netnexus_path), "%s/bin/netnexus", work_dir);
    }
    else
    {
        strlcpy(args->netnexus_path, "/opt/netnexus/bin/netnexus", sizeof(args->netnexus_path));
    }

    LOG_WARN("Swap-image requested: image=%s, script=%s (async)", image, script);

    GError *err = NULL;
    GThread *th = g_thread_try_new("nn-swap", swap_async_worker, args, &err);
    if (!th)
    {
        g_atomic_int_set(&g_reboot_in_progress, 0);
        g_free(args);
        char buf[256];
        snprintf(buf, sizeof(buf), "Dev Error: failed to spawn swap thread: %s\r\n", err ? err->message : "unknown");
        if (err)
        {
            g_error_free(err);
        }
        dev_send_cli_response(ctx, msg, buf);
        return ERRCODE_FAIL;
    }
    g_thread_unref(th); /* detach,后台跑 */

    /* 立刻给客户端 ACK,不在 IPC 路径里等脚本 */
    const char *ack_log_dir = (work_dir && work_dir[0] != '\0') ? work_dir : "/opt/netnexus";
    char ack[384];
    snprintf(ack, sizeof(ack),
             "\r\nDev: swap-image started for '%s' (running in background).\r\n"
             "     Telnet will drop when re-exec fires; reconnect later.\r\n"
             "     Script log: %s/log/swap-image-{pid}.log\r\n",
             image, ack_log_dir);
    dev_send_cli_response(ctx, msg, ack);
    return ERRCODE_SUCCESS;
}

static int handle_set_log_level(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* cfg_id 1=debug 2=info 3=warn 4=error，由 commands.xml 中 keyword 元素定义 */
    log_level_t selected = LOG_LEVEL_DEBUG;
    int found = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1:
                selected = LOG_LEVEL_DEBUG;
                found = 1;
                break;
            case 2:
                selected = LOG_LEVEL_INFO;
                found = 1;
                break;
            case 3:
                selected = LOG_LEVEL_WARN;
                found = 1;
                break;
            case 4:
                selected = LOG_LEVEL_ERROR;
                found = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!found)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: missing log level keyword.\r\n");
        return ERRCODE_FAIL;
    }

    /* 本地生效 */
    log_set_level(selected);

    /* 广播给所有已注册模块，使各模块进程同步生效 */
    dev_broadcast_log_level((uint32_t)selected);

    /* 持久化到 DB */
    if (dev_db_set_log_level(selected) != 0)
    {
        dev_send_cli_response(ctx, msg, "Dev: log level applied but failed to persist to DB.\r\n");
        return ERRCODE_FAIL;
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "Dev: log level set to '%s' (broadcast and persisted).\r\n",
             log_level_to_string(selected));
    dev_send_cli_response(ctx, msg, resp);
    return ERRCODE_SUCCESS;
}

static const char *ipc_state_to_string(dev_ipc_costate_t state)
{
    switch (state)
    {
        case DEV_IPC_CODISCONNECTED:
            return "DISCONNECTED";
        case DEV_IPC_COCONNECTING:
            return "CONNECTING";
        case DEV_IPC_COHANDSHAKING:
            return "HANDSHAKING";
        case DEV_IPC_COCONNECTED:
            return "CONNECTED";
        case DEV_IPC_CORECONNECTING:
            return "RECONNECTING";
        default:
            return "UNKNOWN";
    }
}

/* IPC 连接 wire format 中每条 entry 的大小（见 ipc_context.c 注释） */
#define IPC_QCONNS_ENTRY_SIZE 100

/**
 * @brief 从 QUERY_IPC_CONNS 响应 payload 中解析并格式化第 i 条连接信息到 buf
 */
static void format_ipc_conn_entry(const uint8_t *entry, int idx, GString *buf)
{
    const uint8_t *p = entry;
    uint32_t v;
    uint16_t v16;

    memcpy(&v, p, 4);
    uint32_t remote_module_id = ntohl(v);
    p += 4;
    memcpy(&v, p, 4);
    uint32_t state = ntohl(v);
    p += 4;
    memcpy(&v, p, 4);
    uint32_t is_initiator = ntohl(v);
    p += 4;
    char remote_host[65];
    memcpy(remote_host, p, 64);
    remote_host[64] = '\0';
    p += 64;
    memcpy(&v16, p, 2);
    uint16_t remote_port = ntohs(v16);
    p += 4; /* port + pad */

    uint32_t hi, lo;
    memcpy(&hi, p, 4);
    hi = ntohl(hi);
    p += 4;
    memcpy(&lo, p, 4);
    lo = ntohl(lo);
    p += 4;
    time_t last_hb_sent = (time_t)(((uint64_t)hi << 32) | lo);

    memcpy(&hi, p, 4);
    hi = ntohl(hi);
    p += 4;
    memcpy(&lo, p, 4);
    lo = ntohl(lo);
    p += 4;
    time_t last_hb_recv = (time_t)(((uint64_t)hi << 32) | lo);

    memcpy(&v, p, 4);
    uint32_t reconnect_delay_ms = ntohl(v);

    /* 格式化心跳时间 */
    char hb_sent_str[32] = "N/A";
    char hb_recv_str[32] = "N/A";
    if (last_hb_sent > 0)
    {
        struct tm *t = localtime(&last_hb_sent);
        strftime(hb_sent_str, sizeof(hb_sent_str), "%H:%M:%S", t);
    }
    if (last_hb_recv > 0)
    {
        struct tm *t = localtime(&last_hb_recv);
        strftime(hb_recv_str, sizeof(hb_recv_str), "%H:%M:%S", t);
    }

    g_string_append_printf(buf, "  Connection #%d (peer: 0x%08X):\r\n", idx, remote_module_id);
    g_string_append_printf(buf, "    %-16s: %s\r\n", "Direction",
                           is_initiator ? "Active (Initiator)" : "Passive (Acceptor)");
    g_string_append_printf(buf, "    %-16s: %s\r\n", "State", ipc_state_to_string((dev_ipc_costate_t)state));
    if (is_initiator && remote_host[0] != '\0')
    {
        g_string_append_printf(buf, "    %-16s: %s:%u\r\n", "Remote Addr", remote_host, remote_port);
    }
    g_string_append_printf(buf, "    %-16s: %s\r\n", "HB Sent", hb_sent_str);
    g_string_append_printf(buf, "    %-16s: %s\r\n", "HB Recv", hb_recv_str);
    g_string_append_printf(buf, "    %-16s: %u ms\r\n", "Reconnect Delay", reconnect_delay_ms);
}

/**
 * @brief 将 QUERY_IPC_CONNS 响应 payload 格式化为可读文本，追加到 buf[*off]
 */
static void format_conns_payload(const uint8_t *payload, uint32_t payload_len, const char *module_name,
                                 uint32_t target_id, GString *buf)
{
    const uint8_t *p = payload;
    uint32_t v;
    memcpy(&v, p, 4);
    uint32_t num_conns = ntohl(v);
    p += 4;

    g_string_append_printf(buf, "\r\nIPC Connections of module '%s' (ID: 0x%08X) — %u connection(s):\r\n", module_name,
                           target_id, num_conns);

    uint32_t expected_len = 4 + num_conns * IPC_QCONNS_ENTRY_SIZE;
    if (payload_len < expected_len)
    {
        g_string_append_printf(buf, "  Error: response payload truncated (%u < %u bytes).\r\n\r\n", payload_len,
                               expected_len);
    }
    else if (num_conns == 0)
    {
        g_string_append(buf, "  (no connections)\r\n\r\n");
    }
    else
    {
        for (uint32_t i = 0; i < num_conns; i++)
        {
            format_ipc_conn_entry(p + i * IPC_QCONNS_ENTRY_SIZE, (int)i, buf);
            g_string_append(buf, "\r\n");
        }
    }
}

static int handle_show_ipc(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char module_name[DEV_MODULE_NAME_MAX_LEN] = {0};

    /* 解析模块名称参数（cfg_id=1） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(module_name, text, sizeof(module_name));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (module_name[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Error: missing module name.\r\n");
        return ERRCODE_FAIL;
    }

    /* 从注册表查找模块 ID */
    uint32_t target_id = 0;
    if (dev_get_module_id_by_name(module_name, &target_id) != ERRCODE_SUCCESS)
    {
        char err[128];
        snprintf(err, sizeof(err), "Error: module '%s' not found in registry.\r\n", module_name);
        dev_send_cli_response(ctx, msg, err);
        return ERRCODE_FAIL;
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if (target_id == DEV_MODULE_ID_DEV)
    {
        /* 自查询：DEV 无自连接，直接从 ctx 构造载荷 */
        uint32_t pl_len;
        uint8_t *pl = dev_ipc_build_conns_payload(ctx, &pl_len);
        format_conns_payload(pl, pl_len, module_name, target_id, buf);
        g_free(pl);
    }
    else
    {
        /* 向目标模块发 RPC，查询其自身所有 IPC 连接 */
        dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_QUERY_IPC_CONNS, DEV_MODULE_ID_DEV,
                                                        target_id, 0, NULL, 0, NULL);
        if (!req)
        {
            dev_send_cli_response(ctx, msg, "Error: failed to create IPC query message.\r\n");
            return ERRCODE_FAIL;
        }

        dev_ipc_message_t *resp = dev_ipc_query(ctx, target_id, req, 3000);
        dev_ipc_message_free(req);

        if (!resp || !resp->payload || resp->payload_len < 4)
        {
            g_string_append_printf(buf, "\r\nError: no response from module '%s' (timeout or not connected).\r\n\r\n",
                                   module_name);
            dev_send_cli_response(ctx, msg, buf->str);
            g_string_free(buf, TRUE);
            if (resp)
            {
                dev_ipc_message_free(resp);
            }
            return ERRCODE_FAIL;
        }

        format_conns_payload((const uint8_t *)resp->payload, resp->payload_len, module_name, target_id, buf);
        dev_ipc_message_free(resp);
    }

    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, buf);
}

static int handle_ping(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char ip[64] = {0};
    gboolean ping_ipv6 = FALSE;

    /* 解析目标地址参数
     * cfg_id=1: ping <ipv4-address>
     * cfg_id=2: ping ipv6 <ipv6-address> 的 ipv6 关键字
     * cfg_id=3: ping ipv6 <ipv6-address>
     */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 2)
        {
            ping_ipv6 = TRUE;
        }
        else if (entry.cfg_id == 1 || entry.cfg_id == 3)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(ip, text, sizeof(ip));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (ip[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Error: missing IP address\r\n");
        return ERRCODE_FAIL;
    }

    /* 验证 IP 地址格式并规范化，防止命令注入 */
    net_addr_t addr;
    if (net_addr_from_str(ip, &addr) != 0 || (addr.family != AF_INET && addr.family != AF_INET6))
    {
        dev_send_cli_response(ctx, msg, "Error: invalid IP address format\r\n");
        return ERRCODE_FAIL;
    }
    if (ping_ipv6 && addr.family != AF_INET6)
    {
        dev_send_cli_response(ctx, msg, "Error: ping ipv6 requires an IPv6 address\r\n");
        return ERRCODE_FAIL;
    }
    if (!ping_ipv6 && addr.family != AF_INET)
    {
        dev_send_cli_response(ctx, msg, "Error: ping requires an IPv4 address; use 'ping ipv6 <addr>' for IPv6\r\n");
        return ERRCODE_FAIL;
    }

    char normalized_ip[64];
    net_addr_to_str(&addr, normalized_ip, sizeof(normalized_ip));
    const char *family_opt = ping_ipv6 ? "-6" : "-4";

    if (g_ping_stream_fp)
    {
        pclose(g_ping_stream_fp);
        g_ping_stream_fp = NULL;
        g_ping_stream_prefixed = 0;
    }

    /* 构造 ping 命令并执行（优先 stdbuf 行缓冲，不可用则回退原生 ping） */
    char cmd[320];
    snprintf(cmd, sizeof(cmd),
             "sh -c 'if command -v stdbuf >/dev/null 2>&1; then exec stdbuf -oL ping %s -c 4 -W 2 %s; "
             "else exec ping %s -c 4 -W 2 %s; fi' 2>&1",
             family_opt, normalized_ip, family_opt, normalized_ip);

    g_ping_stream_fp = popen(cmd, "r");
    if (!g_ping_stream_fp)
    {
        dev_send_cli_response(ctx, msg, "Error: failed to execute ping command\r\n");
        return ERRCODE_FAIL;
    }
    g_ping_stream_prefixed = 0;

    return ping_stream_send_next(ctx, msg);
}

// ============================================================================
// 动态候选值查询（Tab/? 补全）
// ============================================================================

static gboolean collect_module_name_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    GByteArray *buf = (GByteArray *)data;
    g_byte_array_append(buf, (const guint8 *)module->name, (guint)strlen(module->name) + 1);
    return FALSE;
}

void dev_cli_handle_query_candidates(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    GByteArray *buf = g_byte_array_new();
    dev_module_foreach(collect_module_name_cb, buf);

    /* 末尾额外 '\0' 标记结束 */
    guint8 nul = '\0';
    g_byte_array_append(buf, &nul, 1);

    guint payload_len = buf->len;
    uint8_t *payload = g_byte_array_free(buf, FALSE);

    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES_RESP, DEV_MODULE_ID_DEV,
                                                     msg->src_module_id, msg->request_id, payload, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }
    dev_ipc_message_free(msg);
}

// ============================================================================
// 主入口
// ============================================================================

int dev_cli_handle_continue(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (g_ping_stream_fp)
    {
        return ping_stream_send_next(ctx, msg);
    }
    return cli_chunk_stream_continue(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg);
}

void dev_cli_cleanup_state(void)
{
    if (g_ping_stream_fp)
    {
        pclose(g_ping_stream_fp);
        g_ping_stream_fp = NULL;
        g_ping_stream_prefixed = 0;
    }
    cli_chunk_stream_reset(&g_dev_local->show_stream);
}

int dev_cli_handle_message(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(&g_dev_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        dev_send_cli_response(ctx, msg, "Dev Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received TLV payload (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case DEV_CLI_GROUP_ID_SHOW_VERSION:
            result = handle_show_version(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_SYSNAME:
            result = handle_sysname(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_SHOW_MODULE:
            result = handle_show_module(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_LOG_LEVEL:
            result = handle_set_log_level(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_PING:
            result = handle_ping(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_SHOW_IPC:
            result = handle_show_ipc(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_REBOOT:
            result = handle_reboot(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_SWAP_IMAGE:
            result = handle_dev_swap_image(ctx, msg, &parser);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            dev_send_cli_response(ctx, msg, "Dev Error: Unknown command.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
