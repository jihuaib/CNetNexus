#ifndef NN_DEV_H
#define NN_DEV_H

#include <glib.h>
#include <stdint.h>

// ============================================================================
// Module IDs
// ============================================================================
#define NN_DEV_MODULE_ID_CFG 0x00000001
#define NN_DEV_MODULE_ID_DEV 0x00000002
#define NN_DEV_MODULE_ID_BGP 0x00000003

// ============================================================================
// Event IDs (for unicast pub/sub)
// ============================================================================
#define NN_DEV_EVENT_CFG 0x00010001

// ============================================================================
// Multicast Group IDs
// ============================================================================
#define NN_DEV_GROUP_HOST_NAME 0x00010001

#define NN_DEV_INVALID_FD (-1)

#define NN_DEV_MODULE_NAME_MAX_LEN 12

// Module initialization callback type
// Returns 0 on success, non-zero on failure
typedef int32_t (*nn_module_init_fn)();

// Module cleanup callback type
typedef void (*nn_module_cleanup_fn)(void);

// Register a module with init/cleanup callbacks
void nn_dev_register_module(uint32_t id, const char *name, nn_module_init_fn init, nn_module_cleanup_fn cleanup);

// Request shutdown of all modules
void nn_request_shutdown(void);

// Check if shutdown was requested
int nn_shutdown_requested(void);

int nn_dev_get_module_name(uint32_t module_id, char *module_name);

// ============================================================================
// MQ System APIs
// ============================================================================

// Message structure
typedef struct nn_dev_message
{
    uint32_t msg_type;       // Message type
    uint32_t sender_id;      // Sender module ID
    uint32_t request_id;     // Request ID (for correlation)
    void *data;              // Message data
    size_t data_len;         // Data length
    void (*free_fn)(void *); // Data g_free function
} nn_dev_message_t;

// Module message queue structure
typedef struct nn_dev_module_mq
{
    GQueue *message_queue; // Message queue (thread-safe with mutex)
    GMutex queue_mutex;    // Queue mutex
} nn_dev_module_mq_t;

// Create a message
nn_dev_message_t *nn_dev_message_create(uint32_t msg_type, uint32_t sender_id, uint32_t request_id, void *data,
                                        size_t data_len, void (*free_fn)(void *));

// Free a message
void nn_dev_message_free(nn_dev_message_t *msg);

// Create module message queue
nn_dev_module_mq_t *nn_dev_mq_create();

void nn_dev_mq_destroy(nn_dev_module_mq_t *mq);

// Send message to module queue (thread-safe)
int nn_nn_mq_send(int event_fd, nn_dev_module_mq_t *mq, nn_dev_message_t *msg);

// Receive message from queue (non-blocking, thread-safe)
nn_dev_message_t *nn_nn_mq_receive(int event_fd, nn_dev_module_mq_t *mq);

// ============================================================================
// Pub/Sub System APIs
// ============================================================================

// Register/unregister module with eventfd and message queue
int nn_dev_pubsub_register(uint32_t module_id, int eventfd, nn_dev_module_mq_t *mq);
void nn_dev_pubsub_unregister(uint32_t module_id);

// Unicast channel: point-to-point subscription
int nn_dev_pubsub_subscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);
int nn_dev_pubsub_unsubscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);
int nn_dev_pubsub_publish(uint32_t publisher_id, uint32_t event_id, nn_dev_message_t *msg);
int nn_dev_pubsub_publish_to_module(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                    nn_dev_message_t *msg);

// Synchronous query: send a message and wait for a response
nn_dev_message_t *nn_dev_pubsub_query(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                      nn_dev_message_t *msg, uint32_t timeout_ms);

// Send a response directly to a module by ID
int nn_dev_pubsub_send_response(uint32_t target_module_id, nn_dev_message_t *msg);

// Multicast channel: group-based communication
int nn_dev_pubsub_create_group(uint32_t owner_id, uint32_t group_id);
int nn_dev_pubsub_destroy_group(uint32_t owner_id, uint32_t group_id);
int nn_dev_pubsub_join_group(uint32_t module_id, uint32_t group_id);
int nn_dev_pubsub_leave_group(uint32_t module_id, uint32_t group_id);
int nn_dev_pubsub_multicast(uint32_t group_id, nn_dev_message_t *msg);

// Utility functions
int nn_dev_pubsub_get_subscriber_count(uint32_t publisher_id, uint32_t event_id);
int nn_dev_pubsub_get_group_member_count(uint32_t group_id);
int nn_dev_pubsub_is_subscribed(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);
int nn_dev_pubsub_is_group_member(uint32_t module_id, uint32_t group_id);
int nn_dev_pubsub_group_exists(uint32_t group_id);

#endif // NN_DEV_H
