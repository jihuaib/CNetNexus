//
// Created by jhb on 1/25/26.
// Module inter-communication pub/sub system implementation
//

#include "nn_dev_pubsub.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "nn_errcode.h"

// ============================================================================
// Global Data Structures
// ============================================================================

// Registered modules: module_id -> nn_dev_pubsub_subscriber_t*
static GHashTable *g_registered_modules = NULL;

// Unicast subscriptions: uint64_t key (publisher_id << 32 | event_id) -> GList of nn_dev_pubsub_subscriber_t*
static GHashTable *g_unicast_subs = NULL;

// Multicast groups: group_id -> nn_dev_pubsub_group_t*
static GHashTable *g_multicast_groups = NULL;

// Global mutex for thread-safe access
static GMutex g_pubsub_mutex;

// Initialization flag
static int g_pubsub_initialized = 0;

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Generate unicast subscription key from publisher_id and event_id
static inline uint64_t make_unicast_key(uint32_t publisher_id, uint32_t event_id)
{
    return ((uint64_t)publisher_id << 32) | event_id;
}

// Clone a subscriber info structure
static nn_dev_pubsub_subscriber_t *clone_subscriber(const nn_dev_pubsub_subscriber_t *src)
{
    nn_dev_pubsub_subscriber_t *dst = g_malloc0(sizeof(nn_dev_pubsub_subscriber_t));
    dst->module_id = src->module_id;
    dst->eventfd = src->eventfd;
    dst->mq = src->mq;
    return dst;
}

// Find subscriber in a list by module_id
static GList *find_subscriber_in_list(GList *list, uint32_t module_id)
{
    for (GList *iter = list; iter != NULL; iter = iter->next)
    {
        nn_dev_pubsub_subscriber_t *sub = (nn_dev_pubsub_subscriber_t *)iter->data;
        if (sub->module_id == module_id)
        {
            return iter;
        }
    }
    return NULL;
}

// Send message to a subscriber (copy message data)
static int send_to_subscriber(nn_dev_pubsub_subscriber_t *sub, nn_dev_message_t *msg)
{
    if (!sub || !sub->mq || !msg)
    {
        return NN_ERRCODE_FAIL;
    }

    // Create a copy of the message for this subscriber
    void *data_copy = NULL;
    if (msg->data && msg->data_len > 0)
    {
        data_copy = g_memdup2(msg->data, msg->data_len);
    }

    nn_dev_message_t *msg_copy =
        nn_dev_message_create(msg->msg_type, msg->sender_id, msg->request_id, data_copy, msg->data_len, g_free);

    return nn_nn_mq_send(sub->eventfd, sub->mq, msg_copy);
}

// Free multicast group
static void free_multicast_group(gpointer data)
{
    nn_dev_pubsub_group_t *group = (nn_dev_pubsub_group_t *)data;
    if (!group)
    {
        return;
    }

    g_mutex_lock(&group->group_mutex);

    // Free all members
    g_list_free_full(group->members, g_free);
    group->members = NULL;

    g_mutex_unlock(&group->group_mutex);
    g_mutex_clear(&group->group_mutex);

    g_free(group);
}

// Free subscriber list
static void free_subscriber_list(gpointer data)
{
    GList *list = (GList *)data;
    g_list_free_full(list, g_free);
}

// ============================================================================
// Initialization / Cleanup
// ============================================================================

int nn_dev_pubsub_init(void)
{
    if (g_pubsub_initialized)
    {
        return NN_ERRCODE_SUCCESS;
    }

    g_mutex_init(&g_pubsub_mutex);

    g_mutex_lock(&g_pubsub_mutex);

    // Create hash tables
    g_registered_modules = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    g_unicast_subs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, free_subscriber_list);
    g_multicast_groups = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free_multicast_group);

    g_pubsub_initialized = 1;

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Pub/sub system initialized\n");

    return NN_ERRCODE_SUCCESS;
}

