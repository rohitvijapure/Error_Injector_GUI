#include "receiver.h"
#include "utils.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/*  UDP / Multicast / RTP socket setup                                */
/* ------------------------------------------------------------------ */

static int init_udp(app_config_t *cfg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    /* 1-second timeout so the thread can check the running flag */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(cfg->input_port);
    sa.sin_addr.s_addr = (cfg->input_type == STREAM_MULTICAST)
                         ? inet_addr(cfg->input_addr)
                         : INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        log_error("bind port %d: %s", cfg->input_port, strerror(errno));
        close(fd);
        return -1;
    }

    if (cfg->input_iface[0])
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                   cfg->input_iface, strlen(cfg->input_iface) + 1);

    if (cfg->input_type == STREAM_MULTICAST) {
        struct ip_mreqn mr;
        memset(&mr, 0, sizeof(mr));
        mr.imr_multiaddr.s_addr = inet_addr(cfg->input_addr);
        if (cfg->input_iface[0])
            mr.imr_ifindex = if_nametoindex(cfg->input_iface);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mr, sizeof(mr)) < 0) {
            log_error("join multicast %s: %s", cfg->input_addr, strerror(errno));
            close(fd);
            return -1;
        }
        log_info("Joined multicast %s:%d", cfg->input_addr, cfg->input_port);
    } else {
        log_info("Listening %s on port %d",
                 cfg->input_type == STREAM_RTP ? "RTP" : "UDP",
                 cfg->input_port);
    }

    cfg->input_fd = fd;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SRT socket setup                                                  */
/* ------------------------------------------------------------------ */

