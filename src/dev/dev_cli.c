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
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli.h"
#include "db.h"
#include "dev_db.h"
#include "dev_fs_cli.h"
#include "dev_main.h"
#include "dev_module.h"
#include "dev_ping.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "path_utils.h"
#include "syslog_report.h"
#include "vrf.h"

static gint g_reboot_in_progress = 0;

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
    const char *phase;
    /* on-demand 模块在未启动状态（REGISTERED + 无 child）显示为 ON-DEMAND，
     * 这是空闲待命的正常状态；框架的 precheck 应将其视为健康。 */
    if (module->on_demand && module->phase == DEV_PHASE_REGISTERED && module->child_pid <= 0)
    {
        phase = "ON-DEMAND";
    }
    else
    {
        phase = dev_phase_to_string(module->phase);
    }
    const char *dev_ipc_state =
        (module->module_id == DEV_MODULE_ID_DEV || dev_ipc_is_connected(ctx->dev_ipc_ctx, module->module_id)) ? "up"
                                                                                                              : "down";
    /* PID 列：DEV 自身显示当前进程 pid；其它模块显示 child_pid（未运行=0 显示 "-"） */
    char pid_str[16];
    pid_t pid_to_show = (module->module_id == DEV_MODULE_ID_DEV) ? getpid() : module->child_pid;
    if (pid_to_show > 0)
    {
        snprintf(pid_str, sizeof(pid_str), "%d", (int)pid_to_show);
    }
    else
    {
        snprintf(pid_str, sizeof(pid_str), "-");
    }
    g_string_append_printf(ctx->resp, "  %-10u %-14s %-12s %-6u %-6s %s\r\n", module->module_id, module->name, phase,
                           module->port, dev_ipc_state, pid_str);

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

typedef struct ping_stream_job
{
    dev_ping_session_t *session;
    uint32_t src_module_id;
    uint32_t request_id;
    uint32_t line_id;
} ping_stream_job_t;

static GAsyncQueue *g_ping_stream_queue = NULL;
static gint g_ping_stream_worker_started = 0;

static void ping_stream_send_final(uint32_t dst_module_id, uint32_t request_id, const char *text)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!ctx || !text)
    {
        return;
    }

    char *data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_DEV, dst_module_id, request_id,
                                                     data, strlen(data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(data);
    }
}

static void *ping_stream_worker_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "dev-pingstream");
    while (1)
    {
        ping_stream_job_t *job = (ping_stream_job_t *)g_async_queue_pop(g_ping_stream_queue);
        if (!job || !job->session)
        {
            g_free(job);
            continue;
        }

        char line[256];
        char out[320];
        int prefixed = 0;
        while (dev_ping_next_line(job->session, line, sizeof(line)))
        {
            if (!prefixed)
            {
                prefixed = 1;
                snprintf(out, sizeof(out), "\r\n%s\r\n", line);
            }
            else
            {
                snprintf(out, sizeof(out), "%s\r\n", line);
            }
            (void)cli_line_progress_send(dev_get_ipc_ctx(), job->line_id, out);
        }

        dev_ping_close(job->session);
        ping_stream_send_final(job->src_module_id, job->request_id, "");
        g_free(job);
    }
    return NULL;
}

static int ping_stream_worker_ensure_started(void)
{
    if (g_atomic_int_get(&g_ping_stream_worker_started))
    {
        return ERRCODE_SUCCESS;
    }
    if (!g_ping_stream_queue)
    {
        g_ping_stream_queue = g_async_queue_new();
    }
    if (!g_atomic_int_compare_and_exchange(&g_ping_stream_worker_started, 0, 1))
    {
        return ERRCODE_SUCCESS;
    }
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, ping_stream_worker_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        LOG_ERROR("Failed to spawn ping stream worker: %s", strerror(rc));
        g_atomic_int_set(&g_ping_stream_worker_started, 0);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static int ping_stream_start(dev_ping_session_t *session, dev_ipc_message_t *msg, uint32_t line_id)
{
    if (!session || !msg || msg->request_id == 0)
    {
        return ERRCODE_FAIL;
    }
    if (ping_stream_worker_ensure_started() != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    ping_stream_job_t *job = g_malloc0(sizeof(*job));
    job->session = session;
    job->src_module_id = msg->src_module_id;
    job->request_id = msg->request_id;
    job->line_id = line_id;
    g_async_queue_push(g_ping_stream_queue, job);
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
                           "  %-10s %-14s %-12s %-6s %-6s %s\r\n"
                           "  ----------------------------------------------------------------\r\n",
                           "ID", "Name", "Phase", "Port", "IPC", "PID");

    dev_module_foreach(show_module_callback, &show_ctx);
    g_string_append(show_ctx.resp, "\r\n");
    return cli_chunk_stream_start(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg, show_ctx.resp);
}

/* ----------------------------------------------------------------------------
 * show dev subscriptions —— 打印 DEV 视角的模块订阅关系，给 CI 定位卡死用
 * 输出格式：每个 module 列名 + phase + 订阅者列表（其它模块对它的 SUBSCRIBE）
 * -------------------------------------------------------------------------- */
static gboolean show_subscriptions_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    show_module_ctx_t *ctx = (show_module_ctx_t *)data;
    dev_module_t *module = (dev_module_t *)value;

    const char *phase;
    if (module->on_demand && module->phase == DEV_PHASE_REGISTERED && module->child_pid <= 0)
    {
        phase = "ON-DEMAND";
    }
    else
    {
        phase = dev_phase_to_string(module->phase);
    }

    g_string_append_printf(ctx->resp, "  %-14s id=0x%08X phase=%-10s epoch=%u port=%u subscribers=[", module->name,
                           module->module_id, phase, module->epoch, module->port);
    int first = 1;
    for (GList *l = module->subscribers; l != NULL; l = l->next)
    {
        uint32_t sub_id = GPOINTER_TO_UINT(l->data);
        dev_module_t *sub_mod = dev_module_find(sub_id);
        if (!first)
        {
            g_string_append_c(ctx->resp, ' ');
        }
        if (sub_mod)
        {
            g_string_append_printf(ctx->resp, "%s(0x%08X)", sub_mod->name, sub_id);
        }
        else
        {
            g_string_append_printf(ctx->resp, "0x%08X", sub_id);
        }
        first = 0;
    }
    g_string_append(ctx->resp, "]\r\n");
    return FALSE;
}