void nn_dev_pubsub_cleanup(void)
{
    if (!g_pubsub_initialized)
    {
        return;
    }

    g_mutex_lock(&g_pubsub_mutex);

    // Destroy hash tables
    if (g_registered_modules)
    {
        g_hash_table_destroy(g_registered_modules);
        g_registered_modules = NULL;
    }

    if (g_unicast_subs)
    {
        g_hash_table_destroy(g_unicast_subs);
        g_unicast_subs = NULL;
    }

    if (g_multicast_groups)
    {
        g_hash_table_destroy(g_multicast_groups);
        g_multicast_groups = NULL;
    }

    g_pubsub_initialized = 0;

    g_mutex_unlock(&g_pubsub_mutex);
    g_mutex_clear(&g_pubsub_mutex);

    printf("[pubsub] Pub/sub system cleaned up\n");
}

// ============================================================================
// Module Registration
// ============================================================================

int nn_dev_pubsub_register(uint32_t module_id, int eventfd, nn_dev_module_mq_t *mq)
{
    if (!g_pubsub_initialized)
    {
        return NN_ERRCODE_FAIL;
    }

    if (eventfd < 0 || !mq)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    // Check if already registered
    if (g_hash_table_contains(g_registered_modules, GUINT_TO_POINTER(module_id)))
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Module 0x%08X already registered\n", module_id);
        return NN_ERRCODE_FAIL;
    }

    // Create subscriber info
    nn_dev_pubsub_subscriber_t *sub = g_malloc0(sizeof(nn_dev_pubsub_subscriber_t));
    sub->module_id = module_id;
    sub->eventfd = eventfd;
    sub->mq = mq;

    g_hash_table_insert(g_registered_modules, GUINT_TO_POINTER(module_id), sub);

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Module 0x%08X registered\n", module_id);

    return NN_ERRCODE_SUCCESS;
}

void nn_dev_pubsub_unregister(uint32_t module_id)
{
    if (!g_pubsub_initialized)
    {
        return;
    }

    g_mutex_lock(&g_pubsub_mutex);

    // Remove from registered modules
    g_hash_table_remove(g_registered_modules, GUINT_TO_POINTER(module_id));

    // Remove from all unicast subscriptions
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, g_unicast_subs);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        GList *sub_list = (GList *)value;
        GList *found = find_subscriber_in_list(sub_list, module_id);
        if (found)
        {
            g_free(found->data);
            sub_list = g_list_delete_link(sub_list, found);
            g_hash_table_iter_replace(&iter, sub_list);
        }
    }

    // Remove from all multicast groups (but don't destroy owned groups here)
    g_hash_table_iter_init(&iter, g_multicast_groups);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_dev_pubsub_group_t *group = (nn_dev_pubsub_group_t *)value;

        g_mutex_lock(&group->group_mutex);
        GList *found = find_subscriber_in_list(group->members, module_id);
        if (found)
        {
            g_free(found->data);
            group->members = g_list_delete_link(group->members, found);
        }
        g_mutex_unlock(&group->group_mutex);
    }

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Module 0x%08X unregistered\n", module_id);
}

// ============================================================================
// Unicast Channel
// ============================================================================

int nn_dev_pubsub_subscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id)
{
    if (!g_pubsub_initialized)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    // Get subscriber info from registered modules
    nn_dev_pubsub_subscriber_t *registered = g_hash_table_lookup(g_registered_modules, GUINT_TO_POINTER(subscriber_id));
    if (!registered)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Module 0x%08X not registered, cannot subscribe\n", subscriber_id);
        return NN_ERRCODE_FAIL;
    }

    // Create subscription key
    uint64_t key_val = make_unicast_key(publisher_id, event_id);

    // Get existing subscriber list
    GList *sub_list = g_hash_table_lookup(g_unicast_subs, &key_val);

    // Check if already subscribed
    if (find_subscriber_in_list(sub_list, subscriber_id))
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Module 0x%08X already subscribed to 0x%08X:0x%08X\n", subscriber_id, publisher_id, event_id);
        return NN_ERRCODE_SUCCESS;
    }

    // Clone subscriber and add to list
    nn_dev_pubsub_subscriber_t *sub_copy = clone_subscriber(registered);

    if (sub_list == NULL)
    {
        // New subscription - need to insert into hash table
        sub_list = g_list_append(NULL, sub_copy);
        uint64_t *key = g_malloc0(sizeof(uint64_t));
        *key = key_val;
        g_hash_table_insert(g_unicast_subs, key, sub_list);
    }
    else
    {
        // Existing subscription list - just append to it
        // The list is already in the hash table, no need to update the hash table
        // Note: g_list_append on a non-empty list returns the same head pointer,
        // but we capture it anyway to satisfy the compiler warning
        sub_list = g_list_append(sub_list, sub_copy);
    }

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Module 0x%08X subscribed to 0x%08X:0x%08X\n", subscriber_id, publisher_id, event_id);

    return NN_ERRCODE_SUCCESS;
}

