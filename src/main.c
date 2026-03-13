#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <srt/srt.h>

#include "config.h"
#include "utils.h"
#include "packet_buffer.h"
#include "receiver.h"
#include "sender.h"
#include "injector.h"
#include "console.h"
#include "filter.h"

/* ==================================================================
 *  Global configuration (single instance)
 * ================================================================== */

static app_config_t g_cfg;

static void on_signal(int sig)
{
    (void)sig;
    g_cfg.running = 0;
}

/* ==================================================================
 *  CLI argument parsing
 * ================================================================== */

static stream_type_t parse_type(const char *s)
{
    if (!strcasecmp(s, "udp"))       return STREAM_UDP;
    if (!strcasecmp(s, "multicast")) return STREAM_MULTICAST;
    if (!strcasecmp(s, "rtp"))       return STREAM_RTP;
    if (!strcasecmp(s, "srt"))       return STREAM_SRT;
    fprintf(stderr, "Unknown stream type '%s', defaulting to UDP\n", s);
    return STREAM_UDP;
}

static void print_help(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS]\n\n"
        "Input:\n"
        "  --input-type <udp|multicast|rtp|srt>  Stream type (default: udp)\n"
        "  --input-addr <addr>                    Address (multicast group or SRT target)\n"
        "  --input-port <port>                    Port to bind/listen\n"
        "  --input-iface <iface>                  Network interface (e.g. eth0)\n"
        "  --srt-mode <listener|caller>           SRT mode (default: listener)\n"
        "  --srt-latency <ms>                     SRT latency (default: 120)\n\n"
        "Output:\n"
        "  --output-type <udp|multicast|srt>      Stream type (default: udp)\n"
        "  --output-addr <addr>                   Destination address\n"
        "  --output-port <port>                   Destination port\n"
        "  --output-iface <iface>                 Network interface\n"
        "  --output-ttl <ttl>                     Multicast TTL (default: 5)\n\n"
        "Filter (optional — when set, injection only affects matching packets):\n"
        "  --filter-src-ip <ip>                   Source IP\n"
        "  --filter-src-port <port>               Source port\n"
        "  --filter-dst-ip <ip>                   Destination IP\n"
        "  --filter-dst-port <port>               Destination port\n\n"
        "Error injection (initial values — adjustable live):\n"
        "  --delay <1-15>                         Delay duration in seconds\n"
        "  --delay-period <seconds>               Cycle length (default: 30)\n"
        "  --delay-burst <seconds>                Burst window (default: 10)\n"
        "  --drop-count <1-500>                   Packets to drop per second\n"
        "  --drop-percent <1-100>                 Percentage of packets to drop\n"
        "  --corrupt <1-95>                       Corruption %% (bytes per packet)\n"
        "  --drop-pid <pid>                       MPEG-TS PID to nullify (repeatable)\n\n"
        "Interactive commands available at runtime — type 'help' for list.\n",
        prog);
}

enum {
    O_ITYPE = 1000, O_IADDR, O_IPORT, O_IIFACE,
    O_SRTM, O_SRTL,
    O_OTYPE, O_OADDR, O_OPORT, O_OIFACE, O_OTTL,
    O_FSIP, O_FSPORT, O_FDIP, O_FDPORT,
    O_DELAY, O_DPERIOD, O_DBURST,
    O_DROPC, O_DROPP, O_CORRUPT, O_DROPPID
};

