#ifndef PACKET_BUFFER_H
#define PACKET_BUFFER_H

#include "config.h"

typedef struct {
    packet_t        *packets;
    int              capacity;
    int              head;
    int              tail;
    int              count;
    pthread_mutex_t  mutex;
    pthread_cond_t   not_empty;
} packet_buffer_t;

int  packet_buffer_init(packet_buffer_t *buf, int capacity);
void packet_buffer_destroy(packet_buffer_t *buf);
int  packet_buffer_enqueue(packet_buffer_t *buf, const packet_t *pkt);
int  packet_buffer_dequeue(packet_buffer_t *buf, packet_t *pkt, int timeout_ms);

#endif /* PACKET_BUFFER_H */
