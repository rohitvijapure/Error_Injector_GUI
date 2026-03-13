#include "console.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#define STATS_LINES 12

static const char *type_str(stream_type_t t)
{
    switch (t) {
    case STREAM_UDP:       return "UDP";
    case STREAM_MULTICAST: return "Multicast";
    case STREAM_RTP:       return "RTP";
    case STREAM_SRT:       return "SRT";
    }
    return "?";
}

void console_setup(void)
{
    struct winsize ws = {0};
    int rows = 40;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        rows = ws.ws_row;

    printf("\033[2J");                                /* clear screen        */
    printf("\033[%d;%dr", STATS_LINES + 2, rows);    /* scroll region below */
    printf("\033[%d;1H", STATS_LINES + 2);            /* cursor in scroll    */
    fflush(stdout);
}

void console_restore(void)
{
    printf("\033[r");
    printf("\033[2J\033[H");
    fflush(stdout);
}

void *console_thread(void *arg)
{
    console_ctx_t *ctx = (console_ctx_t *)arg;
    app_config_t  *cfg = ctx->cfg;

    uint64_t prev_in = 0, prev_out = 0;
    uint64_t prev_t  = get_time_ms();
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (cfg->running) {
        usleep(500000);
        if (!cfg->running) break;

        uint64_t now_ms  = get_time_ms();
        uint64_t bin     = atomic_load(&cfg->stats.bytes_received);
        uint64_t bout    = atomic_load(&cfg->stats.bytes_forwarded);
        uint64_t dt      = now_ms - prev_t;
        double mbps_in   = dt ? (double)(bin  - prev_in)  * 8.0 / (dt * 1000.0) : 0;
        double mbps_out  = dt ? (double)(bout - prev_out) * 8.0 / (dt * 1000.0) : 0;
        prev_in  = bin;
        prev_out = bout;
        prev_t   = now_ms;

        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        int up = (int)(tn.tv_sec - t0.tv_sec);

        pthread_rwlock_rdlock(&cfg->config_lock);
        injection_config_t inj = cfg->injection;
        filter_config_t    flt = cfg->filter;
        pthread_rwlock_unlock(&cfg->config_lock);

        /* Save cursor, write stats to the fixed area at the top */
        printf("\033[s");

        int ln = 1;
#define PL(fmt, ...) printf("\033[%d;1H\033[2K" fmt, ln++, ##__VA_ARGS__)

        PL("\033[1m=== ERROR INJECTOR ===\033[0m  Uptime %02d:%02d:%02d",
           up / 3600, (up % 3600) / 60, up % 60);

        PL("Input:  %-10s %s:%d  iface=%s",
           type_str(cfg->input_type), cfg->input_addr, cfg->input_port,
           cfg->input_iface[0] ? cfg->input_iface : "*");

        PL("Output: %-10s %s:%d  iface=%s",
           type_str(cfg->output_type), cfg->output_addr, cfg->output_port,
           cfg->output_iface[0] ? cfg->output_iface : "*");

        PL("Bitrate In: %8.2f Mbps   Out: %8.2f Mbps", mbps_in, mbps_out);

        PL("Pkts    In: %-10lu       Out: %-10lu",
           (unsigned long)atomic_load(&cfg->stats.pkts_received),
           (unsigned long)atomic_load(&cfg->stats.pkts_forwarded));

        PL("Dropped: %-8lu  Corrupted: %-8lu  Delayed: %-8lu  PID-null: %-8lu",
           (unsigned long)atomic_load(&cfg->stats.pkts_dropped),
           (unsigned long)atomic_load(&cfg->stats.pkts_corrupted),
           (unsigned long)atomic_load(&cfg->stats.pkts_delayed),
           (unsigned long)atomic_load(&cfg->stats.pkts_pid_nullified));

        PL("");   /* blank separator */

        /* Active settings */
        char delay_s[64], drop_s[64], corr_s[32], pid_s[128];

        if (inj.delay_enabled)
            snprintf(delay_s, sizeof(delay_s), "%ds every %ds (burst %ds)",
                     inj.delay_seconds, inj.delay_period, inj.delay_burst);
        else
            snprintf(delay_s, sizeof(delay_s), "OFF");

        if (inj.drop_enabled) {
            if (inj.drop_mode == 0)
                snprintf(drop_s, sizeof(drop_s), "%d pkt/s", inj.drop_count);
            else
                snprintf(drop_s, sizeof(drop_s), "%d%%", inj.drop_percent);
        } else {
            snprintf(drop_s, sizeof(drop_s), "OFF");
        }

        if (inj.corrupt_enabled)
            snprintf(corr_s, sizeof(corr_s), "%d%%", inj.corrupt_percent);
        else
            snprintf(corr_s, sizeof(corr_s), "OFF");

        if (inj.pid_drop_enabled && inj.drop_pid_count > 0) {
            int pos = 0;
            for (int i = 0; i < inj.drop_pid_count && pos < (int)sizeof(pid_s) - 8; i++)
                pos += snprintf(pid_s + pos, sizeof(pid_s) - pos, "%s%u",
                                i ? "," : "", inj.drop_pids[i]);
        } else {
            snprintf(pid_s, sizeof(pid_s), "OFF");
        }

        PL("Delay: %-22s  Drop: %-15s", delay_s, drop_s);
        PL("Corrupt: %-20s  PID-drop: %s", corr_s, pid_s);

        if (flt.active) {
            char sip[INET_ADDRSTRLEN] = "*", dip[INET_ADDRSTRLEN] = "*";
            if (flt.src_ip) {
                struct in_addr a = { .s_addr = flt.src_ip };
                inet_ntop(AF_INET, &a, sip, sizeof(sip));
            }
            if (flt.dst_ip) {
                struct in_addr a = { .s_addr = flt.dst_ip };
                inet_ntop(AF_INET, &a, dip, sizeof(dip));
            }
            PL("Filter: src=%s:%u  dst=%s:%u",
               sip, flt.src_port, dip, flt.dst_port);
        } else {
            PL("Filter: NONE (all traffic)");
        }

        /* Separator line */
        PL("────────────────────────────────────────────────────────────");

        /* Pad remaining lines in the fixed region */
        while (ln <= STATS_LINES + 1)
            PL("");

#undef PL

        /* Restore cursor back to the scroll region */
        printf("\033[u");
        fflush(stdout);
    }

    return NULL;
}