static struct option long_opts[] = {
    { "input-type",      1, 0, O_ITYPE  },
    { "input-addr",      1, 0, O_IADDR  },
    { "input-port",      1, 0, O_IPORT  },
    { "input-iface",     1, 0, O_IIFACE },
    { "srt-mode",        1, 0, O_SRTM   },
    { "srt-latency",     1, 0, O_SRTL   },
    { "output-type",     1, 0, O_OTYPE  },
    { "output-addr",     1, 0, O_OADDR  },
    { "output-port",     1, 0, O_OPORT  },
    { "output-iface",    1, 0, O_OIFACE },
    { "output-ttl",      1, 0, O_OTTL   },
    { "filter-src-ip",   1, 0, O_FSIP   },
    { "filter-src-port", 1, 0, O_FSPORT },
    { "filter-dst-ip",   1, 0, O_FDIP   },
    { "filter-dst-port", 1, 0, O_FDPORT },
    { "delay",           1, 0, O_DELAY  },
    { "delay-period",    1, 0, O_DPERIOD},
    { "delay-burst",     1, 0, O_DBURST },
    { "drop-count",      1, 0, O_DROPC  },
    { "drop-percent",    1, 0, O_DROPP  },
    { "corrupt",         1, 0, O_CORRUPT},
    { "drop-pid",        1, 0, O_DROPPID},
    { "help",            0, 0, 'h'      },
    { 0, 0, 0, 0 }
};