int nn_dev_pubsub_unsubscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id)
{
    if (!g_pubsub_initialized)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    uint64_t key_val = make_unicast_key(publisher_id, event_id);

    GList *sub_list = g_hash_table_lookup(g_unicast_subs, &key_val);
    if (!sub_list)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        return NN_ERRCODE_SUCCESS;
    }

    GList *found = find_subscriber_in_list(sub_list, subscriber_id);
    if (found)
    {
        g_free(found->data);
        sub_list = g_list_delete_link(sub_list, found);

        if (sub_list)
        {
            // Need to create a new key for reinsertion
            uint64_t *new_key = g_malloc0(sizeof(uint64_t));
            *new_key = key_val;
            g_hash_table_insert(g_unicast_subs, new_key, sub_list);
        }
        else
        {
            g_hash_table_remove(g_unicast_subs, &key_val);
        }
    }

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Module 0x%08X unsubscribed from 0x%08X:0x%08X\n", subscriber_id, publisher_id, event_id);

    return NN_ERRCODE_SUCCESS;
}

int nn_dev_pubsub_publish(uint32_t publisher_id, uint32_t event_id, nn_dev_message_t *msg)
{
    if (!g_pubsub_initialized || !msg)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    uint64_t key_val = make_unicast_key(publisher_id, event_id);

    GList *sub_list = g_hash_table_lookup(g_unicast_subs, &key_val);

    if (!sub_list)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        return NN_ERRCODE_SUCCESS; // No subscribers, not an error
    }

    int success_count = 0;
    int fail_count = 0;

    // Send to all subscribers
    for (GList *iter = sub_list; iter != NULL; iter = iter->next)
    {
        nn_dev_pubsub_subscriber_t *sub = (nn_dev_pubsub_subscriber_t *)iter->data;
        if (send_to_subscriber(sub, msg) == NN_ERRCODE_SUCCESS)
        {
            success_count++;
        }
        else
        {
            fail_count++;
        }
    }

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Published to 0x%08X:0x%08X - sent: %d, failed: %d\n", publisher_id, event_id, success_count,
           fail_count);

    return (fail_count == 0) ? NN_ERRCODE_SUCCESS : NN_ERRCODE_FAIL;
}

/**
 * @brief Publish message to a specific target module (unicast to single module)
 * @param publisher_id Publisher module ID
 * @param event_id Event ID
 * @param target_module_id Target module ID to send to
 * @param msg Message to send
 * @return NN_ERRCODE_SUCCESS on success, NN_ERRCODE_FAIL on failure
 */
// Send message directly to a registered module by ID (no subscription required)
int nn_dev_pubsub_send_response(uint32_t target_module_id, nn_dev_message_t *msg)
{
    if (!g_pubsub_initialized || !msg)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    nn_dev_pubsub_subscriber_t *sub = g_hash_table_lookup(g_registered_modules, GUINT_TO_POINTER(target_module_id));
    if (!sub)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        return NN_ERRCODE_FAIL;
    }

    int ret = send_to_subscriber(sub, msg);

    g_mutex_unlock(&g_pubsub_mutex);
    return ret;
}

/**
 * Publish a unicast message to a specific target module
 * Only the specified target_module_id will receive the message (if subscribed)
 */
