/**
 * @file   nn_if_main.c
 * @brief  接口模块主入口，模块注册和消息循环
 * @author jhb
 * @date   2026/01/22
 */
#include "nn_if_main.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "nn_cfg.h"
#include "nn_db.h"
#include "nn_dev.h"
#include "nn_errcode.h"
#include "nn_if_cli.h"
#include "nn_if_map.h"
#include "nn_path_utils.h"

#define IF_MAX_EPOLL_EVENTS 16

nn_if_local_t *g_nn_if_local = NULL;

/**
 * @brief 启动时将映射表中的接口写入数据库
 *
 * 遍历接口映射表，对于每个逻辑接口：
 * - 不存在则插入默认记录
 * - 已存在则保留现有配置（重启不丢失）
 */
static void nn_if_init_db(void)
{
    extern nn_if_map_t g_interface_map;

    for (int i = 0; i < g_interface_map.count; i++)
    {
        const char *logical_name = g_interface_map.entries[i].logical_name;

        // 检查数据库中是否已存在该接口
        char where[64];
        snprintf(where, sizeof(where), "name = '%s'", logical_name);
        gboolean exists = FALSE;
        int ret = nn_db_exists("if_db", "if_interface", where, &exists);

        if (ret == NN_ERRCODE_SUCCESS && !exists)
        {
            // 插入默认记录
            const char *field_names[] = {"name", "ip_address", "netmask", "shutdown"};
            nn_db_value_t values[] = {nn_db_value_text(logical_name), nn_db_value_text(""), nn_db_value_text(""),
                                      nn_db_value_int(0)};
            nn_db_insert("if_db", "if_interface", field_names, values, 4);
            nn_db_value_free(&values[0]);
            nn_db_value_free(&values[1]);
            nn_db_value_free(&values[2]);
            printf("[if] Inserted interface %s into database\n", logical_name);
        }
        else if (ret == NN_ERRCODE_SUCCESS && exists)
        {
            printf("[if] Interface %s already exists in database, preserving config\n", logical_name);
        }
    }
}

// Process all pending messages from queue
static void if_process_messages(nn_if_local_t *ctx)
{
    nn_dev_message_t *msg;

    // Clear eventfd
    uint64_t val;
    read(ctx->event_fd, &val, sizeof(val));

    // Process all pending messages
    while ((msg = nn_dev_mq_receive(ctx->event_fd, ctx->mq)) != NULL)
    {
        // Handle different message types
        switch (msg->msg_type)
        {
            case NN_CFG_MSG_TYPE_CLI:
                // CLI command from cfg module
                printf("[if] Received CLI command message (%zu bytes)\n", msg->data_len);
                nn_if_cli_handle_message(msg);
                break;

            case NN_CFG_MSG_TYPE_CLI_CONTINUE:
                // Continue batch response
                printf("[if] Received CLI continue request\n");
                nn_if_cli_handle_continue(msg);
                break;

            default:
                printf("[if] Received unknown message type: 0x%08X\n", msg->msg_type);
                break;
        }

        nn_dev_message_free(msg);
    }
}

// IF worker thread with epoll
static void *if_worker_thread(void *arg)
{
    (void)arg;
    struct epoll_event events[IF_MAX_EPOLL_EVENTS];

    printf("[if] Worker thread started (epoll_fd=%d, event_fd=%d)\n", g_nn_if_local->epoll_fd, g_nn_if_local->event_fd);

    while (g_nn_if_local->running && !nn_dev_shutdown_requested())
    {
        // Wait for events with 1 second timeout
        int nfds = epoll_wait(g_nn_if_local->epoll_fd, events, IF_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("[if] epoll_wait failed");
            break;
        }

        if (nfds == 0)
        {
            // Timeout - periodic tasks if needed
            continue;
        }

        // Process events
        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == g_nn_if_local->event_fd)
            {
                // Message queue has data
                if_process_messages(g_nn_if_local);
            }
        }
    }

    printf("[if] Worker thread exiting\n");
    return NULL;
}

