#include "injector.h"
#include "filter.h"
#include "mpegts.h"
#include "sender.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==================================================================
 *  Delay queue (ring buffer with dynamically allocated packet data)
 * ================================================================== */

static int dq_init(delay_queue_t *q, int cap)
{
    q->entries = calloc(cap, sizeof(delay_entry_t));
    if (!q->entries)
        return -1;
    q->capacity = cap;
    q->head = q->tail = q->count = 0;
    return 0;
}

static void dq_destroy(delay_queue_t *q)
{
    while (q->count > 0) {
        free(q->entries[q->head].data);
        q->head = (q->head + 1) % q->capacity;
        q->count--;
    }
    free(q->entries);
}

static int dq_enqueue(delay_queue_t *q, const packet_t *pkt, uint64_t rel_ms)
{
    if (q->count >= q->capacity)
        return -1;

    delay_entry_t *e = &q->entries[q->tail];
    e->data = malloc(pkt->length);
    if (!e->data)
        return -1;

    memcpy(e->data, pkt->data, pkt->length);
    e->length     = pkt->length;
    e->src_addr   = pkt->src_addr;
    e->release_ms = rel_ms;

    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    return 0;
}

static int dq_peek_ready(delay_queue_t *q, uint64_t now_ms)
{
    return q->count > 0 && q->entries[q->head].release_ms <= now_ms;
}

static int dq_dequeue(delay_queue_t *q, packet_t *pkt)
{
    if (q->count == 0)
        return -1;

    delay_entry_t *e = &q->entries[q->head];
    memcpy(pkt->data, e->data, e->length);
    pkt->length   = e->length;
    pkt->src_addr = e->src_addr;

    free(e->data);
    e->data = NULL;

    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return 0;
}

/* ==================================================================
 *  Injector context init / destroy
 * ================================================================== */

int injector_init(injector_ctx_t *ctx, app_config_t *cfg, packet_buffer_t *in)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg   = cfg;
    ctx->input = in;
    ctx->rng   = (unsigned int)time(NULL);
    ctx->delay_cycle_start = get_time_ms();
    ctx->drop_sec_start    = get_time_ms();
    return dq_init(&ctx->dq, MAX_DELAY_QUEUE);
}

void injector_destroy(injector_ctx_t *ctx)
{
    dq_destroy(&ctx->dq);
}

/* ==================================================================
 *  Per-packet injection decisions
 * ================================================================== */

static int check_delay(injector_ctx_t *ctx, uint64_t now)
{
    pthread_rwlock_rdlock(&ctx->cfg->config_lock);
    injection_config_t inj = ctx->cfg->injection;
    pthread_rwlock_unlock(&ctx->cfg->config_lock);

    if (!inj.delay_enabled)
        return 0;

    uint64_t elapsed = now - ctx->delay_cycle_start;
    uint64_t period  = (uint64_t)inj.delay_period * 1000;
    uint64_t burst   = (uint64_t)inj.delay_burst  * 1000;

    if (period > 0 && elapsed >= period) {
        ctx->delay_cycle_start = now;
        elapsed = 0;
    }

    return (elapsed < burst) ? inj.delay_seconds : 0;
}

static int check_drop(injector_ctx_t *ctx, uint64_t now)
{
    pthread_rwlock_rdlock(&ctx->cfg->config_lock);
    injection_config_t inj = ctx->cfg->injection;
    pthread_rwlock_unlock(&ctx->cfg->config_lock);

    if (!inj.drop_enabled)
        return 0;

    if (inj.drop_mode == 0) {
        /* count mode: drop up to N packets per second */
        if (now - ctx->drop_sec_start >= 1000) {
            ctx->drop_sec_start  = now;
            ctx->drops_this_sec  = 0;
        }
        if (ctx->drops_this_sec < inj.drop_count) {
            ctx->drops_this_sec++;
            return 1;
        }
        return 0;
    }

    /* percent mode */
    return ((int)(rand_r(&ctx->rng) % 100) < inj.drop_percent);
}