/* 收集需要 RPC 查询的模块 id 列表（除自己之外 IPC 已连上的） */
typedef struct collect_connected_ctx
{
    dev_ipc_context_t *dev_ipc_ctx;
    GArray *ids;
} collect_connected_ctx_t;

static gboolean collect_connected_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    collect_connected_ctx_t *cc = (collect_connected_ctx_t *)data;
    dev_module_t *module = (dev_module_t *)value;
    if (!module || module->module_id == DEV_MODULE_ID_DEV)
    {
        return FALSE;
    }
    if (dev_ipc_is_connected(cc->dev_ipc_ctx, module->module_id))
    {
        g_array_append_val(cc->ids, module->module_id);
    }
    return FALSE;
}

static int handle_show_subscriptions(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
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

    /* Section 1: DEV-side view —— 每个模块的"被谁订阅"列表 */
    g_string_append(show_ctx.resp, "\r\n========== DEV view: subscribers per module ==========\r\n");
    dev_module_foreach(show_subscriptions_callback, &show_ctx);

    /* Section 2: 模块本地视图 —— 通过 RPC 询问每个已连接的模块"你订阅了谁"
     * 由 IPC 库层（ipc_context.c handle_frame）自动响应，无需各业务模块写 handler */
    g_string_append(show_ctx.resp, "\r\n========== Module local view: subscriptions per module ==========\r\n");
    /* DEV 自己也带上一份本地视图，便于看清依赖闭环 */
    {
        char *local_dump = dev_ipc_format_local_subs(ctx, NULL);
        g_string_append_printf(show_ctx.resp, "[dev local sub_mgr]\r\n%s",
                               local_dump ? local_dump : "  (unavailable)\r\n");
        g_free(local_dump);
    }

    collect_connected_ctx_t cc = {.dev_ipc_ctx = ctx, .ids = g_array_new(FALSE, FALSE, sizeof(uint32_t))};
    dev_module_foreach(collect_connected_cb, &cc);
    for (guint i = 0; i < cc.ids->len; i++)
    {
        uint32_t tid = g_array_index(cc.ids, uint32_t, i);
        dev_module_t *m = dev_module_find(tid);
        const char *mname = m ? m->name : "?";
        g_string_append_printf(show_ctx.resp, "[%s(0x%08X) local sub_mgr]\r\n", mname, tid);

        dev_ipc_message_t *req =
            dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_QUERY_SUBS, ctx->module_id, tid, 0, NULL, 0, NULL);
        if (!req)
        {
            g_string_append(show_ctx.resp, "  (oom)\r\n");
            continue;
        }
        dev_ipc_message_t *resp = dev_ipc_query(ctx, tid, req, 1500);
        dev_ipc_message_free(req);
        if (!resp)
        {
            g_string_append(show_ctx.resp, "  (no response within 1.5s)\r\n");
            continue;
        }
        if (resp->payload && resp->payload_len > 0)
        {
            const char *text = (const char *)resp->payload;
            /* 文本里换行用 \n；CLI 输出习惯 \r\n */
            for (const char *p = text; *p; p++)
            {
                if (*p == '\n')
                {
                    g_string_append(show_ctx.resp, "\r\n");
                }
                else if (*p != '\r')
                {
                    g_string_append_c(show_ctx.resp, *p);
                }
            }
        }
        else
        {
            g_string_append(show_ctx.resp, "  (empty response)\r\n");
        }
        dev_ipc_message_free(resp);
    }
    g_array_free(cc.ids, TRUE);

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

    if (resolve_version_file(version_path, sizeof(version_path)) == 0)
    {
        if (file_read_first_line(version_path, version, sizeof(version)) != 0)
        {
            strlcpy(version, "unknown", sizeof(version));
        }
    }

    /* 镜像 tag 由 swap-image.sh 在替换镜像后写入,首次部署可能不存在 */
    char image_tag[160] = "n/a";
    char image_tag_path[PATH_MAX];
    if (resolve_image_tag_file(image_tag_path, sizeof(image_tag_path)) == ERRCODE_SUCCESS)
    {
        if (file_read_first_line(image_tag_path, image_tag, sizeof(image_tag)) != 0)
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

static int dev_set_kernel_hostname(const char *hostname)
{
    if (!hostname || hostname[0] == '\0')
    {
        return -1;
    }

    if (sethostname(hostname, strlen(hostname)) != 0)
    {
        LOG_WARN("DEV: failed to set kernel hostname to %s: %s", hostname, strerror(errno));
        return -1;
    }
    return 0;
}

static int handle_sysname(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    /* DB 不在线时拒绝：避免 sysname 改了内存（CLI 提示）但 DB 写不到 */
    if (db_rpc_guard_reject(ctx, msg, "Dev"))
    {
        return ERRCODE_FAIL;
    }

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

    const char *kernel_hostname = is_no ? CLI_SYSNAME_DEFAULT : hostname;
    char old_kernel_hostname[CLI_SYSNAME_MAX_LEN] = {0};
    (void)gethostname(old_kernel_hostname, sizeof(old_kernel_hostname) - 1u);

    if (is_no)
    {
        /* no sysname：清空 DB + 通知 CLI/内核 hostname 恢复默认 */
        if (dev_set_kernel_hostname(kernel_hostname) != 0)
        {
            dev_send_cli_response(ctx, msg, "Dev Error: failed to set kernel hostname\r\n");
            return ERRCODE_FAIL;
        }
        if (dev_db_set_sysname("") != 0)
        {
            if (old_kernel_hostname[0] != '\0')
            {
                (void)dev_set_kernel_hostname(old_kernel_hostname);
            }
            dev_send_cli_response(ctx, msg, "Dev Error: failed to clear sysname\r\n");
            return ERRCODE_FAIL;
        }
        dev_push_sysname_to_cli(ctx, "");
        dev_send_cli_response_with_type(ctx, msg, CLI_MSG_TYPE_SYSNAME_UPDATE_RESP, "");
        return ERRCODE_SUCCESS;
    }

    if (hostname[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Dev Error: hostname required\r\n");
        return ERRCODE_FAIL;
    }

    if (dev_set_kernel_hostname(kernel_hostname) != 0)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: failed to set kernel hostname\r\n");
        return ERRCODE_FAIL;
    }

    if (dev_db_set_sysname(hostname) != 0)
    {
        if (old_kernel_hostname[0] != '\0')
        {
            (void)dev_set_kernel_hostname(old_kernel_hostname);
        }
        dev_send_cli_response(ctx, msg, "Dev Error: failed to persist sysname\r\n");
        return ERRCODE_FAIL;
    }
    dev_push_sysname_to_cli(ctx, hostname);
    dev_send_cli_response_with_type(ctx, msg, CLI_MSG_TYPE_SYSNAME_UPDATE_RESP, hostname);
    return ERRCODE_SUCCESS;
}

/* reboot 必须在单独线程跑（不能阻塞 DEV worker，否则会和子模块 SUBSCRIBE RPC 形成跨进程死锁）。
 * 这个线程必须常驻——它通过 fork() 创建子进程，PR_SET_PDEATHSIG=SIGTERM 会让子进程
 * 在创建它的线程退出时收到 SIGTERM。所以 reboot 线程持续等下一次请求，永不退出。 */
static pthread_mutex_t g_reboot_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_reboot_cond = PTHREAD_COND_INITIALIZER;
static int g_reboot_pending = 0;
static int g_reboot_worker_started = 0;

static void *reboot_worker_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "dev-reboot");
    while (1)
    {
        pthread_mutex_lock(&g_reboot_mutex);
        while (!g_reboot_pending)
        {
            pthread_cond_wait(&g_reboot_cond, &g_reboot_mutex);
        }
        g_reboot_pending = 0;
        pthread_mutex_unlock(&g_reboot_mutex);

        int ret = dev_reboot_software();
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("Software reboot failed");
        }
        g_atomic_int_set(&g_reboot_in_progress, 0);
    }
    return NULL;
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

    /* 第一次进入：起常驻 reboot 线程 */
    if (g_atomic_int_compare_and_exchange(&g_reboot_worker_started, 0, 1))
    {
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&tid, &attr, reboot_worker_thread, NULL);
        pthread_attr_destroy(&attr);
        if (rc != 0)
        {
            LOG_ERROR("Failed to spawn reboot worker thread: %s", strerror(rc));
            g_atomic_int_set(&g_reboot_worker_started, 0);
            g_atomic_int_set(&g_reboot_in_progress, 0);
            return ERRCODE_FAIL;
        }
    }

    pthread_mutex_lock(&g_reboot_mutex);
    g_reboot_pending = 1;
    pthread_cond_signal(&g_reboot_cond);
    pthread_mutex_unlock(&g_reboot_mutex);

    return ERRCODE_SUCCESS;
}

