#include "console.h"
#include "utils.h"
#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <arpa/inet.h>

/* ================================================================
 *  Thread-safe log ring buffer
 * ================================================================ */

#define LOG_RING_SIZE  128
#define LOG_LINE_LEN   256

static char            log_ring[LOG_RING_SIZE][LOG_LINE_LEN];
static int             log_head  = 0;
static int             log_count = 0;
static pthread_mutex_t log_lock  = PTHREAD_MUTEX_INITIALIZER;

void tui_log(const char *fmt, ...)
{
    pthread_mutex_lock(&log_lock);

    int idx;
    if (log_count >= LOG_RING_SIZE) {
        idx = log_head;
        log_head = (log_head + 1) % LOG_RING_SIZE;
    } else {
        idx = (log_head + log_count) % LOG_RING_SIZE;
        log_count++;
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int pos = snprintf(log_ring[idx], LOG_LINE_LEN,
                       "[%02d:%02d:%02d] ",
                       t->tm_hour, t->tm_min, t->tm_sec);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_ring[idx] + pos, LOG_LINE_LEN - pos, fmt, ap);
    va_end(ap);

    pthread_mutex_unlock(&log_lock);
}

/* ================================================================
 *  Helpers
 * ================================================================ */

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

/* ================================================================
 *  TUI init / shutdown
 * ================================================================ */

void tui_init(void)
{
    initscr();
    cbreak();
    noecho();
    halfdelay(5);           /* 500 ms timeout on getch() */
    keypad(stdscr, TRUE);
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN,   -1);  /* headers     */
        init_pair(2, COLOR_GREEN,  -1);  /* active/on   */
        init_pair(3, COLOR_RED,    -1);  /* off/error   */
        init_pair(4, COLOR_YELLOW, -1);  /* separators  */
    }
}

void tui_shutdown(void)
{
    endwin();
}

/* ================================================================
 *  Full-screen refresh (called from main thread every ~500 ms)
 * ================================================================ */