int nn_dev_pubsub_publish_to_module(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                    nn_dev_message_t *msg)
{
    if (!g_pubsub_initialized || !msg)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    uint64_t key_val = make_unicast_key(publisher_id, event_id);

    GList *sub_list = g_hash_table_lookup(g_unicast_subs, &key_val);

    if (!sub_list)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] No subscribers for 0x%08X:0x%08X\n", publisher_id, event_id);
        return NN_ERRCODE_FAIL;
    }

    // Find the specific target module in the subscriber list
    nn_dev_pubsub_subscriber_t *target_sub = NULL;
    for (GList *iter = sub_list; iter != NULL; iter = iter->next)
    {
        nn_dev_pubsub_subscriber_t *sub = (nn_dev_pubsub_subscriber_t *)iter->data;
        if (sub->module_id == target_module_id)
        {
            target_sub = sub;
            break;
        }
    }

    if (!target_sub)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Target module 0x%08X not subscribed to 0x%08X:0x%08X\n", target_module_id, publisher_id,
               event_id);
        return NN_ERRCODE_FAIL;
    }

    // Send to the specific target module
    int ret = send_to_subscriber(target_sub, msg);

    g_mutex_unlock(&g_pubsub_mutex);

    if (ret == NN_ERRCODE_SUCCESS)
    {
        printf("[pubsub] Published to module 0x%08X via 0x%08X:0x%08X\n", target_module_id, publisher_id, event_id);
    }

    return ret;
}

// Synchronous query: send a message and wait for a response
nn_dev_message_t *nn_dev_pubsub_query(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                      nn_dev_message_t *msg, uint32_t timeout_ms)
{
    if (!g_pubsub_initialized || !msg)
    {
        return NULL;
    }

    // Allocate a temporary module ID for the response
    // We use a high ID to avoid collisions (e.g., 0x80000000 + random/hash)
    static uint32_t g_query_id_counter = 0x80000000;
    uint32_t temp_module_id = g_atomic_int_add(&g_query_id_counter, 1);

    // Create temporary MQ and eventfd
    nn_dev_module_mq_t *temp_mq = nn_dev_mq_create();
    int temp_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (!temp_mq || temp_event_fd < 0)
    {
        if (temp_mq)
        {
            nn_nn_mq_destroy(temp_mq);
        }
        if (temp_event_fd >= 0)
        {
            close(temp_event_fd);
        }
        return NULL;
    }

    // Register temporary module
    if (nn_dev_pubsub_register(temp_module_id, temp_event_fd, temp_mq) != NN_ERRCODE_SUCCESS)
    {
        nn_nn_mq_destroy(temp_mq);
        close(temp_event_fd);
        return NULL;
    }

    // Set sender info in the message
    msg->sender_id = temp_module_id;
    if (msg->request_id == 0)
    {
        msg->request_id = temp_module_id; // Simple correlation
    }

    // Send the request
    if (nn_dev_pubsub_publish_to_module(publisher_id, event_id, target_module_id, msg) != NN_ERRCODE_SUCCESS)
    {
        nn_dev_pubsub_unregister(temp_module_id);
        nn_nn_mq_destroy(temp_mq);
        close(temp_event_fd);
        return NULL;
    }

    // Wait for response
    struct pollfd pfd;
    pfd.fd = temp_event_fd;
    pfd.events = POLLIN;

    nn_dev_message_t *response = NULL;
    int ret = poll(&pfd, 1, timeout_ms);

    if (ret > 0)
    {
        if (pfd.revents & POLLIN)
        {
            response = nn_nn_mq_receive(temp_event_fd, temp_mq);
        }
    }
    else if (ret == 0)
    {
        printf("[pubsub] Query to 0x%08X timed out after %u ms\n", target_module_id, timeout_ms);
    }
    else
    {
        perror("poll");
    }

    // Cleanup
    nn_dev_pubsub_unregister(temp_module_id);
    nn_nn_mq_destroy(temp_mq);
    close(temp_event_fd);

    return response;
}