/* ============================================================================
 * process reboot/start 异步等 READY 才回响应
 *
 * 不能阻塞 DEV 的 IPC worker（会和被等模块的 SUBSCRIBE RPC 形成死锁），
 * 也不能阻塞 CLI 派发线程；专门起一个常驻 poller 线程消费 job 队列，每 200ms
 * 查一次目标模块 phase（DEV 进程内直接 dev_module_find），ready / 超时再补响应。
 * ============================================================================ */
enum
{
    OP_REBOOT = 1,
    OP_START = 2,
    OP_STOP = 3
};

typedef struct process_wait_job
{
    int op; /* OP_START / OP_REBOOT */
    uint32_t module_id;
    uint32_t src_module_id; /* 原始请求方（CLI 模块或 ACCESS 模块） */
    uint32_t line_id;       /* ACCESS line_id，用于异步进度输出 */
    char modname[DEV_MODULE_NAME_MAX_LEN];
    uint32_t epoch_before; /* 触发动作前模块的 epoch，等到 READY 且 epoch>before 才算新进程就绪 */
    pid_t pid_before;
    int64_t start_us;
    int64_t deadline_us;
    int64_t next_progress_us;
} process_wait_job_t;

typedef struct process_wait_request
{
    process_wait_job_t *job;
    uint32_t request_id;
} process_wait_request_t;

