#ifndef RECEIVER_H
#define RECEIVER_H

#include "config.h"
#include "packet_buffer.h"

typedef struct {
    app_config_t    *cfg;
    packet_buffer_t *buffer;
} receiver_ctx_t;

int   receiver_init(app_config_t *cfg);
void  receiver_cleanup(app_config_t *cfg);
void *receiver_thread(void *arg);

#endif /* RECEIVER_H */
