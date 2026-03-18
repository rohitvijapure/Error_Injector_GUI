#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <srt/srt.h>

#include "config.h"
#include "utils.h"
#include "packet_buffer.h"
#include "receiver.h"
#include "sender.h"
#include "injector.h"
#include "console.h"
#include "filter.h"
#include "netem.h"

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
        "  --srt-mode <listener|caller>           SRT input mode (default: listener)\n"
        "  --srt-latency <ms>                     SRT latency (default: 120)\n"
        "  --srt-conntimeo <ms>                    SRT connection timeout (default: 10000)\n"
        "  --srt-output-mode <listener|caller>    SRT output mode (default: caller)\n\n"
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
    O_SRTM, O_SRTL, O_SRTC, O_SRTOM,
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
    { "srt-conntimeo",   1, 0, O_SRTC   },
    { "srt-output-mode", 1, 0, O_SRTOM  },
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
        case O_SRTC:
            g_cfg.srt_conntimeo = atoi(optarg);
            break;
        case O_SRTOM:
            g_cfg.srt_output_mode = !strcasecmp(optarg, "listener")
                                    ? SRT_MODE_LISTENER : SRT_MODE_CALLER;
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
    tui_show_help();
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

    /* --- stop all Layer 1 injection --- */
    if (!strcmp(a, "stop")) {
        pthread_rwlock_wrlock(&g_cfg.config_lock);
        g_cfg.injection.delay_enabled    = 0;
        g_cfg.injection.drop_enabled     = 0;
        g_cfg.injection.corrupt_enabled  = 0;
        g_cfg.injection.pid_drop_enabled = 0;
        pthread_rwlock_unlock(&g_cfg.config_lock);
        tui_log("All Layer 1 error injection disabled");
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
        tui_log("Statistics reset");
        return;
    }

    /* --- delay --- */
    if (!strcmp(a, "delay")) {
        if (n < 2) {
            tui_log("Usage: delay <1-15> [period <s>] [burst <s>] | delay off");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.delay_enabled = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("Delay disabled");
            return;
        }
        int v = atoi(b);
        if (v < 1 || v > 15) { tui_log("Delay must be 1-15 seconds"); return; }

        pthread_rwlock_wrlock(&g_cfg.config_lock);
        g_cfg.injection.delay_enabled = 1;
        g_cfg.injection.delay_seconds = v;
        if (n >= 4 && !strcmp(c, "period")) g_cfg.injection.delay_period = atoi(d);
        if (n >= 6 && !strcmp(e, "burst"))  g_cfg.injection.delay_burst  = atoi(f);
        if (n >= 4 && !strcmp(c, "burst"))  g_cfg.injection.delay_burst  = atoi(d);
        pthread_rwlock_unlock(&g_cfg.config_lock);

        tui_log("Delay: %ds  period=%ds  burst=%ds",
                g_cfg.injection.delay_seconds,
                g_cfg.injection.delay_period,
                g_cfg.injection.delay_burst);
        return;
    }

    /* --- drop --- */
    if (!strcmp(a, "drop")) {
        if (n < 2) {
            tui_log("Usage: drop count <1-500> | drop percent <1-100> | drop off");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.drop_enabled = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("Drop disabled");
            return;
        }
        if (!strcmp(b, "count") && n >= 3) {
            int v = atoi(c);
            if (v < 1 || v > 500) { tui_log("Count: 1-500"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.drop_enabled = 1;
            g_cfg.injection.drop_mode    = 0;
            g_cfg.injection.drop_count   = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("Dropping %d packets/sec", v);
            return;
        }
        if (!strcmp(b, "percent") && n >= 3) {
            int v = atoi(c);
            if (v < 1 || v > 100) { tui_log("Percent: 1-100"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.drop_enabled  = 1;
            g_cfg.injection.drop_mode     = 1;
            g_cfg.injection.drop_percent  = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("Dropping %d%% of packets", v);
            return;
        }
        tui_log("Usage: drop count <N> | drop percent <N> | drop off");
        return;
    }

    /* --- corrupt --- */
    if (!strcmp(a, "corrupt")) {
        if (n < 2) {
            tui_log("Usage: corrupt <1-95> | corrupt off");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.corrupt_enabled = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("Corruption disabled");
            return;
        }
        int v = atoi(b);
        if (v < 1 || v > 95) { tui_log("Corrupt: 1-95%%"); return; }
        pthread_rwlock_wrlock(&g_cfg.config_lock);
        g_cfg.injection.corrupt_enabled = 1;
        g_cfg.injection.corrupt_percent = v;
        pthread_rwlock_unlock(&g_cfg.config_lock);
        tui_log("Corruption: %d%% of bytes per packet", v);
        return;
    }

    /* --- droppid --- */
    if (!strcmp(a, "droppid")) {
        if (n < 2) {
            tui_log("Usage: droppid <pid> | droppid remove <pid> | droppid off");
            return;
        }
        if (!strcmp(b, "off")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.injection.pid_drop_enabled = 0;
            g_cfg.injection.drop_pid_count   = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("PID drop disabled");
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
            tui_log("Removed PID %u", pid);
            return;
        }
        /* Add a PID */
        uint16_t pid = (uint16_t)atoi(b);
        pthread_rwlock_wrlock(&g_cfg.config_lock);
        if (g_cfg.injection.drop_pid_count < MAX_TS_PIDS) {
            g_cfg.injection.drop_pids[g_cfg.injection.drop_pid_count++] = pid;
            g_cfg.injection.pid_drop_enabled = 1;
        } else {
            tui_log("Max %d PIDs reached", MAX_TS_PIDS);
        }
        pthread_rwlock_unlock(&g_cfg.config_lock);
        tui_log("Dropping PID %u (total: %d)", pid, g_cfg.injection.drop_pid_count);
        return;
    }

    /* --- filter --- */
    if (!strcmp(a, "filter")) {
        if (n < 2) {
            tui_log("Usage: filter src <ip[:port]> | filter dst <ip[:port]> | filter clear");
            return;
        }
        if (!strcmp(b, "clear")) {
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            memset(&g_cfg.filter, 0, sizeof(g_cfg.filter));
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("Filters cleared");
            return;
        }
        if ((!strcmp(b, "src") || !strcmp(b, "dst")) && n >= 3) {
            uint32_t ip;
            uint16_t port;
            if (filter_parse_addr(c, &ip, &port) < 0) {
                tui_log("Invalid address format (use ip or ip:port)");
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
            tui_log("Filter updated");
            return;
        }
        tui_log("Usage: filter src|dst <ip[:port]> | filter clear");
        return;
    }

    /* --- status --- */
    if (!strcmp(a, "status")) {
        pthread_rwlock_rdlock(&g_cfg.config_lock);
        injection_config_t inj = g_cfg.injection;
        filter_config_t    flt = g_cfg.filter;
        netem_config_t     nem = g_cfg.netem;
        pthread_rwlock_unlock(&g_cfg.config_lock);

        tui_log("--- Current Settings ---");

        if (inj.delay_enabled)
            tui_log("  Delay:    %ds  period=%ds  burst=%ds",
                    inj.delay_seconds, inj.delay_period, inj.delay_burst);
        else
            tui_log("  Delay:    OFF");

        if (inj.drop_enabled) {
            if (inj.drop_mode == 0)
                tui_log("  Drop:     %d pkt/s", inj.drop_count);
            else
                tui_log("  Drop:     %d%%", inj.drop_percent);
        } else {
            tui_log("  Drop:     OFF");
        }

        if (inj.corrupt_enabled)
            tui_log("  Corrupt:  %d%%", inj.corrupt_percent);
        else
            tui_log("  Corrupt:  OFF");

        if (inj.pid_drop_enabled && inj.drop_pid_count > 0) {
            char tmp[128];
            int p = 0;
            for (int i = 0; i < inj.drop_pid_count; i++)
                p += snprintf(tmp + p, sizeof(tmp) - p, "%s%u",
                              i ? "," : "", inj.drop_pids[i]);
            tui_log("  PID-drop: %s", tmp);
        } else {
            tui_log("  PID-drop: OFF");
        }

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
            tui_log("  Filter:   src=%s:%u  dst=%s:%u",
                    sip, flt.src_port, dip, flt.dst_port);
        } else {
            tui_log("  Filter:   NONE");
        }

        tui_log("  Netem:    %s on %s",
                nem.active ? "ACTIVE" : "INACTIVE",
                nem.iface[0] ? nem.iface : "(not set)");
        return;
    }

    /* ==============================================================
     *  NETEM (Layer 2) commands
     * ============================================================== */

    if (!strcmp(a, "netem")) {
        if (n < 2) {
            tui_log("Usage: netem loss|delay|reorder|duplicate|corrupt|apply|clear|iface|status");
            return;
        }

        /* netem loss <percent> */
        if (!strcmp(b, "loss")) {
            if (n < 3) { tui_log("Usage: netem loss <0-100>"); return; }
            double v = atof(c);
            if (v < 0 || v > 100) { tui_log("Loss: 0-100"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.netem.loss_percent = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("netem loss staged: %.1f%% (run 'netem apply')", v);
            return;
        }

        /* netem delay <ms> [jitter_ms] */
        if (!strcmp(b, "delay")) {
            if (n < 3) { tui_log("Usage: netem delay <ms> [jitter_ms]"); return; }
            int ms = atoi(c);
            int jit = (n >= 4) ? atoi(d) : 0;
            if (ms < 0) { tui_log("Delay must be >= 0"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.netem.delay_ms  = ms;
            g_cfg.netem.jitter_ms = jit;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("netem delay staged: %dms jitter %dms (run 'netem apply')", ms, jit);
            return;
        }

        /* netem reorder <percent> */
        if (!strcmp(b, "reorder")) {
            if (n < 3) { tui_log("Usage: netem reorder <0-100>"); return; }
            double v = atof(c);
            if (v < 0 || v > 100) { tui_log("Reorder: 0-100"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.netem.reorder_percent = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("netem reorder staged: %.1f%% (run 'netem apply')", v);
            return;
        }

        /* netem duplicate <percent> */
        if (!strcmp(b, "duplicate")) {
            if (n < 3) { tui_log("Usage: netem duplicate <0-100>"); return; }
            double v = atof(c);
            if (v < 0 || v > 100) { tui_log("Duplicate: 0-100"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.netem.duplicate_percent = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("netem duplicate staged: %.1f%% (run 'netem apply')", v);
            return;
        }

        /* netem corrupt <percent> */
        if (!strcmp(b, "corrupt")) {
            if (n < 3) { tui_log("Usage: netem corrupt <0-100>"); return; }
            double v = atof(c);
            if (v < 0 || v > 100) { tui_log("Corrupt: 0-100"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.netem.corrupt_percent = v;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("netem corrupt staged: %.1f%% (run 'netem apply')", v);
            return;
        }

        /* netem iface <name> */
        if (!strcmp(b, "iface")) {
            if (n < 3) { tui_log("Usage: netem iface <name>"); return; }
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            strncpy(g_cfg.netem.iface, c, sizeof(g_cfg.netem.iface) - 1);
            g_cfg.netem.iface[sizeof(g_cfg.netem.iface) - 1] = '\0';
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("netem interface set to '%s'", c);
            return;
        }

        /* netem apply */
        if (!strcmp(b, "apply")) {
            pthread_rwlock_rdlock(&g_cfg.config_lock);
            netem_config_t nc = g_cfg.netem;
            pthread_rwlock_unlock(&g_cfg.config_lock);

            if (!nc.iface[0]) {
                tui_log("netem: set interface first (netem iface <name>)");
                return;
            }
            int ret = netem_apply(&nc);
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.netem.active = nc.active;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            if (ret == 0)
                tui_log("netem applied on %s", nc.iface);
            else
                tui_log("netem apply FAILED on %s (need root?)", nc.iface);
            return;
        }

        /* netem clear */
        if (!strcmp(b, "clear")) {
            pthread_rwlock_rdlock(&g_cfg.config_lock);
            char iface[64];
            strncpy(iface, g_cfg.netem.iface, sizeof(iface));
            iface[sizeof(iface) - 1] = '\0';
            pthread_rwlock_unlock(&g_cfg.config_lock);

            netem_clear(iface);
            pthread_rwlock_wrlock(&g_cfg.config_lock);
            g_cfg.netem.active = 0;
            pthread_rwlock_unlock(&g_cfg.config_lock);
            tui_log("netem cleared on %s", iface);
            return;
        }

        /* netem status */
        if (!strcmp(b, "status")) {
            pthread_rwlock_rdlock(&g_cfg.config_lock);
            char iface[64];
            strncpy(iface, g_cfg.netem.iface, sizeof(iface));
            iface[sizeof(iface) - 1] = '\0';
            pthread_rwlock_unlock(&g_cfg.config_lock);

            char buf[1024] = "";
            netem_query(iface, buf, sizeof(buf));
            if (buf[0]) {
                /* Print each line of tc output to the log */
                char *saveptr = NULL;
                char *tok = strtok_r(buf, "\n", &saveptr);
                while (tok) {
                    tui_log("  %s", tok);
                    tok = strtok_r(NULL, "\n", &saveptr);
                }
            } else {
                tui_log("netem: no qdisc on %s",
                        iface[0] ? iface : "(not set)");
            }
            return;
        }

        tui_log("Unknown netem command '%s'. Type 'help'.", b);
        return;
    }

    tui_log("Unknown command '%s'. Type 'help'.", a);
}

/* ==================================================================
 *  main
 * ================================================================== */

int main(int argc, char **argv)
{
    /* Defaults */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.srt_latency              = 120;
    g_cfg.srt_conntimeo            = 10000;  /* 10 s connection timeout */
    g_cfg.output_ttl               = 5;
    g_cfg.injection.delay_period   = 30;
    g_cfg.injection.delay_burst    = 10;
    g_cfg.input_fd                 = -1;
    g_cfg.output_fd                = -1;
    g_cfg.srt_input_sock           = SRT_INVALID_SOCK;
    g_cfg.srt_output_sock          = SRT_INVALID_SOCK;
    g_cfg.srt_accepted_sock        = SRT_INVALID_SOCK;
    g_cfg.srt_accepted_output_sock = SRT_INVALID_SOCK;

    if (parse_args(argc, argv) != 0)
        return 1;

    /* Default netem interface to output interface */
    if (!g_cfg.netem.iface[0] && g_cfg.output_iface[0])
        strncpy(g_cfg.netem.iface, g_cfg.output_iface,
                sizeof(g_cfg.netem.iface) - 1);

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

    /* Ring buffer between receiver -> injector */
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

    /* Start ncurses TUI */
    tui_init();
    tui_log("Error Injector v2.0 started");

    /* Spawn worker threads (no console thread — main handles TUI) */
    pthread_t t_recv, t_inj;
    pthread_create(&t_recv, NULL, receiver_thread, &recv_ctx);
    pthread_create(&t_inj,  NULL, injector_thread, &inj_ctx);

    /* ========================================================
     *  Main loop: ncurses display refresh + character input
     * ======================================================== */

    char cmdline[512] = "";
    int  cmdpos = 0;

    while (g_cfg.running) {
        tui_refresh(&g_cfg);
        tui_draw_input(cmdline, cmdpos);

        int ch = getch();

        if (ch == ERR)
            continue;

        if (ch == KEY_RESIZE) {
            clear();
            continue;
        }

        if (ch == '\n' || ch == KEY_ENTER) {
            cmdline[cmdpos] = '\0';
            if (cmdpos > 0) {
                tui_log("> %s", cmdline);
                process_cmd(cmdline);
            }
            cmdpos = 0;
            cmdline[0] = '\0';
            continue;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cmdpos > 0) {
                cmdpos--;
                cmdline[cmdpos] = '\0';
            }
            tui_draw_input(cmdline, cmdpos);
            continue;
        }

        if (cmdpos < (int)sizeof(cmdline) - 1 && ch >= 32 && ch < 127) {
            cmdline[cmdpos++] = (char)ch;
            cmdline[cmdpos] = '\0';
            tui_draw_input(cmdline, cmdpos);
        }
    }

    /* --- Graceful shutdown --- */
    g_cfg.running = 0;

    /* Auto-clear netem rules */
    if (g_cfg.netem.active) {
        netem_clear(g_cfg.netem.iface);
        g_cfg.netem.active = 0;
    }

    /* Shutdown TUI before joining threads (so error messages go to stderr) */
    tui_shutdown();

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

    /* Unblock output if SRT listener is waiting for a caller */
    if (g_cfg.output_type == STREAM_SRT) {
        if (g_cfg.srt_accepted_output_sock != SRT_INVALID_SOCK) {
            srt_close(g_cfg.srt_accepted_output_sock);
            g_cfg.srt_accepted_output_sock = SRT_INVALID_SOCK;
        }
        if (g_cfg.srt_output_sock != SRT_INVALID_SOCK) {
            srt_close(g_cfg.srt_output_sock);
            g_cfg.srt_output_sock = SRT_INVALID_SOCK;
        }
    }

    pthread_join(t_recv, NULL);
    pthread_join(t_inj,  NULL);

    injector_destroy(&inj_ctx);
    packet_buffer_destroy(&ring);
    sender_cleanup(&g_cfg);
    receiver_cleanup(&g_cfg);
    pthread_rwlock_destroy(&g_cfg.config_lock);
    srt_cleanup();

    log_info("Shutdown complete.");
    return 0;
}