#define PROCESS_WAIT_TIMEOUT_MS 30000
#define PROCESS_WAIT_POLL_US (200 * 1000)
#define PROCESS_WAIT_PROGRESS_US (1000 * 1000)

static GAsyncQueue *g_process_wait_queue = NULL;
static gint g_process_wait_worker_started = 0;
static GMutex g_process_wait_mutex;
static process_wait_job_t *g_process_wait_active = NULL;

static void send_cli_response_to(uint32_t dst_module_id, uint32_t request_id, const char *text)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!ctx || !text)
    {
        return;
    }
    char *data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_DEV, dst_module_id, request_id,
                                                     data, strlen(data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
}

static const char *process_wait_op_label(int op)
{
    switch (op)
    {
        case OP_REBOOT:
            return "reboot";
        case OP_START:
            return "start";
        case OP_STOP:
            return "stop";
        default:
            return "process";
    }
}

static void process_wait_send_progress(process_wait_job_t *job, const char *text)
{
    if (!job || !text)
    {
        return;
    }
    (void)cli_line_progress_send(dev_get_ipc_ctx(), job->line_id, text);
}

static void process_wait_finish(process_wait_job_t *job)
{
    g_mutex_lock(&g_process_wait_mutex);
    if (g_process_wait_active == job)
    {
        g_process_wait_active = NULL;
    }
    g_mutex_unlock(&g_process_wait_mutex);
    g_free(job);
}

static int process_wait_schedule(process_wait_job_t *job, uint32_t request_id)
{
    if (!job || request_id == 0)
    {
        return ERRCODE_FAIL;
    }
    process_wait_request_t *req = g_malloc0(sizeof(*req));
    req->job = job;
    req->request_id = request_id;
    g_async_queue_push(g_process_wait_queue, req);
    return ERRCODE_SUCCESS;
}

static void *process_wait_worker_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "dev-procwait");
    while (1)
    {
        process_wait_request_t *req = (process_wait_request_t *)g_async_queue_pop(g_process_wait_queue);
        if (!req || !req->job)
        {
            g_free(req);
            continue;
        }
        process_wait_job_t *job = req->job;

        char buf[160];
        while (1)
        {
            dev_module_t *m = dev_module_find(job->module_id);
            const char *op_label = process_wait_op_label(job->op);
            if (!m)
            {
                snprintf(buf, sizeof(buf), "Dev Error: module %s vanished during %s\r\n", job->modname, op_label);
                send_cli_response_to(job->src_module_id, req->request_id, buf);
                process_wait_finish(job);
                break;
            }
            if (job->op == OP_STOP && m->phase == DEV_PHASE_REGISTERED && m->child_pid <= 0)
            {
                snprintf(buf, sizeof(buf), "Dev: stop %s ok (pid=%d).\r\n", m->name, job->pid_before);
                send_cli_response_to(job->src_module_id, req->request_id, buf);
                process_wait_finish(job);
                break;
            }
            if (m->phase == DEV_PHASE_READY && m->epoch > job->epoch_before)
            {
                snprintf(buf, sizeof(buf), "Dev: %s %s ok (pid=%d).\r\n", op_label, m->name, m->child_pid);
                send_cli_response_to(job->src_module_id, req->request_id, buf);
                process_wait_finish(job);
                break;
            }
            int64_t now_us = (int64_t)g_get_monotonic_time();
            if (now_us >= job->deadline_us)
            {
                const char *state_label = (job->op == OP_STOP) ? "STOPPED" : "READY";
                snprintf(buf, sizeof(buf), "Dev Error: %s %s timed out waiting for %s (phase=%u, pid=%d).\r\n",
                         op_label, job->modname, state_label, m->phase, m->child_pid);
                send_cli_response_to(job->src_module_id, req->request_id, buf);
                process_wait_finish(job);
                break;
            }
            if (now_us >= job->next_progress_us)
            {
                uint32_t elapsed_sec = (uint32_t)((now_us - job->start_us) / 1000000);
                if (elapsed_sec == 0)
                {
                    snprintf(buf, sizeof(buf), "Dev: %s requested, waiting for %s...\r\n", op_label,
                             (job->op == OP_STOP) ? "STOPPED" : "READY");
                }
                else
                {
                    snprintf(buf, sizeof(buf), "Dev: waiting for %s (%us)...\r\n",
                             (job->op == OP_STOP) ? "STOPPED" : "READY", elapsed_sec);
                }
                job->next_progress_us = now_us + PROCESS_WAIT_PROGRESS_US;
                process_wait_send_progress(job, buf);
            }

            int64_t sleep_us = job->next_progress_us - now_us;
            if (sleep_us > PROCESS_WAIT_POLL_US)
            {
                sleep_us = PROCESS_WAIT_POLL_US;
            }
            if (sleep_us > 0)
            {
                g_usleep((gulong)sleep_us);
            }
        }
        g_free(req);
    }
    return NULL;
}

