#ifndef FIB_SHOW_H
#define FIB_SHOW_H

#include "dev.h"

#define FIB_CLI_GROUP_ID_SHOW 1

int fib_show_dispatch(dev_ipc_message_t *msg);
void fib_show_cleanup(void);

#endif /* FIB_SHOW_H */
