#include "packet_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

int packet_buffer_init(packet_buffer_t *buf, int capacity)
{
    buf->packets = calloc(capacity, sizeof(packet_t));
    if (!buf->packets)
        return -1;
    buf->capacity = capacity;
    buf->head = buf->tail = buf->count = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
    return 0;
}

void packet_buffer_destroy(packet_buffer_t *buf)
{
    free(buf->packets);
    pthread_mutex_destroy(&buf->mutex);
    pthread_cond_destroy(&buf->not_empty);
}

int packet_buffer_enqueue(packet_buffer_t *buf, const packet_t *pkt)
{
    pthread_mutex_lock(&buf->mutex);

    if (buf->count >= buf->capacity) {
        buf->head = (buf->head + 1) % buf->capacity;
        buf->count--;
    }

    packet_t *dst = &buf->packets[buf->tail];
    memcpy(dst->data, pkt->data, pkt->length);
    dst->length    = pkt->length;
    dst->src_addr  = pkt->src_addr;
    dst->dst_addr  = pkt->dst_addr;
    dst->recv_time = pkt->recv_time;

    buf->tail = (buf->tail + 1) % buf->capacity;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

int packet_buffer_dequeue(packet_buffer_t *buf, packet_t *pkt, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)timeout_ms * 1000000L;
    while (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0) {
        if (pthread_cond_timedwait(&buf->not_empty, &buf->mutex, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&buf->mutex);
            return -1;
        }
    }

    packet_t *src = &buf->packets[buf->head];
    memcpy(pkt->data, src->data, src->length);
    pkt->length    = src->length;
    pkt->src_addr  = src->src_addr;
    pkt->dst_addr  = src->dst_addr;
    pkt->recv_time = src->recv_time;

    buf->head = (buf->head + 1) % buf->capacity;
    buf->count--;

    pthread_mutex_unlock(&buf->mutex);
    return 0;
}