static int parse_args(int argc, char **argv)
{
    int c;
    while ((c = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (c) {
        case 'h':
            print_help(argv[0]);
            return -1;

        /* Input */
        case O_ITYPE:
            g_cfg.input_type = parse_type(optarg);
            break;
        case O_IADDR:
            strncpy(g_cfg.input_addr, optarg, sizeof(g_cfg.input_addr) - 1);
            break;
        case O_IPORT:
            g_cfg.input_port = atoi(optarg);
            break;
        case O_IIFACE:
            strncpy(g_cfg.input_iface, optarg, sizeof(g_cfg.input_iface) - 1);
            break;
        case O_SRTM:
            g_cfg.srt_mode = !strcasecmp(optarg, "caller")
                             ? SRT_MODE_CALLER : SRT_MODE_LISTENER;
            break;
        case O_SRTL:
            g_cfg.srt_latency = atoi(optarg);
            break;

        /* Output */
        case O_OTYPE:
            g_cfg.output_type = parse_type(optarg);
            break;
        case O_OADDR:
            strncpy(g_cfg.output_addr, optarg, sizeof(g_cfg.output_addr) - 1);
            break;
        case O_OPORT:
            g_cfg.output_port = atoi(optarg);
            break;
        case O_OIFACE:
            strncpy(g_cfg.output_iface, optarg, sizeof(g_cfg.output_iface) - 1);
            break;
        case O_OTTL:
            g_cfg.output_ttl = atoi(optarg);
            break;

        /* Filter */
        case O_FSIP: {
            struct in_addr a;
            if (inet_pton(AF_INET, optarg, &a) == 1) {
                g_cfg.filter.src_ip = a.s_addr;
                g_cfg.filter.active = 1;
            }
            break;
        }
        case O_FSPORT:
            g_cfg.filter.src_port = (uint16_t)atoi(optarg);
            g_cfg.filter.active   = 1;
            break;
        case O_FDIP: {
            struct in_addr a;
            if (inet_pton(AF_INET, optarg, &a) == 1) {
                g_cfg.filter.dst_ip = a.s_addr;
                g_cfg.filter.active = 1;
            }
            break;
        }
        case O_FDPORT:
            g_cfg.filter.dst_port = (uint16_t)atoi(optarg);
            g_cfg.filter.active   = 1;
            break;

        /* Error injection */
        case O_DELAY: {
            int v = atoi(optarg);
            if (v >= 1 && v <= 15) {
                g_cfg.injection.delay_enabled = 1;
                g_cfg.injection.delay_seconds = v;
            }
            break;
        }
        case O_DPERIOD:
            g_cfg.injection.delay_period = atoi(optarg);
            break;
        case O_DBURST:
            g_cfg.injection.delay_burst = atoi(optarg);
            break;
        case O_DROPC: {
            int v = atoi(optarg);
            if (v >= 1 && v <= 500) {
                g_cfg.injection.drop_enabled = 1;
                g_cfg.injection.drop_mode    = 0;
                g_cfg.injection.drop_count   = v;
            }
            break;
        }
        case O_DROPP: {
            int v = atoi(optarg);
            if (v >= 1 && v <= 100) {
                g_cfg.injection.drop_enabled  = 1;
                g_cfg.injection.drop_mode     = 1;
                g_cfg.injection.drop_percent  = v;
            }
            break;
        }
        case O_CORRUPT: {
            int v = atoi(optarg);
            if (v >= 1 && v <= 95) {
                g_cfg.injection.corrupt_enabled = 1;
                g_cfg.injection.corrupt_percent = v;
            }
            break;
        }
        case O_DROPPID:
            if (g_cfg.injection.drop_pid_count < MAX_TS_PIDS) {
                g_cfg.injection.drop_pids[g_cfg.injection.drop_pid_count++] =
                    (uint16_t)atoi(optarg);
                g_cfg.injection.pid_drop_enabled = 1;
            }
            break;

        default:
            break;
        }
    }

    if (!g_cfg.input_port) {
        fprintf(stderr, "Error: --input-port is required\n");
        return -1;
    }
    if (!g_cfg.output_port || !g_cfg.output_addr[0]) {
        fprintf(stderr, "Error: --output-addr and --output-port are required\n");
        return -1;
    }
    return 0;
}

/* ==================================================================
 *  Interactive command processing
 * ================================================================== */

static void cmd_help(void)
{
    printf(
        "Commands:\n"
        "  delay <1-15> [period <s>] [burst <s>]  Set delay injection\n"
        "  delay off                               Disable delay\n"
        "  drop count <1-500>                      Drop N packets/sec\n"
        "  drop percent <1-100>                    Drop N%% of packets\n"
        "  drop off                                Disable drop\n"
        "  corrupt <1-95>                          Corrupt N%% of bytes\n"
        "  corrupt off                             Disable corruption\n"
        "  droppid <pid>                           Add PID to drop list\n"
        "  droppid remove <pid>                    Remove PID from list\n"
        "  droppid off                             Disable PID drop\n"
        "  filter src <ip>[:<port>]                Set source filter\n"
        "  filter dst <ip>[:<port>]                Set destination filter\n"
        "  filter clear                            Clear all filters\n"
        "  status                                  Show current settings\n"
        "  stop                                    Disable all injection\n"
        "  reset                                   Reset statistics\n"
        "  help                                    This message\n"
        "  quit / exit                             Shutdown\n");
}

static void process_cmd(const char *line)
{
    char a[64] = "", b[64] = "", c[64] = "", d[64] = "", e[64] = "", f[64] = "";
    int n = sscanf(line, "%63s %63s %63s %63s %63s %63s", a, b, c, d, e, f);
    if (n < 1) return;

    /* --- quit --- */
    if (!strcmp(a, "quit") || !strcmp(a, "exit")) {
        g_cfg.running = 0;
        return;
    }

    /* --- help --- */
    if (!strcmp(a, "help")) { cmd_help(); return; }

    /* --- stop all injection --- */
    if (!strcmp(a, "stop")) {
        pthread_rwlock_wrlock(&g_cfg.config_lock);
        g_cfg.injection.delay_enabled    = 0;
        g_cfg.injection.drop_enabled     = 0;
        g_cfg.injection.corrupt_enabled  = 0;
        g_cfg.injection.pid_drop_enabled = 0;
        pthread_rwlock_unlock(&g_cfg.config_lock);
        printf("All error injection disabled\n");
        return;
    }

    /* --- reset statistics --- */
    if (!strcmp(a, "reset")) {
        atomic_store(&g_cfg.stats.pkts_received,      (uint_fast64_t)0);
        atomic_store(&g_cfg.stats.pkts_forwarded,      (uint_fast64_t)0);
        atomic_store(&g_cfg.stats.pkts_dropped,        (uint_fast64_t)0);
        atomic_store(&g_cfg.stats.pkts_corrupted,      (uint_fast64_t)0);
        atomic_store(&g_cfg.stats.pkts_delayed,        (uint_fast64_t)0);
        atomic_store(&g_cfg.stats.pkts_pid_nullified,  (uint_fast64_t)0);
        atomic_store(&g_cfg.stats.bytes_received,      (uint_fast64_t)0);
        atomic_store(&g_cfg.stats.bytes_forwarded,     (uint_fast64_t)0);
        for (int i = 0; i < MAX_TS_PIDS; i++)
            atomic_store(&g_cfg.stats.pid_drop_counts[i], (uint_fast64_t)0);
        printf("Statistics reset\n");
        return;
    }

    /* --- delay --- */
    if (!strcmp(a, "delay")) {
        if (n < 2) {
            printf("Usage: delay <1-15> [period <s>] [burst <s>] | delay off\n");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.delay_enabled = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Delay disabled\n");
            return;
        }
        int v = atoi(b);
        if (v < 1 || v > 15) { printf("Delay must be 1-15 seconds\n"); return; }

        pthread_rwlock_wrlock(&g_cfg.config_lock);
        g_cfg.injection.delay_enabled = 1;
        g_cfg.injection.delay_seconds = v;
        /* parse optional "period X" and "burst Y" tokens */
        if (n >= 4 && !strcmp(c, "period")) g_cfg.injection.delay_period = atoi(d);
        if (n >= 6 && !strcmp(e, "burst"))  g_cfg.injection.delay_burst  = atoi(f);
        if (n >= 4 && !strcmp(c, "burst"))  g_cfg.injection.delay_burst  = atoi(d);
        pthread_rwlock_unlock(&g_cfg.config_lock);

        printf("Delay: %ds  period=%ds  burst=%ds\n",
               g_cfg.injection.delay_seconds,
               g_cfg.injection.delay_period,
               g_cfg.injection.delay_burst);
        return;
    }

    /* --- drop --- */
    if (!strcmp(a, "drop")) {
        if (n < 2) {
            printf("Usage: drop count <1-500> | drop percent <1-100> | drop off\n");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.drop_enabled = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Drop disabled\n");
            return;
        }
        if (!strcmp(b, "count") && n >= 3) {
            int v = atoi(c);
            if (v < 1 || v > 500) { printf("Count: 1-500\n"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.drop_enabled = 1;
            g_cfg.injection.drop_mode    = 0;
            g_cfg.injection.drop_count   = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Dropping %d packets/sec\n", v);
            return;
        }
        if (!strcmp(b, "percent") && n >= 3) {
            int v = atoi(c);
            if (v < 1 || v > 100) { printf("Percent: 1-100\n"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.drop_enabled  = 1;
            g_cfg.injection.drop_mode     = 1;
            g_cfg.injection.drop_percent  = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Dropping %d%% of packets\n", v);
            return;
        }
        printf("Usage: drop count <N> | drop percent <N> | drop off\n");
        return;
    }

    /* --- corrupt --- */
    if (!strcmp(a, "corrupt")) {
        if (n < 2) {
            printf("Usage: corrupt <1-95> | corrupt off\n");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.corrupt_enabled = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Corruption disabled\n");
            return;
        }
        int v = atoi(b);
        if (v < 1 || v > 95) { printf("Corrupt: 1-95%%\n"); return; }
        pthread_rwlock_wrlock(&g_cfg.config_lock);
        g_cfg.injection.corrupt_enabled = 1;
        g_cfg.injection.corrupt_percent = v;
        pthread_rwlock_unlock(&g_cfg.config_lock);
        printf("Corruption: %d%% of bytes per packet\n", v);
        return;
    }

    /* --- droppid --- */
    if (!strcmp(a, "droppid")) {
        if (n < 2) {
            printf("Usage: droppid <pid> | droppid remove <pid> | droppid off\n");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.pid_drop_enabled = 0;
            g_cfg.injection.drop_pid_count   = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("PID drop disabled\n");
            return;
        }
        if (!strcmp(b, "remove") && n >= 3) {
            uint16_t pid = (uint16_t)atoi(c);
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            for (int i = 0; i < g_cfg.injection.drop_pid_count; i++) {
                if (g_cfg.injection.drop_pids[i] == pid) {
                    memmove(&g_cfg.injection.drop_pids[i],
                            &g_cfg.injection.drop_pids[i + 1],
                            (g_cfg.injection.drop_pid_count - i - 1)
                            * sizeof(uint16_t));
                    g_cfg.injection.drop_pid_count--;
                    if (g_cfg.injection.drop_pid_count == 0)
                        g_cfg.injection.pid_drop_enabled = 0;
                    break;
                }
            }
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Removed PID %u\n", pid);
            return;
        }
        /* Add a PID */
        uint16_t pid = (uint16_t)atoi(b);
        pthread_rwlock_wrlock(&g_cfg.config_lock);
        if (g_cfg.injection.drop_pid_count < MAX_TS_PIDS) {
            g_cfg.injection.drop_pids[g_cfg.injection.drop_pid_count++] = pid;
            g_cfg.injection.pid_drop_enabled = 1;
        } else {
            printf("Max %d PIDs reached\n", MAX_TS_PIDS);
        }
        pthread_rwlock_unlock(&g_cfg.config_lock);
        printf("Dropping PID %u (total: %d)\n", pid, g_cfg.injection.drop_pid_count);
        return;
    }

    /* --- filter --- */
    if (!strcmp(a, "filter")) {
        if (n < 2) {
            printf("Usage: filter src <ip[:port]> | filter dst <ip[:port]> | filter clear\n");
            return;
        }
        if (!strcmp(b, "clear")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            memset(&g_cfg.filter, 0, sizeof(g_cfg.filter));
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Filters cleared\n");
            return;
        }
        if ((!strcmp(b, "src") || !strcmp(b, "dst")) && n >= 3) {
            uint32_t ip;
            uint16_t port;
            if (filter_parse_addr(c, &ip, &port) < 0) {
                printf("Invalid address format (use ip or ip:port)\n");
                return;
            }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            if (!strcmp(b, "src")) {
                g_cfg.filter.src_ip   = ip;
                g_cfg.filter.src_port = port;
            } else {
                g_cfg.filter.dst_ip   = ip;
                g_cfg.filter.dst_port = port;
            }
            g_cfg.filter.active = 1;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            printf("Filter updated\n");
            return;
        }
        printf("Usage: filter src|dst <ip[:port]> | filter clear\n");
        return;
    }

    /* --- status --- */
    if (!strcmp(a, "status")) {
        pthread_rwlock_rdlock(&g_cfg.config_lock);
        injection_config_t inj = g_cfg.injection;
        filter_config_t    flt = g_cfg.filter;
        pthread_rwlock_unlock(&g_cfg.config_lock);

        printf("--- Current Settings ---\n");

        if (inj.delay_enabled)
            printf("  Delay:    %ds  period=%ds  burst=%ds\n",
                   inj.delay_seconds, inj.delay_period, inj.delay_burst);
        else
            printf("  Delay:    OFF\n");

        printf("  Drop:     ");
        if (inj.drop_enabled) {
            if (inj.drop_mode == 0)
                printf("%d pkt/s\n", inj.drop_count);
            else
                printf("%d%%\n", inj.drop_percent);
        } else {
            printf("OFF\n");
        }

        if (inj.corrupt_enabled)
            printf("  Corrupt:  %d%%\n", inj.corrupt_percent);
        else
            printf("  Corrupt:  OFF\n");

        printf("  PID drop:");
        if (inj.pid_drop_enabled && inj.drop_pid_count > 0) {
            for (int i = 0; i < inj.drop_pid_count; i++)
                printf(" %u", inj.drop_pids[i]);
            printf("\n");
        } else {
            printf(" OFF\n");
        }

        printf("  Filter:   ");
        if (flt.active) {
            char sip[INET_ADDRSTRLEN] = "*", dip[INET_ADDRSTRLEN] = "*";
            if (flt.src_ip) {
                struct in_addr aa = { .s_addr = flt.src_ip };
                inet_ntop(AF_INET, &aa, sip, sizeof(sip));
            }
            if (flt.dst_ip) {
                struct in_addr aa = { .s_addr = flt.dst_ip };
                inet_ntop(AF_INET, &aa, dip, sizeof(dip));
            }
            printf("src=%s:%u  dst=%s:%u\n",
                   sip, flt.src_port, dip, flt.dst_port);
        } else {
            printf("NONE\n");
        }
        return;
    }

    printf("Unknown command '%s'. Type 'help' for usage.\n", a);
}

/* ==================================================================
 *  main
 * ================================================================== */

int main(int argc, char **argv)
{
    /* Defaults */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.srt_latency              = 120;
    g_cfg.output_ttl               = 5;
    g_cfg.injection.delay_period   = 30;
    g_cfg.injection.delay_burst    = 10;
    g_cfg.input_fd                 = -1;
    g_cfg.output_fd                = -1;
    g_cfg.srt_input_sock           = SRT_INVALID_SOCK;
    g_cfg.srt_output_sock          = SRT_INVALID_SOCK;
    g_cfg.srt_accepted_sock        = SRT_INVALID_SOCK;

    if (parse_args(argc, argv) != 0)
        return 1;

    pthread_rwlock_init(&g_cfg.config_lock, NULL);
    g_cfg.running = 1;
    setup_signals(on_signal);

    /* SRT global init */
    srt_startup();

    /* Initialise receiver and sender */
    if (receiver_init(&g_cfg) != 0) {
        srt_cleanup();
        return 1;
    }
    if (sender_init(&g_cfg) != 0) {
        receiver_cleanup(&g_cfg);
        srt_cleanup();
        return 1;
    }

    /* Ring buffer between receiver → injector */
    packet_buffer_t ring;
    if (packet_buffer_init(&ring, RING_BUFFER_SIZE) != 0) {
        log_error("Failed to allocate ring buffer");
        sender_cleanup(&g_cfg);
        receiver_cleanup(&g_cfg);
        srt_cleanup();
        return 1;
    }

    /* Injector context */
    injector_ctx_t inj_ctx;
    if (injector_init(&inj_ctx, &g_cfg, &ring) != 0) {
        log_error("Failed to allocate delay queue");
        packet_buffer_destroy(&ring);
        sender_cleanup(&g_cfg);
        receiver_cleanup(&g_cfg);
        srt_cleanup();
        return 1;
    }

    /* Thread contexts */
    receiver_ctx_t recv_ctx = { .cfg = &g_cfg, .buffer = &ring };
    console_ctx_t  con_ctx  = { .cfg = &g_cfg };

    /* Set up console display (scroll region) */
    console_setup();

    /* Spawn worker threads */
    pthread_t t_recv, t_inj, t_con;
    pthread_create(&t_recv, NULL, receiver_thread,  &recv_ctx);
    pthread_create(&t_inj,  NULL, injector_thread,  &inj_ctx);
    pthread_create(&t_con,  NULL, console_thread,   &con_ctx);

    /* Interactive command loop on main thread */
    char line[512];
    while (g_cfg.running) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;
        line[strcspn(line, "\n")] = '\0';
        if (line[0])
            process_cmd(line);
    }

    /* --- Graceful shutdown --- */
    g_cfg.running = 0;

    /* Unblock receiver thread */
    if (g_cfg.input_type == STREAM_SRT) {
        if (g_cfg.srt_accepted_sock != SRT_INVALID_SOCK)
            srt_close(g_cfg.srt_accepted_sock);
        if (g_cfg.srt_input_sock != SRT_INVALID_SOCK)
            srt_close(g_cfg.srt_input_sock);
        g_cfg.srt_accepted_sock = SRT_INVALID_SOCK;
        g_cfg.srt_input_sock    = SRT_INVALID_SOCK;
    } else if (g_cfg.input_fd >= 0) {
        shutdown(g_cfg.input_fd, SHUT_RDWR);
    }

    pthread_join(t_recv, NULL);
    pthread_join(t_inj,  NULL);
    pthread_join(t_con,  NULL);

    console_restore();

    injector_destroy(&inj_ctx);
    packet_buffer_destroy(&ring);
    sender_cleanup(&g_cfg);
    receiver_cleanup(&g_cfg);
    pthread_rwlock_destroy(&g_cfg.config_lock);
    srt_cleanup();

    log_info("Shutdown complete.");
    return 0;
}