// ============================================================================
// Multicast Channel
// ============================================================================

int nn_dev_pubsub_create_group(uint32_t owner_id, uint32_t group_id)
{
    if (!g_pubsub_initialized)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    // Check if group already exists
    if (g_hash_table_contains(g_multicast_groups, GUINT_TO_POINTER(group_id)))
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Group 0x%08X already exists\n", group_id);
        return NN_ERRCODE_FAIL;
    }

    // Create new group
    nn_dev_pubsub_group_t *group = g_malloc0(sizeof(nn_dev_pubsub_group_t));
    group->group_id = group_id;
    group->owner_id = owner_id;
    group->members = NULL;
    g_mutex_init(&group->group_mutex);

    g_hash_table_insert(g_multicast_groups, GUINT_TO_POINTER(group_id), group);

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Group 0x%08X created by module 0x%08X\n", group_id, owner_id);

    return NN_ERRCODE_SUCCESS;
}

int nn_dev_pubsub_destroy_group(uint32_t owner_id, uint32_t group_id)
{
    if (!g_pubsub_initialized)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    nn_dev_pubsub_group_t *group = g_hash_table_lookup(g_multicast_groups, GUINT_TO_POINTER(group_id));
    if (!group)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Group 0x%08X does not exist\n", group_id);
        return NN_ERRCODE_FAIL;
    }

    // Only owner can destroy group
    if (group->owner_id != owner_id)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Module 0x%08X is not owner of group 0x%08X\n", owner_id, group_id);
        return NN_ERRCODE_FAIL;
    }

    g_hash_table_remove(g_multicast_groups, GUINT_TO_POINTER(group_id));

    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Group 0x%08X destroyed by owner 0x%08X\n", group_id, owner_id);

    return NN_ERRCODE_SUCCESS;
}

int nn_dev_pubsub_join_group(uint32_t module_id, uint32_t group_id)
{
    if (!g_pubsub_initialized)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    // Get subscriber info from registered modules
    nn_dev_pubsub_subscriber_t *registered = g_hash_table_lookup(g_registered_modules, GUINT_TO_POINTER(module_id));
    if (!registered)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Module 0x%08X not registered, cannot join group\n", module_id);
        return NN_ERRCODE_FAIL;
    }

    // Group must already exist
    nn_dev_pubsub_group_t *group = g_hash_table_lookup(g_multicast_groups, GUINT_TO_POINTER(group_id));
    if (!group)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Group 0x%08X does not exist, cannot join\n", group_id);
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&group->group_mutex);

    // Check if already a member
    if (find_subscriber_in_list(group->members, module_id))
    {
        g_mutex_unlock(&group->group_mutex);
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Module 0x%08X already in group 0x%08X\n", module_id, group_id);
        return NN_ERRCODE_SUCCESS;
    }

    // Add to group
    nn_dev_pubsub_subscriber_t *sub_copy = clone_subscriber(registered);
    group->members = g_list_append(group->members, sub_copy);

    g_mutex_unlock(&group->group_mutex);
    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Module 0x%08X joined group 0x%08X\n", module_id, group_id);

    return NN_ERRCODE_SUCCESS;
}

int nn_dev_pubsub_leave_group(uint32_t module_id, uint32_t group_id)
{
    if (!g_pubsub_initialized)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    nn_dev_pubsub_group_t *group = g_hash_table_lookup(g_multicast_groups, GUINT_TO_POINTER(group_id));
    if (!group)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        return NN_ERRCODE_SUCCESS;
    }

    g_mutex_lock(&group->group_mutex);

    GList *found = find_subscriber_in_list(group->members, module_id);
    if (found)
    {
        g_free(found->data);
        group->members = g_list_delete_link(group->members, found);
    }

    g_mutex_unlock(&group->group_mutex);
    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Module 0x%08X left group 0x%08X\n", module_id, group_id);

    return NN_ERRCODE_SUCCESS;
}