static int init_srt(app_config_t *cfg)
{
    SRTSOCKET s = srt_create_socket();
    if (s == SRT_INVALID_SOCK) {
        log_error("srt_create_socket: %s", srt_getlasterror_str());
        return -1;
    }

    int tt = SRTT_LIVE;
    srt_setsockflag(s, SRTO_TRANSTYPE, &tt, sizeof(tt));

    int lat = cfg->srt_latency;
    srt_setsockflag(s, SRTO_RCVLATENCY,  &lat, sizeof(lat));
    srt_setsockflag(s, SRTO_PEERLATENCY, &lat, sizeof(lat));

    int rcv_timeout = 1000; /* ms — lets receiver_thread check cfg->running */
    srt_setsockflag(s, SRTO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    int payload = 1456; /* max IPv4: MTU(1500) - UDP(28) - SRT(16) */
    srt_setsockflag(s, SRTO_PAYLOADSIZE, &payload, sizeof(payload));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(cfg->input_port);

    if (cfg->srt_mode == SRT_MODE_LISTENER) {
        sa.sin_addr.s_addr = INADDR_ANY;
        if (srt_bind(s, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR ||
            srt_listen(s, 1) == SRT_ERROR) {
            log_error("SRT listen: %s", srt_getlasterror_str());
            srt_close(s);
            return -1;
        }
        log_info("SRT listening on port %d", cfg->input_port);
    } else {
        int conntimeo = (cfg->srt_conntimeo > 0) ? cfg->srt_conntimeo : 10000;
        srt_setsockflag(s, SRTO_CONNTIMEO, &conntimeo, sizeof(conntimeo));
        inet_pton(AF_INET, cfg->input_addr, &sa.sin_addr);
        if (srt_connect(s, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
            log_error("SRT connect: %s", srt_getlasterror_str());
            srt_close(s);
            return -1;
        }
        log_info("SRT connected to %s:%d", cfg->input_addr, cfg->input_port);
    }

    cfg->srt_input_sock    = s;
    cfg->srt_accepted_sock = SRT_INVALID_SOCK;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int receiver_init(app_config_t *cfg)
{
    return (cfg->input_type == STREAM_SRT) ? init_srt(cfg) : init_udp(cfg);
}

void receiver_cleanup(app_config_t *cfg)
{
    if (cfg->input_type == STREAM_SRT) {
        if (cfg->srt_accepted_sock != SRT_INVALID_SOCK)
            srt_close(cfg->srt_accepted_sock);
        if (cfg->srt_input_sock != SRT_INVALID_SOCK)
            srt_close(cfg->srt_input_sock);
        cfg->srt_accepted_sock = SRT_INVALID_SOCK;
        cfg->srt_input_sock    = SRT_INVALID_SOCK;
    } else {
        if (cfg->input_fd >= 0) {
            if (cfg->input_type == STREAM_MULTICAST) {
                struct ip_mreqn mr;
                memset(&mr, 0, sizeof(mr));
                mr.imr_multiaddr.s_addr = inet_addr(cfg->input_addr);
                setsockopt(cfg->input_fd, IPPROTO_IP,
                           IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
            }
            close(cfg->input_fd);
            cfg->input_fd = -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Receiver thread                                                   */
/* ------------------------------------------------------------------ */

void *receiver_thread(void *arg)
{
    receiver_ctx_t  *ctx = (receiver_ctx_t *)arg;
    app_config_t    *cfg = ctx->cfg;
    packet_buffer_t *buf = ctx->buffer;
    packet_t pkt;

    if (cfg->input_type == STREAM_SRT) {
        /* --- SRT receive loop --- */
        if (cfg->srt_mode == SRT_MODE_LISTENER) {
            log_info("Waiting for SRT caller...");
            struct sockaddr_in ca;
            int calen = sizeof(ca);
            cfg->srt_accepted_sock =
                srt_accept(cfg->srt_input_sock,
                           (struct sockaddr *)&ca, &calen);
            if (cfg->srt_accepted_sock == SRT_INVALID_SOCK) {
                if (cfg->running)
                    log_error("srt_accept: %s", srt_getlasterror_str());
                return NULL;
            }
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof(ip));
            log_info("SRT caller connected from %s:%d",
                     ip, ntohs(ca.sin_port));
        }

        SRTSOCKET rs = (cfg->srt_mode == SRT_MODE_LISTENER)
                       ? cfg->srt_accepted_sock
                       : cfg->srt_input_sock;

        while (cfg->running) {
            pkt.length = 0;
            memset(&pkt.src_addr,  0, sizeof(pkt.src_addr));
            memset(&pkt.dst_addr,  0, sizeof(pkt.dst_addr));
            memset(&pkt.recv_time, 0, sizeof(pkt.recv_time));
            int n = srt_recvmsg(rs, (char *)pkt.data, MAX_PACKET_SIZE);
            if (n == SRT_ERROR) {
                int err = srt_getlasterror(NULL);
                if (err == SRT_ETIMEOUT)
                    continue; /* 1-second timeout — loop back and check running */
                if (cfg->running)
                    log_error("srt_recvmsg: %s", srt_getlasterror_str());
                break;
            }
            pkt.length = n;
            clock_gettime(CLOCK_MONOTONIC, &pkt.recv_time);
            inet_pton(AF_INET, cfg->input_addr, &pkt.src_addr.sin_addr);
            pkt.src_addr.sin_port = htons(cfg->input_port);
            pkt.dst_addr.sin_family = AF_INET;
            pkt.dst_addr.sin_port   = htons(cfg->input_port);
            inet_pton(AF_INET, cfg->input_addr, &pkt.dst_addr.sin_addr);

            atomic_fetch_add(&cfg->stats.pkts_received, 1);
            atomic_fetch_add(&cfg->stats.bytes_received, (uint64_t)n);
            packet_buffer_enqueue(buf, &pkt);
        }

    } else {
        /* --- UDP / Multicast / RTP receive loop --- */
        while (cfg->running) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(cfg->input_fd, pkt.data,
                                 MAX_PACKET_SIZE, 0,
                                 (struct sockaddr *)&from, &flen);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                if (cfg->running)
                    log_error("recvfrom: %s", strerror(errno));
                continue;
            }

            pkt.length   = (int)n;
            pkt.src_addr = from;
            pkt.dst_addr.sin_family = AF_INET;
            pkt.dst_addr.sin_port   = htons(cfg->input_port);
            if (cfg->input_type == STREAM_MULTICAST)
                inet_pton(AF_INET, cfg->input_addr,
                          &pkt.dst_addr.sin_addr);
            clock_gettime(CLOCK_MONOTONIC, &pkt.recv_time);

            atomic_fetch_add(&cfg->stats.pkts_received, 1);
            atomic_fetch_add(&cfg->stats.bytes_received, (uint64_t)n);
            packet_buffer_enqueue(buf, &pkt);
        }
    }

    return NULL;
}
