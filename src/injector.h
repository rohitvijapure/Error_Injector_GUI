#ifndef INJECTOR_H
#define INJECTOR_H

#include "config.h"
#include "packet_buffer.h"

typedef struct {
    uint8_t            *data;       /* dynamically allocated per entry */
    int                 length;
    struct sockaddr_in  src_addr;
    uint64_t            release_ms;
} delay_entry_t;

typedef struct {
    delay_entry_t *entries;
    int            capacity;
    int            head;
    int            tail;
    int            count;
} delay_queue_t;

typedef struct {
    app_config_t    *cfg;
    packet_buffer_t *input;
    delay_queue_t    dq;
    uint64_t         delay_cycle_start;
    uint64_t         drop_sec_start;
    int              drops_this_sec;
    unsigned int     rng;
} injector_ctx_t;

int   injector_init(injector_ctx_t *ctx, app_config_t *cfg, packet_buffer_t *in);
void  injector_destroy(injector_ctx_t *ctx);
void *injector_thread(void *arg);

#endif /* INJECTOR_H */