static int nn_if_init_local()
{
    g_nn_if_local = g_malloc0(sizeof(nn_if_local_t));
    g_nn_if_local->epoll_fd = NN_DEV_INVALID_FD;
    g_nn_if_local->event_fd = NN_DEV_INVALID_FD;
    g_nn_if_local->worker_thread = 0;
    g_nn_if_local->running = 0;

    // Create message queue
    nn_dev_module_mq_t *mq = nn_dev_mq_create();
    if (mq == NULL)
    {
        fprintf(stderr, "[if] Failed to create message queue\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_if_local->mq = mq;

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0)
    {
        fprintf(stderr, "[if] Failed to create event fd\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_if_local->event_fd = event_fd;

    // Create epoll instance
    g_nn_if_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_nn_if_local->epoll_fd < 0)
    {
        perror("[if] Failed to create epoll");
        return NN_ERRCODE_FAIL;
    }

    // Add eventfd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_nn_if_local->event_fd;
    if (epoll_ctl(g_nn_if_local->epoll_fd, EPOLL_CTL_ADD, g_nn_if_local->event_fd, &ev) < 0)
    {
        perror("[if] Failed to add eventfd to epoll");
        return NN_ERRCODE_FAIL;
    }

    // Register with pub/sub system
    int ret = nn_dev_pubsub_register(NN_DEV_MODULE_ID_IF, g_nn_if_local->event_fd, g_nn_if_local->mq);
    if (ret != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[if] Failed to register with pub/sub system\n");
        return NN_ERRCODE_FAIL;
    }

    // Subscribe to events from CFG module
    nn_dev_pubsub_subscribe(NN_DEV_MODULE_ID_IF, NN_DEV_MODULE_ID_CFG, NN_DEV_EVENT_CFG);

    // 初始化接口映射
    // 优先级：NN_RESOURCES_DIR（生产/GNS3） > 源码目录（开发）
    char if_map_path[PATH_MAX];
    const char *resources_dir = getenv("NN_RESOURCES_DIR");
    if (resources_dir != NULL)
    {
        // 生产/GNS3 环境：使用 NN_RESOURCES_DIR 下的 gns3 配置
        snprintf(if_map_path, sizeof(if_map_path), "%s/if/nn_if_map.conf.gns3", resources_dir);
        printf("[if] Using GNS3 interface mapping: %s\n", if_map_path);
    }
    else
    {
        // 开发环境：使用源码目录下的 local 配置
        char exe_dir[PATH_MAX];
        if (nn_get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
        {
            fprintf(stderr, "[if] Failed to get exe directory\n");
            return NN_ERRCODE_FAIL;
        }
        snprintf(if_map_path, sizeof(if_map_path), "%s/../../src/if/resources/nn_if_map.conf.local", exe_dir);
        printf("[if] Using local interface mapping: %s\n", if_map_path);
    }
    if (nn_if_map_init(if_map_path) != NN_ERRCODE_SUCCESS)
    {
        return NN_ERRCODE_FAIL;
    }

    // 将接口信息写入数据库
    nn_if_init_db();

    g_nn_if_local->running = 1;

    if (pthread_create(&g_nn_if_local->worker_thread, NULL, if_worker_thread, NULL) != 0)
    {
        perror("[if] Failed to create worker thread");
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

static void nn_if_cleanup_local()
{
    if (g_nn_if_local == NULL)
    {
        return;
    }

    printf("[if] Shutting down if module...\n");

    g_nn_if_local->running = 0;

    // Unregister from pub/sub
    nn_dev_pubsub_unregister(NN_DEV_MODULE_ID_IF);

    // Wait for thread to exit
    if (g_nn_if_local->worker_thread != 0)
    {
        pthread_join(g_nn_if_local->worker_thread, NULL);
    }

    // Close epoll fd
    if (g_nn_if_local->epoll_fd >= 0)
    {
        close(g_nn_if_local->epoll_fd);
    }

    if (g_nn_if_local->event_fd >= 0)
    {
        close(g_nn_if_local->event_fd);
    }

    if (g_nn_if_local->mq)
    {
        nn_dev_mq_destroy(g_nn_if_local->mq);
    }

    g_free(g_nn_if_local);
    g_nn_if_local = NULL;

    printf("[if] if module cleanup complete\n");
}

static int32_t if_module_init()
{
    int ret = nn_if_init_local();
    if (ret != NN_ERRCODE_SUCCESS)
    {
        nn_if_cleanup_local();
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

static void if_module_cleanup(void)
{
    nn_if_cleanup_local();
}

static void __attribute__((constructor)) register_if_module(void)
{
    nn_dev_register_module(NN_DEV_MODULE_ID_IF, "if", if_module_init, if_module_cleanup);

    char if_xml_path[256];
    if (nn_resolve_xml_path("if", if_xml_path, sizeof(if_xml_path)) == 0)
    {
        nn_cfg_register_module_xml(NN_DEV_MODULE_ID_IF, if_xml_path);
    }
}