static int check_corrupt(injector_ctx_t *ctx)
{
    pthread_rwlock_rdlock(&ctx->cfg->config_lock);
    int en  = ctx->cfg->injection.corrupt_enabled;
    int pct = ctx->cfg->injection.corrupt_percent;
    pthread_rwlock_unlock(&ctx->cfg->config_lock);

    return en ? pct : 0;
}

static void do_corrupt(injector_ctx_t *ctx, packet_t *pkt, int pct)
{
    int n = (pkt->length * pct) / 100;
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
        int pos = rand_r(&ctx->rng) % pkt->length;
        pkt->data[pos] ^= (uint8_t)(rand_r(&ctx->rng) & 0xFF);
    }
}

static int do_pid_drop(injector_ctx_t *ctx, packet_t *pkt)
{
    pthread_rwlock_rdlock(&ctx->cfg->config_lock);
    int en  = ctx->cfg->injection.pid_drop_enabled;
    int cnt = ctx->cfg->injection.drop_pid_count;
    uint16_t pids[MAX_TS_PIDS];
    if (en && cnt > 0)
        memcpy(pids, ctx->cfg->injection.drop_pids, cnt * sizeof(uint16_t));
    pthread_rwlock_unlock(&ctx->cfg->config_lock);

    if (!en || cnt == 0)
        return 0;

    uint64_t per[MAX_TS_PIDS] = {0};
    int r = mpegts_nullify_pids(pkt->data, pkt->length, pids, cnt, per);
    for (int i = 0; i < cnt; i++)
        if (per[i])
            atomic_fetch_add(&ctx->cfg->stats.pid_drop_counts[i], per[i]);
    return r;
}

/* ==================================================================
 *  Processing thread — main injection pipeline
 * ================================================================== */

void *injector_thread(void *arg)
{
    injector_ctx_t *ctx = (injector_ctx_t *)arg;
    app_config_t   *cfg = ctx->cfg;
    packet_t pkt;

    while (cfg->running) {
        uint64_t now = get_time_ms();

        /* Release delayed packets whose time has come */
        while (dq_peek_ready(&ctx->dq, now)) {
            packet_t dp;
            if (dq_dequeue(&ctx->dq, &dp) == 0)
                sender_send(cfg, &dp);
        }

        /* Dequeue next packet (10 ms timeout keeps shutdown responsive) */
        if (packet_buffer_dequeue(ctx->input, &pkt, 10) != 0)
            continue;

        /* Filter check */
        pthread_rwlock_rdlock(&cfg->config_lock);
        filter_config_t filt = cfg->filter;
        pthread_rwlock_unlock(&cfg->config_lock);

        if (filter_matches(&filt, &pkt)) {
            /* 1. Drop */
            if (check_drop(ctx, now)) {
                atomic_fetch_add(&cfg->stats.pkts_dropped, 1);
                continue;
            }

            /* 2. Corrupt */
            int cpct = check_corrupt(ctx);
            if (cpct > 0) {
                do_corrupt(ctx, &pkt, cpct);
                atomic_fetch_add(&cfg->stats.pkts_corrupted, 1);
            }

            /* 3. PID nullify — only meaningful for MPEG-TS streams, skip for raw UDP bypass */
            if (cfg->input_type != STREAM_SRT_BYPASS) {
                int pn = do_pid_drop(ctx, &pkt);
                if (pn > 0)
                    atomic_fetch_add(&cfg->stats.pkts_pid_nullified, (uint64_t)pn);
            }

            /* 4. Delay */
            int ds = check_delay(ctx, now);
            if (ds > 0) {
                uint64_t rel = now + (uint64_t)ds * 1000;
                if (dq_enqueue(&ctx->dq, &pkt, rel) == 0) {
                    atomic_fetch_add(&cfg->stats.pkts_delayed, 1);
                    continue;   /* packet queued; don't forward now */
                }
                /* delay queue full → forward immediately */
            }
        }

        /* Forward packet */
        if (sender_send(cfg, &pkt) != 0)
            log_error("sender_send failed; packet lost");
    }

    /* Flush remaining delayed packets on shutdown */
    packet_t dp;
    while (ctx->dq.count > 0) {
        if (dq_dequeue(&ctx->dq, &dp) == 0)
            sender_send(cfg, &dp);
    }

    return NULL;
}
