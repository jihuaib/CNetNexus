#include "nn_db_main.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nn_cfg.h"
#include "nn_db_cli.h"
#include "nn_dev.h"
#include "nn_errcode.h"
#include "nn_path_utils.h"

#define DB_MAX_EPOLL_EVENTS 10

// Global context instance
nn_db_local_t *g_nn_db_local = NULL;

// ============================================================================
// Connection Management
// ============================================================================

static nn_db_connection_t *create_connection(const char *db_path)
{
    nn_db_connection_t *conn = g_malloc0(sizeof(nn_db_connection_t));
    conn->db_path = g_strdup(db_path);
    conn->handle = NULL;
    g_mutex_init(&conn->db_mutex);

    return conn;
}

static void free_connection(nn_db_connection_t *conn)
{
    if (!conn)
    {
        return;
    }

    if (conn->handle)
    {
        sqlite3_close(conn->handle);
    }

    g_mutex_clear(&conn->db_mutex);
    g_free(conn->db_path);
    g_free(conn);
}

nn_db_connection_t *nn_db_get_connection(const char *db_name)
{
    if (!db_name || !g_nn_db_local)
    {
        return NULL;
    }

    return g_hash_table_lookup(g_nn_db_local->connections, db_name);
}

// ============================================================================
// Message Processing
// ============================================================================

static void db_process_messages(nn_db_local_t *ctx)
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
                printf("[db] Received CLI command message (%zu bytes)\n", msg->data_len);
                nn_db_cli_process_command(msg);
                break;

            default:
                fprintf(stderr, "[db] Received unknown message type: %d\n", msg->msg_type);
                break;
        }

        // Free message
        nn_dev_message_free(msg);
    }
}

// Worker thread
static void *db_worker_thread(void *arg)
{
    (void)arg;
    struct epoll_event events[DB_MAX_EPOLL_EVENTS];

    printf("[db] Worker thread started (epoll_fd=%d, event_fd=%d)\n", g_nn_db_local->epoll_fd, g_nn_db_local->event_fd);

    while (g_nn_db_local->running && !nn_dev_shutdown_requested())
    {
        // Wait for events with 1 second timeout
        int nfds = epoll_wait(g_nn_db_local->epoll_fd, events, DB_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("[db] epoll_wait failed");
            break;
        }

        if (nfds == 0)
        {
            // Timeout - no events
            continue;
        }

        // Process events
        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == g_nn_db_local->event_fd)
            {
                // Message queue has data
                db_process_messages(g_nn_db_local);
            }
        }
    }

    printf("[db] Worker thread exiting\n");
    return NULL;
}

static int nn_db_init_local()
{
    // Create context
    g_nn_db_local = g_malloc0(sizeof(nn_db_local_t));
    g_nn_db_local->connections =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_connection);

    // Initialize registry
    g_nn_db_local->registry = nn_db_registry_get_instance();

    // Create message queue
    nn_dev_module_mq_t *mq = nn_dev_mq_create();
    if (mq == NULL)
    {
        fprintf(stderr, "[db] Failed to create message queue\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_db_local->mq = mq;

    // Create eventfd for message queue
    g_nn_db_local->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_nn_db_local->event_fd < 0)
    {
        perror("[db] Failed to create eventfd");
        return NN_ERRCODE_FAIL;
    }

    // Create epoll instance
    g_nn_db_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_nn_db_local->epoll_fd < 0)
    {
        perror("[db] Failed to create epoll");
        return NN_ERRCODE_FAIL;
    }

    // Add eventfd to epoll
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = g_nn_db_local->event_fd;
    if (epoll_ctl(g_nn_db_local->epoll_fd, EPOLL_CTL_ADD, g_nn_db_local->event_fd, &ev) < 0)
    {
        perror("[db] Failed to add eventfd to epoll");
        return NN_ERRCODE_FAIL;
    }

    // Register with pub/sub system
    int ret = nn_dev_pubsub_register(NN_DEV_MODULE_ID_DB, g_nn_db_local->event_fd, g_nn_db_local->mq);
    if (ret != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[db] Failed to register with pub/sub system\n");
        return NN_ERRCODE_FAIL;
    }

    // Subscribe to events from CFG module
    nn_dev_pubsub_subscribe(NN_DEV_MODULE_ID_DB, NN_DEV_MODULE_ID_CFG, NN_DEV_EVENT_CFG);

    g_nn_db_local->running = 1;

    // Start worker thread
    if (pthread_create(&g_nn_db_local->worker_thread, NULL, db_worker_thread, NULL) != 0)
    {
        perror("[db] Failed to create worker thread");
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

static void nn_db_cleanup_local()
{
    if (!g_nn_db_local)
    {
        return;
    }

    printf("[db] Cleaning up database module local state\n");

    // Signal worker thread to stop
    g_nn_db_local->running = 0;

    // Wait for worker thread to exit
    if (g_nn_db_local->worker_thread != 0)
    {
        pthread_join(g_nn_db_local->worker_thread, NULL);
    }

    // Unregister from pub/sub
    nn_dev_pubsub_unregister(NN_DEV_MODULE_ID_DB);

    // Close file descriptors
    if (g_nn_db_local->epoll_fd >= 0)
    {
        close(g_nn_db_local->epoll_fd);
    }

    if (g_nn_db_local->event_fd >= 0)
    {
        close(g_nn_db_local->event_fd);
    }

    // Destroy message queue
    if (g_nn_db_local->mq)
    {
        nn_dev_mq_destroy(g_nn_db_local->mq);
    }

    // Close all database connections
    if (g_nn_db_local->connections)
    {
        g_hash_table_destroy(g_nn_db_local->connections);
    }

    // Destroy registry
    nn_db_registry_destroy();

    g_free(g_nn_db_local);
    g_nn_db_local = NULL;
}

// ============================================================================
// Module Lifecycle Functions
// ============================================================================

int32_t db_module_init()
{
    int ret = nn_db_init_local();
    if (ret != NN_ERRCODE_SUCCESS)
    {
        nn_db_cleanup_local();
        return NN_ERRCODE_FAIL;
    }

    printf("[db] Database module initialized (epoll_fd=%d, event_fd=%d)\n", g_nn_db_local->epoll_fd,
           g_nn_db_local->event_fd);
    return NN_ERRCODE_SUCCESS;
}

void db_module_cleanup(void)
{
    nn_db_cleanup_local();
    printf("[db] Database module cleaned up\n");
}

// ============================================================================
// Module Registration (Constructor)
// ============================================================================

static void __attribute__((constructor)) register_db_module(void)
{
    // Register module with init/cleanup callbacks
    nn_dev_register_module(NN_DEV_MODULE_ID_DB, "db", db_module_init, db_module_cleanup);

    char cfg_xml_path[256];
    if (nn_resolve_xml_path("db", cfg_xml_path, sizeof(cfg_xml_path)) == 0)
    {
        nn_cfg_register_module_xml(NN_DEV_MODULE_ID_DB, cfg_xml_path);
    }
    else
    {
        fprintf(stderr, "[db] Warning: Could not resolve XML path for cfg module\n");
    }
}