static int process_wait_worker_ensure_started(void)
{
    if (g_atomic_int_get(&g_process_wait_worker_started))
    {
        return ERRCODE_SUCCESS;
    }
    if (!g_process_wait_queue)
    {
        g_process_wait_queue = g_async_queue_new();
    }
    if (!g_atomic_int_compare_and_exchange(&g_process_wait_worker_started, 0, 1))
    {
        return ERRCODE_SUCCESS;
    }
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, process_wait_worker_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        LOG_ERROR("Failed to spawn process-wait worker: %s", strerror(rc));
        g_atomic_int_set(&g_process_wait_worker_started, 0);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static int process_wait_enqueue(int op, dev_module_t *m, dev_ipc_message_t *msg, uint32_t epoch_before,
                                uint32_t line_id)
{
    if (process_wait_worker_ensure_started() != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    g_mutex_lock(&g_process_wait_mutex);
    if (g_process_wait_active)
    {
        g_mutex_unlock(&g_process_wait_mutex);
        return ERRCODE_FAIL;
    }

    process_wait_job_t *job = g_malloc0(sizeof(*job));
    job->op = op;
    job->module_id = m->module_id;
    job->src_module_id = msg->src_module_id;
    job->line_id = line_id;
    snprintf(job->modname, sizeof(job->modname), "%s", m->name);
    job->epoch_before = epoch_before;
    job->pid_before = m->child_pid;
    job->start_us = (int64_t)g_get_monotonic_time();
    job->deadline_us = job->start_us + (int64_t)PROCESS_WAIT_TIMEOUT_MS * 1000;
    job->next_progress_us = job->start_us;
    g_process_wait_active = job;
    g_mutex_unlock(&g_process_wait_mutex);

    if (process_wait_schedule(job, msg->request_id) != ERRCODE_SUCCESS)
    {
        process_wait_finish(job);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 process { reboot | start | stop } <module-name>
 *
 * 子命令通过 keyword 的 cfg-id 区分（XML 中 reboot=1, start=2, stop=3, modname=4）：
 *   - reboot：进程在跑→SIGTERM + pending_restart→SIGCHLD respawn；不在跑→直接 spawn；
 *             响应延迟到目标模块 phase=READY 且 epoch 推进后才送出
 *   - start ：进程不在跑→spawn；在跑→提示已在运行（已在跑时立即响应）；
 *             新拉起的进程响应延迟到 phase=READY 才送出
 *   - stop  ：进程在跑→SIGTERM + pending_stop（SIGCHLD 不重启、不告警）；不在跑→no-op
 *             响应延迟到目标模块 STOPPED 才送出
 */
static int handle_process_cmd(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int op = 0;
    char modname[DEV_MODULE_NAME_MAX_LEN] = {0};
    uint32_t line_id = UINT32_MAX;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ACCESS_LINE)
            {
                line_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
        }
        else
        {
            switch (entry.cfg_id)
            {
                case 1: /* reboot */
                    op = OP_REBOOT;
                    break;
                case 2: /* start */
                    op = OP_START;
                    break;
                case 3: /* stop */
                    op = OP_STOP;
                    break;
                case 4: /* <module-name> */
                {
                    const char *s = cli_tlv_entry_get_text(&entry);
                    if (s)
                    {
                        snprintf(modname, sizeof(modname), "%s", s);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (op == 0 || modname[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Dev Error: usage: process {reboot|start|stop} <module>\r\n");
        return ERRCODE_FAIL;
    }

    uint32_t module_id = 0;
    if (dev_get_module_id_by_name(modname, &module_id) != ERRCODE_SUCCESS)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "Dev Error: module '%s' not registered.\r\n", modname);
        dev_send_cli_response(ctx, msg, buf);
        return ERRCODE_FAIL;
    }
    if (module_id == DEV_MODULE_ID_DEV)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: cannot manage DEV via this command.\r\n");
        return ERRCODE_FAIL;
    }

    dev_module_t *m = dev_module_find(module_id);
    if (!m)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: module entry not found.\r\n");
        return ERRCODE_FAIL;
    }
    if (m->exe_name[0] == '\0' && op != OP_STOP)
    {
        dev_send_cli_response(ctx, msg, "Dev Error: module has no exe_name, cannot spawn.\r\n");
        return ERRCODE_FAIL;
    }

    char buf[160];
    switch (op)
    {
        case OP_STOP:
            if (m->child_pid <= 0)
            {
                snprintf(buf, sizeof(buf), "Dev: %s is not running.\r\n", m->name);
                dev_send_cli_response(ctx, msg, buf);
                return ERRCODE_SUCCESS;
            }
            m->pending_stop = 1;
            m->pending_restart = 0;
            LOG_INFO("Process stop: SIGTERM %s (pid=%d), pending_stop=1", m->name, m->child_pid);
            if (kill(m->child_pid, SIGTERM) != 0)
            {
                snprintf(buf, sizeof(buf), "Dev Error: SIGTERM %s failed: %s\r\n", m->name, strerror(errno));
                m->pending_stop = 0;
                dev_send_cli_response(ctx, msg, buf);
                return ERRCODE_FAIL;
            }
            if (process_wait_enqueue(OP_STOP, m, msg, m->epoch, line_id) != ERRCODE_SUCCESS)
            {
                snprintf(buf, sizeof(buf), "Dev: stop %s requested (pid=%d), wait failed.\r\n", m->name, m->child_pid);
                dev_send_cli_response(ctx, msg, buf);
            }
            return ERRCODE_SUCCESS;

        case OP_START:
        {
            if (m->child_pid > 0)
            {
                snprintf(buf, sizeof(buf), "Dev: %s already running (pid=%d).\r\n", m->name, m->child_pid);
                dev_send_cli_response(ctx, msg, buf);
                return ERRCODE_SUCCESS;
            }
            /* 人工 start：清掉历史 crash 窗口，让模块从干净状态开始计数；
             * 这样自动 respawn 触发上限放弃后，运维 process start 即可重置。 */
            m->crash_count = 0;
            m->last_crash_time = 0;
            uint32_t epoch_before = m->epoch;
            if (dev_module_respawn(m) != ERRCODE_SUCCESS)
            {
                snprintf(buf, sizeof(buf), "Dev Error: failed to spawn %s\r\n", m->name);
                dev_send_cli_response(ctx, msg, buf);
                return ERRCODE_FAIL;
            }
            dev_ipc_connect(ctx, m->module_id, DEV_IPC_HOST_LOCAL, m->port);
            /* 异步等 READY；worker 会替我们回响应 */
            if (process_wait_enqueue(OP_START, m, msg, epoch_before, line_id) != ERRCODE_SUCCESS)
            {
                snprintf(buf, sizeof(buf), "Dev: start %s spawned (pid=%d), wait failed.\r\n", m->name, m->child_pid);
                dev_send_cli_response(ctx, msg, buf);
            }
            /* 注意：实际响应由 worker 在 READY 后发送 */
            return ERRCODE_SUCCESS;
        }

        case OP_REBOOT:
        {
            uint32_t epoch_before = m->epoch;
            if (m->child_pid > 0)
            {
                m->pending_restart = 1;
                LOG_INFO("Process reboot: SIGTERM %s (pid=%d), pending_restart=1", m->name, m->child_pid);
                if (kill(m->child_pid, SIGTERM) != 0)
                {
                    snprintf(buf, sizeof(buf), "Dev Error: SIGTERM %s failed: %s\r\n", m->name, strerror(errno));
                    m->pending_restart = 0;
                    dev_send_cli_response(ctx, msg, buf);
                    return ERRCODE_FAIL;
                }
                /* SIGCHLD 处理路径会 respawn；worker 等新进程 READY 后回响应 */
                if (process_wait_enqueue(OP_REBOOT, m, msg, epoch_before, line_id) != ERRCODE_SUCCESS)
                {
                    snprintf(buf, sizeof(buf), "Dev: reboot %s requested (pid=%d), wait failed.\r\n", m->name,
                             m->child_pid);
                    dev_send_cli_response(ctx, msg, buf);
                }
                return ERRCODE_SUCCESS;
            }
            /* 未运行：直接 spawn */
            if (dev_module_respawn(m) != ERRCODE_SUCCESS)
            {
                snprintf(buf, sizeof(buf), "Dev Error: failed to spawn %s\r\n", m->name);
                dev_send_cli_response(ctx, msg, buf);
                return ERRCODE_FAIL;
            }
            dev_ipc_connect(ctx, m->module_id, DEV_IPC_HOST_LOCAL, m->port);
            if (process_wait_enqueue(OP_REBOOT, m, msg, epoch_before, line_id) != ERRCODE_SUCCESS)
            {
                snprintf(buf, sizeof(buf), "Dev: reboot %s spawned (pid=%d), wait failed.\r\n", m->name, m->child_pid);
                dev_send_cli_response(ctx, msg, buf);
            }
            return ERRCODE_SUCCESS;
        }

        default:
            dev_send_cli_response(ctx, msg, "Dev Error: invalid op\r\n");
            return ERRCODE_FAIL;
    }
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
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    log_level_t selected = is_no ? dev_db_default_log_level() : LOG_LEVEL_DEBUG;
    int found = is_no ? 1 : 0;

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

static int handle_set_syslog_remote(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char server[SYSLOG_REPORT_SERVER_MAX] = {0};
    uint16_t port = 514u;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 1 && entry.value && entry.length > 0)
        {
            size_t n = entry.length < sizeof(server) - 1u ? entry.length : sizeof(server) - 1u;
            memcpy(server, entry.value, n);
            server[n] = '\0';
        }
        else if (entry.cfg_id == 2 && entry.value && entry.length == 8)
        {
            uint32_t hi = 0;
            uint32_t lo = 0;
            memcpy(&hi, entry.value, 4);
            memcpy(&lo, entry.value + 4, 4);
            uint64_t v = ((uint64_t)ntohl(hi) << 32) | ntohl(lo);
            if (v > 0 && v <= 65535)
            {
                port = (uint16_t)v;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        syslog_report_disable_remote();
        syslog_report_remote_config_t cfg = {0};
        dev_broadcast_syslog_remote(&cfg);
        if (dev_db_set_syslog_remote("", 0) != 0)
        {
            dev_send_cli_response(ctx, msg, "Dev: syslog remote disabled but failed to persist to DB.\r\n");
            return ERRCODE_FAIL;
        }
        dev_send_cli_response(ctx, msg, "Dev: syslog remote disabled.\r\n");
        return ERRCODE_SUCCESS;
    }

    if (server[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Dev Error: syslog server required.\r\n");
        return ERRCODE_FAIL;
    }

    syslog_report_set_remote(server, port);
    syslog_report_remote_config_t cfg;
    syslog_report_get_remote(&cfg);
    dev_broadcast_syslog_remote(&cfg);

    if (dev_db_set_syslog_remote(server, port) != 0)
    {
        dev_send_cli_response(ctx, msg, "Dev: syslog remote applied but failed to persist to DB.\r\n");
        return ERRCODE_FAIL;
    }

    char resp[256];
    snprintf(resp, sizeof(resp), "Dev: syslog remote set to %s:%u.\r\n", server, (unsigned)port);
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

static int parse_ipv4_prefix_token(const char *token, net_addr_t *addr, uint8_t *prefix_len)
{
    if (!token || !addr || !prefix_len)
    {
        return ERRCODE_FAIL;
    }

    char tmp[80];
    g_strlcpy(tmp, token, sizeof(tmp));
    char *slash = strchr(tmp, '/');
    if (!slash || slash == tmp || slash[1] == '\0')
    {
        return ERRCODE_FAIL;
    }
    *slash = '\0';

    char *end = NULL;
    unsigned long len = strtoul(slash + 1, &end, 10);
    if (!end || *end != '\0' || len > 32)
    {
        return ERRCODE_FAIL;
    }
    if (net_addr_from_str(tmp, addr) != 0 || addr->family != AF_INET)
    {
        return ERRCODE_FAIL;
    }
    *prefix_len = (uint8_t)len;
    return ERRCODE_SUCCESS;
}

static int dev_tunnel_resolve_query(dev_ipc_context_t *ctx, const tunnel_resolve_req_t *req,
                                    tunnel_resolve_notify_t *notify_out, uint32_t timeout_ms)
{
    if (!ctx || !req || !notify_out)
    {
        return ERRCODE_FAIL;
    }

    tunnel_resolve_req_t *payload = g_memdup2(req, sizeof(*req));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *query = dev_ipc_message_create(TUNNEL_MSG_TYPE_RESOLVE_QUERY, DEV_MODULE_ID_DEV,
                                                      DEV_MODULE_ID_TUNNEL, 0, payload, sizeof(*payload), g_free);
    if (!query)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_TUNNEL, query, timeout_ms);
    dev_ipc_message_free(query);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    if (resp->msg_type == TUNNEL_MSG_TYPE_RESOLVE_NOTIFY && resp->payload &&
        resp->payload_len >= sizeof(tunnel_resolve_notify_t))
    {
        memcpy(notify_out, resp->payload, sizeof(*notify_out));
        rc = ERRCODE_SUCCESS;
    }
    dev_ipc_message_free(resp);
    return rc;
}

static int handle_ping(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char ip[64] = {0};
    char src_ip[64] = {0};
    char vrf_name[VRF_NAME_MAX_LEN] = {0};
    gboolean ping_ipv6 = FALSE;
    gboolean ping_mpls = FALSE;
    uint32_t line_id = UINT32_MAX;

    /* 解析参数：
     * cfg_id=1: ping <ipv4-address>
     * cfg_id=2: ping ipv6 关键字
     * cfg_id=3: ping ipv6 <ipv6-address>
     * cfg_id=4: -a <src-ipv4>
     * cfg_id=5: -a <src-ipv6>
     * cfg_id=6: ping mpls 关键字
     * cfg_id=8: ping mpls ipv4 <ipv4-prefix>
     * cfg_id=9: vrf <vrf-name>
     */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ACCESS_LINE)
            {
                line_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 2)
        {
            ping_ipv6 = TRUE;
        }
        else if (entry.cfg_id == 6)
        {
            ping_mpls = TRUE;
        }
        else if (entry.cfg_id == 1 || entry.cfg_id == 3 || entry.cfg_id == 8)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(ip, text, sizeof(ip));
            }
        }
        else if (entry.cfg_id == 4 || entry.cfg_id == 5)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(src_ip, text, sizeof(src_ip));
            }
        }
        else if (entry.cfg_id == 9)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strlcpy(vrf_name, text, sizeof(vrf_name));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (ip[0] == '\0')
    {
        dev_send_cli_response(ctx, msg, "Error: missing IP address\r\n");
        return ERRCODE_FAIL;
    }

    if (ping_mpls)
    {
        if (vrf_name[0] != '\0' && strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) != 0)
        {
            dev_send_cli_response(ctx, msg, "Error: MPLS ping vrf is not supported\r\n");
            return ERRCODE_FAIL;
        }
        net_addr_t target;
        uint8_t prefix_len = 0;
        if (parse_ipv4_prefix_token(ip, &target, &prefix_len) != ERRCODE_SUCCESS)
        {
            dev_send_cli_response(ctx, msg, "Error: invalid MPLS IPv4 prefix; use A.B.C.D/len\r\n");
            return ERRCODE_FAIL;
        }
        if (prefix_len != 32u)
        {
            dev_send_cli_response(ctx, msg, "Error: MPLS ping currently requires an IPv4 /32 FEC\r\n");
            return ERRCODE_FAIL;
        }

        net_addr_t src_addr;
        bool has_src = false;
        if (src_ip[0] != '\0')
        {
            if (net_addr_from_str(src_ip, &src_addr) != 0 || src_addr.family != AF_INET)
            {
                dev_send_cli_response(ctx, msg, "Error: invalid MPLS ping source address\r\n");
                return ERRCODE_FAIL;
            }
            has_src = true;
        }

        tunnel_resolve_req_t req;
        memset(&req, 0, sizeof(req));
        req.vrf_id = 0;
        req.afi = 1;
        req.endpoint = target;

        tunnel_resolve_notify_t notify;
        memset(&notify, 0, sizeof(notify));
        if (dev_tunnel_resolve_query(ctx, &req, &notify, 3000) != ERRCODE_SUCCESS || !notify.resolved ||
            notify.label_count == 0)
        {
            dev_send_cli_response(ctx, msg, "Error: no resolved MPLS tunnel for target FEC\r\n");
            return ERRCODE_FAIL;
        }

        char errbuf[160] = {0};
        dev_ping_session_t *session =
            dev_ping_mpls_start(&target, has_src ? &src_addr : NULL, &notify, 4, 2000, errbuf, sizeof(errbuf));
        if (!session)
        {
            char out[256];
            snprintf(out, sizeof(out), "Error: failed to start MPLS ping: %s\r\n", errbuf[0] ? errbuf : "unknown");
            dev_send_cli_response(ctx, msg, out);
            return ERRCODE_FAIL;
        }
        if (ping_stream_start(session, msg, line_id) != ERRCODE_SUCCESS)
        {
            dev_ping_close(session);
            dev_send_cli_response(ctx, msg, "Error: failed to start ping stream\r\n");
            return ERRCODE_FAIL;
        }
        return ERRCODE_SUCCESS;
    }

    /* 验证 IP 地址格式并规范化 */
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

    net_addr_t src_addr;
    bool has_src = false;
    if (src_ip[0] != '\0')
    {
        if (net_addr_from_str(src_ip, &src_addr) != 0 || src_addr.family != addr.family)
        {
            dev_send_cli_response(ctx, msg, "Error: invalid or mismatched source address\r\n");
            return ERRCODE_FAIL;
        }
        has_src = true;
    }

    char errbuf[128] = {0};
    const char *bind_ifname = NULL;
    if (vrf_name[0] != '\0' && strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) != 0)
    {
        if (if_nametoindex(vrf_name) == 0)
        {
            char out[160];
            snprintf(out, sizeof(out), "Error: VRF %s not found\r\n", vrf_name);
            dev_send_cli_response(ctx, msg, out);
            return ERRCODE_FAIL;
        }
        /*
         * ping vrf 必须绑定到 L3VRF 设备。即使显式指定 -a，源地址也属于该 VRF
         * 的地址域；不绑定 VRF 时，内核会在默认地址域 bind(source)，VRF loopback
         * 源地址会报 EADDRNOTAVAIL。local-cross 转发依赖的是该 VRF FIB 中已安装的
         * 泄漏路由，而不是绕过 VRF 绑定。
         */
        bind_ifname = vrf_name;
    }
    dev_ping_session_t *session =
        dev_ping_start_bound(&addr, has_src ? &src_addr : NULL, bind_ifname, 4, 2000, errbuf, sizeof(errbuf));
    if (!session)
    {
        char out[256];
        snprintf(out, sizeof(out), "Error: failed to start ping: %s\r\n", errbuf[0] ? errbuf : "unknown");
        dev_send_cli_response(ctx, msg, out);
        return ERRCODE_FAIL;
    }

    if (ping_stream_start(session, msg, line_id) != ERRCODE_SUCCESS)
    {
        dev_ping_close(session);
        dev_send_cli_response(ctx, msg, "Error: failed to start ping stream\r\n");
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
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
    return cli_chunk_stream_continue(&g_dev_local->show_stream, ctx, DEV_MODULE_ID_DEV, msg);
}

void dev_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_dev_local->show_stream);
    dev_fs_cli_cleanup_all();
}

void dev_cli_handle_line_closed(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(uint32_t))
    {
        return;
    }

    uint32_t line_id = UINT32_MAX;
    memcpy(&line_id, msg->payload, sizeof(line_id));
    dev_fs_cli_cleanup_line(line_id);
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
        case DEV_CLI_GROUP_ID_SYSLOG_REMOTE:
            result = handle_set_syslog_remote(ctx, msg, &parser);
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
        case DEV_CLI_GROUP_ID_PROCESS_CMD:
            result = handle_process_cmd(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_SHOW_SUBSCRIPTIONS:
            result = handle_show_subscriptions(ctx, msg);
            break;
        case DEV_CLI_GROUP_ID_LS:
            result = dev_fs_cli_handle_ls(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_CD:
            result = dev_fs_cli_handle_cd(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_MORE:
            result = dev_fs_cli_handle_more(ctx, msg, &parser);
            break;
        case DEV_CLI_GROUP_ID_PWD:
            result = dev_fs_cli_handle_pwd(ctx, msg, &parser);
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
