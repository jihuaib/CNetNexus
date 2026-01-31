/**
 * @file   nn_dev_pubsub.h
 * @brief  模块间 Pub/Sub 通信系统头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_DEV_PUBSUB_H
#define NN_DEV_PUBSUB_H

#include <glib.h>
#include <stdint.h>

#include "nn_dev_module.h"
#include "nn_dev_mq.h"

// Subscriber info structure
typedef struct nn_dev_pubsub_subscriber
{
    uint32_t module_id;     // Subscriber module ID
    int eventfd;            // Subscriber's eventfd
    nn_dev_module_mq_t *mq; // Subscriber's message queue
} nn_dev_pubsub_subscriber_t;

// Unicast subscription entry
// Represents: subscriber subscribes to publisher's event_id
typedef struct nn_dev_pubsub_unicast_sub
{
    uint32_t publisher_id;                 // Publisher module ID
    uint32_t event_id;                     // Event ID to subscribe
    nn_dev_pubsub_subscriber_t subscriber; // Subscriber info
} nn_dev_pubsub_unicast_sub_t;

// Multicast group structure
typedef struct nn_dev_pubsub_group
{
    uint32_t group_id;  // Group ID
    uint32_t owner_id;  // Owner module ID (creator)
    GList *members;     // List of nn_dev_pubsub_subscriber_t*
    GMutex group_mutex; // Mutex for thread-safe access
} nn_dev_pubsub_group_t;

// ============================================================================
// Initialization / Cleanup
// ============================================================================

// Initialize pub/sub system
int nn_dev_pubsub_init(void);

// Cleanup pub/sub system
void nn_dev_pubsub_cleanup(void);

// ============================================================================
// Module Registration
// ============================================================================

// Register a module with its eventfd and message queue for receiving messages
// Must be called before subscribing to events
// Register a module with its eventfd and message queue for receiving messages
// Must be called before subscribing to events
int nn_dev_pubsub_register_inner(uint32_t module_id, int eventfd, nn_dev_module_mq_t *mq);

// Unregister a module from pub/sub system
void nn_dev_pubsub_unregister_inner(uint32_t module_id);

// ============================================================================
// Unicast Channel (Point-to-Point Subscription)
// ============================================================================

// Subscribe to a specific publisher's event ID
// Example: module A subscribes to module B's NN_EVENT_INTERFACE_UP event
// Subscribe to a specific publisher's event ID
// Example: module A subscribes to module B's NN_EVENT_INTERFACE_UP event
int nn_dev_pubsub_subscribe_inner(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);

// Unsubscribe from a specific publisher's event ID
int nn_dev_pubsub_unsubscribe_inner(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);

// Publish a unicast message to all subscribers of this event ID
// All modules that subscribed to publisher_id's event_id will receive the message
// Publish a unicast message to all subscribers of this event ID
// All modules that subscribed to publisher_id's event_id will receive the message
int nn_dev_pubsub_publish_inner(uint32_t publisher_id, uint32_t event_id, nn_dev_message_t *msg);

// Publish a unicast message to a specific target module
// Only the specified target_module_id will receive the message (if subscribed)
int nn_dev_pubsub_publish_to_module_inner(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                          nn_dev_message_t *msg);

// ============================================================================
// Multicast Channel (Group-based Communication)
// ============================================================================

// Create a multicast group (only owner can create)
// Returns NN_ERRCODE_SUCCESS if created, NN_ERRCODE_FAIL if already exists
int nn_dev_pubsub_create_group(uint32_t owner_id, uint32_t group_id);

// Destroy a multicast group (only owner can destroy)
int nn_dev_pubsub_destroy_group(uint32_t owner_id, uint32_t group_id);

// Join an existing multicast group
// Returns NN_ERRCODE_FAIL if group does not exist
int nn_dev_pubsub_join_group(uint32_t module_id, uint32_t group_id);

// Leave a multicast group
int nn_dev_pubsub_leave_group(uint32_t module_id, uint32_t group_id);

// Publish a message to a multicast group
// All members of the group will receive a copy of the message
int nn_dev_pubsub_multicast(uint32_t group_id, nn_dev_message_t *msg);

// Synchronous query: send a message and wait for a response
nn_dev_message_t *nn_dev_pubsub_query_inner(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                            nn_dev_message_t *msg, uint32_t timeout_ms);

// Send a response directly to a module by ID
int nn_dev_pubsub_send_response_inner(uint32_t target_module_id, nn_dev_message_t *msg);

// ============================================================================
// Utility Functions
// ============================================================================

// Get count of subscribers for a publisher's event ID
int nn_dev_pubsub_get_subscriber_count(uint32_t publisher_id, uint32_t event_id);

// Get count of members in a multicast group
int nn_dev_pubsub_get_group_member_count(uint32_t group_id);

// Check if a module is subscribed to a publisher's event ID
int nn_dev_pubsub_is_subscribed(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id);

// Check if a module is a member of a multicast group
int nn_dev_pubsub_is_group_member(uint32_t module_id, uint32_t group_id);

// Check if a multicast group exists
int nn_dev_pubsub_group_exists(uint32_t group_id);

void nn_dev_pubsub_foreach_subscriber(GHFunc func, gpointer user_data);

#endif // NN_DEV_PUBSUB_H