int nn_dev_pubsub_multicast(uint32_t group_id, nn_dev_message_t *msg)
{
    if (!g_pubsub_initialized || !msg)
    {
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&g_pubsub_mutex);

    nn_dev_pubsub_group_t *group = g_hash_table_lookup(g_multicast_groups, GUINT_TO_POINTER(group_id));
    if (!group)
    {
        g_mutex_unlock(&g_pubsub_mutex);
        printf("[pubsub] Group 0x%08X does not exist\n", group_id);
        return NN_ERRCODE_FAIL;
    }

    g_mutex_lock(&group->group_mutex);

    int success_count = 0;
    int fail_count = 0;

    // Send to all group members
    for (GList *iter = group->members; iter != NULL; iter = iter->next)
    {
        nn_dev_pubsub_subscriber_t *sub = (nn_dev_pubsub_subscriber_t *)iter->data;
        if (send_to_subscriber(sub, msg) == NN_ERRCODE_SUCCESS)
        {
            success_count++;
        }
        else
        {
            fail_count++;
        }
    }

    g_mutex_unlock(&group->group_mutex);
    g_mutex_unlock(&g_pubsub_mutex);

    printf("[pubsub] Multicast to group 0x%08X - sent: %d, failed: %d\n", group_id, success_count, fail_count);

    return (fail_count == 0) ? NN_ERRCODE_SUCCESS : NN_ERRCODE_FAIL;
}

// ============================================================================
// Utility Functions
// ============================================================================

int nn_dev_pubsub_get_subscriber_count(uint32_t publisher_id, uint32_t event_id)
{
    if (!g_pubsub_initialized)
    {
        return 0;
    }

    g_mutex_lock(&g_pubsub_mutex);

    uint64_t key_val = make_unicast_key(publisher_id, event_id);
    GList *sub_list = g_hash_table_lookup(g_unicast_subs, &key_val);

    int count = sub_list ? g_list_length(sub_list) : 0;

    g_mutex_unlock(&g_pubsub_mutex);

    return count;
}

int nn_dev_pubsub_get_group_member_count(uint32_t group_id)
{
    if (!g_pubsub_initialized)
    {
        return 0;
    }

    g_mutex_lock(&g_pubsub_mutex);

    nn_dev_pubsub_group_t *group = g_hash_table_lookup(g_multicast_groups, GUINT_TO_POINTER(group_id));
    int count = 0;

    if (group)
    {
        g_mutex_lock(&group->group_mutex);
        count = g_list_length(group->members);
        g_mutex_unlock(&group->group_mutex);
    }

    g_mutex_unlock(&g_pubsub_mutex);

    return count;
}

int nn_dev_pubsub_is_subscribed(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id)
{
    if (!g_pubsub_initialized)
    {
        return 0;
    }

    g_mutex_lock(&g_pubsub_mutex);

    uint64_t key_val = make_unicast_key(publisher_id, event_id);
    GList *sub_list = g_hash_table_lookup(g_unicast_subs, &key_val);

    int result = find_subscriber_in_list(sub_list, subscriber_id) != NULL;

    g_mutex_unlock(&g_pubsub_mutex);

    return result;
}

int nn_dev_pubsub_is_group_member(uint32_t module_id, uint32_t group_id)
{
    if (!g_pubsub_initialized)
    {
        return 0;
    }

    g_mutex_lock(&g_pubsub_mutex);

    nn_dev_pubsub_group_t *group = g_hash_table_lookup(g_multicast_groups, GUINT_TO_POINTER(group_id));
    int result = 0;

    if (group)
    {
        g_mutex_lock(&group->group_mutex);
        result = find_subscriber_in_list(group->members, module_id) != NULL;
        g_mutex_unlock(&group->group_mutex);
    }

    g_mutex_unlock(&g_pubsub_mutex);

    return result;
}

int nn_dev_pubsub_group_exists(uint32_t group_id)
{
    if (!g_pubsub_initialized)
    {
        return 0;
    }

    g_mutex_lock(&g_pubsub_mutex);

    int exists = g_hash_table_contains(g_multicast_groups, GUINT_TO_POINTER(group_id));

    g_mutex_unlock(&g_pubsub_mutex);

    return exists;
}