void tui_refresh(app_config_t *cfg)
{
    static uint64_t        prev_in  = 0, prev_out = 0, prev_t = 0;
    static struct timespec t0       = {0, 0};

    if (prev_t == 0) {
        prev_t = get_time_ms();
        clock_gettime(CLOCK_MONOTONIC, &t0);
    }

    /* --- Bitrate calculation --- */
    uint64_t now  = get_time_ms();
    uint64_t bin  = atomic_load(&cfg->stats.bytes_received);
    uint64_t bout = atomic_load(&cfg->stats.bytes_forwarded);
    uint64_t dt   = now - prev_t;
    double mbps_in  = dt ? (double)(bin  - prev_in)  * 8.0 / (dt * 1000.0) : 0;
    double mbps_out = dt ? (double)(bout - prev_out) * 8.0 / (dt * 1000.0) : 0;
    prev_in  = bin;
    prev_out = bout;
    prev_t   = now;

    /* Uptime */
    struct timespec tn;
    clock_gettime(CLOCK_MONOTONIC, &tn);
    int up = (int)(tn.tv_sec - t0.tv_sec);

    /* Snapshot config under lock */
    pthread_rwlock_rdlock(&cfg->config_lock);
    injection_config_t inj = cfg->injection;
    filter_config_t    flt = cfg->filter;
    netem_config_t     nem = cfg->netem;
    pthread_rwlock_unlock(&cfg->config_lock);

    /* --- Layout --- */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int hdr_h   = 3;
    int log_h   = 5;
    int input_y = rows - 1;
    int sep3_y  = rows - 2;
    int log_y   = sep3_y - log_h;
    int sep2_y  = log_y - 1;
    int panel_y = hdr_h + 1;
    int panel_h = sep2_y - panel_y;
    int half    = cols / 2;

    if (panel_h < 4) panel_h = 4;

    erase();

    /* ---- HEADER ---- */
    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(0, 0, "=== ERROR INJECTOR v2.0 ===");
    attroff(A_BOLD | COLOR_PAIR(1));
    mvprintw(0, 28, "  Uptime %02d:%02d:%02d",
             up / 3600, (up % 3600) / 60, up % 60);

    mvprintw(1, 0, "Input:  %-10s %s:%d (%s)  In:  %.2f Mbps",
             type_str(cfg->input_type), cfg->input_addr, cfg->input_port,
             cfg->input_iface[0] ? cfg->input_iface : "*", mbps_in);
    mvprintw(2, 0, "Output: %-10s %s:%d (%s)  Out: %.2f Mbps",
             type_str(cfg->output_type), cfg->output_addr, cfg->output_port,
             cfg->output_iface[0] ? cfg->output_iface : "*", mbps_out);

    /* ---- SEPARATOR 1 ---- */
    attron(COLOR_PAIR(4));
    mvhline(hdr_h, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(4));

    /* ---- LAYER 1 PANEL (left) ---- */
    int y = panel_y;
    int lw = half - 1;   /* left panel usable width */

    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(y, 1, "LAYER 1 - Application");
    attroff(A_BOLD | COLOR_PAIR(1));
    y++;

    mvprintw(y, 1, "Pkts In: %-10lu  Out: %-10lu",
             (unsigned long)atomic_load(&cfg->stats.pkts_received),
             (unsigned long)atomic_load(&cfg->stats.pkts_forwarded));
    y++;
    mvprintw(y, 1, "Dropped: %-8lu  Corrupted: %-8lu",
             (unsigned long)atomic_load(&cfg->stats.pkts_dropped),
             (unsigned long)atomic_load(&cfg->stats.pkts_corrupted));
    y++;
    mvprintw(y, 1, "Delayed: %-8lu  PID-null:  %-8lu",
             (unsigned long)atomic_load(&cfg->stats.pkts_delayed),
             (unsigned long)atomic_load(&cfg->stats.pkts_pid_nullified));
    y += 2;

    /* Settings */
    if (y < sep2_y) {
        if (inj.delay_enabled) {
            attron(COLOR_PAIR(2));
            mvprintw(y, 1, "Delay:   %ds every %ds (burst %ds)",
                     inj.delay_seconds, inj.delay_period, inj.delay_burst);
            attroff(COLOR_PAIR(2));
        } else {
            attron(COLOR_PAIR(3));
            mvprintw(y, 1, "Delay:   OFF");
            attroff(COLOR_PAIR(3));
        }
        y++;
    }
    if (y < sep2_y) {
        if (inj.drop_enabled) {
            attron(COLOR_PAIR(2));
            if (inj.drop_mode == 0)
                mvprintw(y, 1, "Drop:    %d pkt/s", inj.drop_count);
            else
                mvprintw(y, 1, "Drop:    %d%%", inj.drop_percent);
            attroff(COLOR_PAIR(2));
        } else {
            attron(COLOR_PAIR(3));
            mvprintw(y, 1, "Drop:    OFF");
            attroff(COLOR_PAIR(3));
        }
        y++;
    }
    if (y < sep2_y) {
        if (inj.corrupt_enabled) {
            attron(COLOR_PAIR(2));
            mvprintw(y, 1, "Corrupt: %d%%", inj.corrupt_percent);
            attroff(COLOR_PAIR(2));
        } else {
            attron(COLOR_PAIR(3));
            mvprintw(y, 1, "Corrupt: OFF");
            attroff(COLOR_PAIR(3));
        }
        y++;
    }
    if (y < sep2_y) {
        if (inj.pid_drop_enabled && inj.drop_pid_count > 0) {
            char pids[128];
            int p = 0;
            for (int i = 0; i < inj.drop_pid_count && p < (int)sizeof(pids) - 8; i++)
                p += snprintf(pids + p, sizeof(pids) - p,
                              "%s%u", i ? "," : "", inj.drop_pids[i]);
            attron(COLOR_PAIR(2));
            mvprintw(y, 1, "PID-drop: %s", pids);
            attroff(COLOR_PAIR(2));
        } else {
            attron(COLOR_PAIR(3));
            mvprintw(y, 1, "PID-drop: OFF");
            attroff(COLOR_PAIR(3));
        }
        y++;
    }
    if (y < sep2_y) {
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
            mvprintw(y, 1, "Filter:  src=%s:%u dst=%s:%u",
                     sip, flt.src_port, dip, flt.dst_port);
        } else {
            mvprintw(y, 1, "Filter:  NONE");
        }
    }

    (void)lw;

    /* ---- VERTICAL DIVIDER ---- */
    attron(COLOR_PAIR(4));
    mvvline(panel_y, half, ACS_VLINE, panel_h);
    mvaddch(hdr_h, half, ACS_TTEE);
    mvaddch(sep2_y, half, ACS_BTEE);
    attroff(COLOR_PAIR(4));

    /* ---- LAYER 2 PANEL (right) ---- */
    y = panel_y;
    int rx = half + 2;

    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(y, rx, "LAYER 2 - Network (tc netem)");
    attroff(A_BOLD | COLOR_PAIR(1));
    y++;

    mvprintw(y, rx, "Interface: %s",
             nem.iface[0] ? nem.iface : "(not set)");
    y++;

    if (nem.loss_percent > 0) {
        attron(COLOR_PAIR(2));
        mvprintw(y, rx, "Loss:      %.1f%%", nem.loss_percent);
        attroff(COLOR_PAIR(2));
    } else {
        mvprintw(y, rx, "Loss:      OFF");
    }
    y++;

    if (nem.delay_ms > 0) {
        attron(COLOR_PAIR(2));
        if (nem.jitter_ms > 0)
            mvprintw(y, rx, "Delay:     %dms (+/- %dms)",
                     nem.delay_ms, nem.jitter_ms);
        else
            mvprintw(y, rx, "Delay:     %dms", nem.delay_ms);
        attroff(COLOR_PAIR(2));
    } else {
        mvprintw(y, rx, "Delay:     OFF");
    }
    y++;

    if (nem.reorder_percent > 0) {
        attron(COLOR_PAIR(2));
        mvprintw(y, rx, "Reorder:   %.1f%%", nem.reorder_percent);
        attroff(COLOR_PAIR(2));
    } else {
        mvprintw(y, rx, "Reorder:   OFF");
    }
    y++;

    if (nem.duplicate_percent > 0) {
        attron(COLOR_PAIR(2));
        mvprintw(y, rx, "Duplicate: %.1f%%", nem.duplicate_percent);
        attroff(COLOR_PAIR(2));
    } else {
        mvprintw(y, rx, "Duplicate: OFF");
    }
    y++;

    if (nem.corrupt_percent > 0) {
        attron(COLOR_PAIR(2));
        mvprintw(y, rx, "Corrupt:   %.1f%%", nem.corrupt_percent);
        attroff(COLOR_PAIR(2));
    } else {
        mvprintw(y, rx, "Corrupt:   OFF");
    }
    y += 2;

    if (y < sep2_y) {
        if (nem.active) {
            attron(A_BOLD | COLOR_PAIR(2));
            mvprintw(y, rx, "[ACTIVE]");
            attroff(A_BOLD | COLOR_PAIR(2));
        } else {
            attron(COLOR_PAIR(3));
            mvprintw(y, rx, "[INACTIVE]");
            attroff(COLOR_PAIR(3));
        }
    }

    /* ---- SEPARATOR 2 ---- */
    attron(COLOR_PAIR(4));
    mvhline(sep2_y, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(4));

    /* ---- LOG PANEL ---- */
    pthread_mutex_lock(&log_lock);
    int visible = log_h;
    if (log_count < visible) visible = log_count;
    for (int i = 0; i < visible; i++) {
        int idx = (log_head + log_count - visible + i) % LOG_RING_SIZE;
        mvprintw(log_y + i, 1, "%.*s", cols - 2, log_ring[idx]);
    }
    pthread_mutex_unlock(&log_lock);

    /* ---- SEPARATOR 3 ---- */
    attron(COLOR_PAIR(4));
    mvhline(sep3_y, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(4));

    /* input line is drawn by tui_draw_input() */
    (void)input_y;

    refresh();
}

/* ================================================================
 *  Input line (called after every keystroke and after refresh)
 * ================================================================ */

void tui_draw_input(const char *line, int cursor_pos)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;

    int y = rows - 1;
    move(y, 0);
    clrtoeol();
    mvprintw(y, 0, "> %s", line);
    move(y, 2 + cursor_pos);
    refresh();
}
