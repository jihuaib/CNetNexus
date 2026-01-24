#define NN_MODULE_NAME_MAX_LEN 12

// Message structure
typedef struct nn_message
{
    char *type;                    // Message type
    void *data;                    // Message data
    size_t data_len;               // Data length
    void (*free_fn)(void *);       // Data free function
} nn_dev_message_t;

// Module message queue structure
typedef struct nn_module_mq
{
    int eventfd;                   // Event notification fd
    GQueue *message_queue;         // Message queue (thread-safe with mutex)
    GMutex queue_mutex;            // Queue mutex
} nn_module_mq_t;


// Module descriptor structure
typedef struct nn_module
{
    uint32_t module_id;
    char name[NN_MODULE_NAME_MAX_LEN];            // Module name
    nn_module_init_fn init;        // Module initialization function
    nn_module_cleanup_fn cleanup;  // Module cleanup function
    struct nn_module *next;        // Linked list pointer (for backward compatibility)
    
    // Message queue for inter-module communication
    nn_module_mq_t *mq;            // Module message queue (NULL if not initialized)
} nn_dev_module_t;