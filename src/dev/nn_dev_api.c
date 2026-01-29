#include "nn_dev_module.h"
#include "nn_dev_mq.h"
#include "nn_dev_pubsub.h"

#include "nn_errcode.h"
#include <stdio.h>

void nn_dev_register_module(uint32_t id, const char *name, nn_module_init_fn init, nn_module_cleanup_fn cleanup)
{
    if (!name)
    {
        return;
    }

    nn_dev_register_module_inner(id, name, init, cleanup);

    printf("[dev] Registered module: %s\n", name);
}

int nn_dev_get_module_name(uint32_t module_id, char *module_name)
{
    if (!module_name)
    {
        return NN_ERRCODE_FAIL;
    }

    return nn_dev_get_module_name_inner(module_id, module_name);
}

// Request shutdown
void nn_dev_request_shutdown(void)
{
    nn_dev_request_shutdown_inner();
}

// Check if shutdown was requested
int nn_dev_shutdown_requested(void)
{
    return nn_dev_shutdown_requested_inner();
}

// ============================================================================
// MQ System APIs
// ============================================================================

nn_dev_message_t *nn_dev_message_create(uint32_t msg_type, uint32_t sender_id, uint32_t request_id, void *data,
                                        size_t data_len, void (*free_fn)(void *))
{
    return nn_dev_message_create_inner(msg_type, sender_id, request_id, data, data_len, free_fn);
}

void nn_dev_message_free(nn_dev_message_t *msg)
{
    nn_dev_message_free_inner(msg);
}

nn_dev_module_mq_t *nn_dev_mq_create()
{
    return nn_dev_mq_create_inner();
}

void nn_dev_mq_destroy(nn_dev_module_mq_t *mq)
{
    nn_dev_mq_destroy_inner(mq);
}

int nn_dev_mq_send(int event_fd, nn_dev_module_mq_t *mq, nn_dev_message_t *msg)
{
    return nn_nn_mq_send_inner(event_fd, mq, msg);
}

nn_dev_message_t *nn_dev_mq_receive(int event_fd, nn_dev_module_mq_t *mq)
{
    return nn_dev_mq_receive_inner(event_fd, mq);
}

// ============================================================================
// Pub/Sub System APIs
// ============================================================================

int nn_dev_pubsub_register(uint32_t module_id, int eventfd, nn_dev_module_mq_t *mq)
{
    if (mq == NULL || eventfd < 0)
    {
        return NN_ERRCODE_FAIL;
    }
    return nn_dev_pubsub_register_inner(module_id, eventfd, mq);
}

void nn_dev_pubsub_unregister(uint32_t module_id)
{
    nn_dev_pubsub_unregister_inner(module_id);
}

int nn_dev_pubsub_subscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id)
{
    return nn_dev_pubsub_subscribe_inner(subscriber_id, publisher_id, event_id);
}

int nn_dev_pubsub_unsubscribe(uint32_t subscriber_id, uint32_t publisher_id, uint32_t event_id)
{
    return nn_dev_pubsub_unsubscribe_inner(subscriber_id, publisher_id, event_id);
}

int nn_dev_pubsub_publish(uint32_t publisher_id, uint32_t event_id, nn_dev_message_t *msg)
{
    return nn_dev_pubsub_publish_inner(publisher_id, event_id, msg);
}

int nn_dev_pubsub_publish_to_module(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                    nn_dev_message_t *msg)
{
    return nn_dev_pubsub_publish_to_module_inner(publisher_id, event_id, target_module_id, msg);
}

nn_dev_message_t *nn_dev_pubsub_query(uint32_t publisher_id, uint32_t event_id, uint32_t target_module_id,
                                      nn_dev_message_t *msg, uint32_t timeout_ms)
{
    return nn_dev_pubsub_query_inner(publisher_id, event_id, target_module_id, msg, timeout_ms);
}

int nn_dev_pubsub_send_response(uint32_t target_module_id, nn_dev_message_t *msg)
{
    return nn_dev_pubsub_send_response_inner(target_module_id, msg);
}