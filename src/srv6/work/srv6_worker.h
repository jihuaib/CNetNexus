#ifndef SRV6_WORKER_H
#define SRV6_WORKER_H

#include <glib.h>
#include <pthread.h>

#include "cli.h"
#include "dev.h"
#include "srv6_db.h"

typedef struct srv6_binding_state
{
    srv6_sid_entry_t entry;
    gboolean fib_programmed;
    int last_error;
} srv6_binding_state_t;

typedef struct srv6_work_local
{
    GHashTable *locators; /**< name -> srv6_locator_t */
    GHashTable *bindings; /**< srv6_sid_key_t -> srv6_binding_state_t */
    GAsyncQueue *cmd_queue;
    pthread_t thread;
    volatile int running;
    gboolean thread_started;
    gboolean fib_ready;
    gboolean route_ready;
    gboolean vrf_ready;
    cli_chunk_stream_t show_stream;
} srv6_work_local_t;

extern srv6_work_local_t *g_srv6_work_local;

int srv6_worker_prepare(void);
int srv6_worker_launch(void);
void srv6_worker_shutdown(void);

int srv6_worker_post_rpc(dev_ipc_message_t *msg);
/** RPC 无法投递到 worker 时，按请求类型立即返回失败结果。 */
void srv6_worker_send_rpc_failure(const dev_ipc_message_t *msg);
int srv6_worker_post_show(dev_ipc_message_t *msg);
int srv6_worker_post_vrf_event(dev_ipc_message_t *msg);
int srv6_worker_post_vrf_down(void);
int srv6_worker_post_fib_ready(void);
int srv6_worker_post_fib_down(void);
int srv6_worker_post_route_ready(void);
int srv6_worker_post_route_down(void);

/** 冷恢复栅栏：先清理 ROUTE 中 SRV6 模块独占的 locator aggregate 路径。 */
int srv6_worker_prepare_restore(void);

int srv6_worker_locator_upsert(const srv6_locator_t *locator, char *error, size_t error_len);
int srv6_worker_locator_delete(const char *name, char *error, size_t error_len);
int srv6_worker_delete_config(char *error, size_t error_len);
int srv6_worker_restore(const GPtrArray *locators, const GPtrArray *bindings);

#endif /* SRV6_WORKER_H */
