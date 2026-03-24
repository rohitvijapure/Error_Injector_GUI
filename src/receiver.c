#include "receiver.h"
#include "utils.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <poll.h>

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
/*  SRT Bypass (bidirectional UDP proxy) socket setup                */
/* ------------------------------------------------------------------ */

static int init_srt_bypass(app_config_t *cfg)
{
    /* Socket facing the SRT caller (listens on input_port) */
    int fd_in = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_in < 0) {
        log_error("bypass input socket: %s", strerror(errno));
        return -1;
    }

    int on = 1;
    setsockopt(fd_in, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd_in, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd_in, SOL_SOCKET, SO_SNDBUF, &rcvbuf, sizeof(rcvbuf));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd_in, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(cfg->input_port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd_in, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        log_error("bypass input bind port %d: %s", cfg->input_port, strerror(errno));
        close(fd_in);
        return -1;
    }

    if (cfg->input_iface[0])
        setsockopt(fd_in, SOL_SOCKET, SO_BINDTODEVICE,
                   cfg->input_iface, strlen(cfg->input_iface) + 1);

    /* Socket facing the SRT listener (connects to output_addr:output_port) */
    int fd_out = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_out < 0) {
        log_error("bypass output socket: %s", strerror(errno));
        close(fd_in);
        return -1;
    }

    setsockopt(fd_out, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd_out, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd_out, SOL_SOCKET, SO_SNDBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd_out, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (cfg->output_iface[0])
        setsockopt(fd_out, SOL_SOCKET, SO_BINDTODEVICE,
                   cfg->output_iface, strlen(cfg->output_iface) + 1);

    /* Pre-populate the destination address */
    memset(&cfg->output_sockaddr, 0, sizeof(cfg->output_sockaddr));
    cfg->output_sockaddr.sin_family = AF_INET;
    cfg->output_sockaddr.sin_port   = htons(cfg->output_port);
    inet_pton(AF_INET, cfg->output_addr, &cfg->output_sockaddr.sin_addr);

    cfg->input_fd  = fd_in;
    cfg->bypass_fd = fd_out;
    cfg->output_fd = fd_out;  /* sender_send() uses output_fd for forward direction */

    log_info("SRT Bypass: listening for SRT caller on port %d, forwarding to %s:%d",
             cfg->input_port, cfg->output_addr, cfg->output_port);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int receiver_init(app_config_t *cfg)
{
    if (cfg->input_type == STREAM_SRT)
        return init_srt(cfg);
    if (cfg->input_type == STREAM_SRT_BYPASS)
        return init_srt_bypass(cfg);
    return init_udp(cfg);
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
    } else if (cfg->input_type == STREAM_SRT_BYPASS) {
        if (cfg->input_fd >= 0) {
            close(cfg->input_fd);
            cfg->input_fd = -1;
        }
        if (cfg->bypass_fd >= 0 && cfg->bypass_fd != cfg->output_fd) {
            close(cfg->bypass_fd);
        }
        cfg->bypass_fd = -1;
        cfg->output_fd = -1;
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

    if (cfg->input_type == STREAM_SRT_BYPASS) {
        /* --- SRT Bypass bidirectional UDP proxy loop --- */
        struct pollfd pfds[2];
        pfds[0].fd     = cfg->input_fd;   /* caller-facing socket */
        pfds[0].events = POLLIN;
        pfds[1].fd     = cfg->bypass_fd;  /* destination-facing socket */
        pfds[1].events = POLLIN;

        while (cfg->running) {
            int ready = poll(pfds, 2, 1000); /* 1s timeout to check running */
            if (ready < 0) {
                if (errno == EINTR) continue;
                if (cfg->running)
                    log_error("bypass poll: %s", strerror(errno));
                break;
            }
            if (ready == 0)
                continue;

            /* Packet from SRT Caller → forward to Destination */
            if (pfds[0].revents & POLLIN) {
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                pkt.direction = 0;
                pkt.length    = 0;
                ssize_t n = recvfrom(cfg->input_fd, pkt.data, MAX_PACKET_SIZE,
                                     0, (struct sockaddr *)&from, &flen);
                if (n > 0) {
                    pkt.length   = (int)n;
                    pkt.src_addr = from;
                    pkt.dst_addr = cfg->output_sockaddr;
                    clock_gettime(CLOCK_MONOTONIC, &pkt.recv_time);
                    /* Save the caller address for the backward path */
                    cfg->bypass_client_addr   = from;
                    cfg->bypass_client_active = 1;
                    atomic_fetch_add(&cfg->stats.pkts_received, 1);
                    atomic_fetch_add(&cfg->stats.bytes_received, (uint64_t)n);
                    packet_buffer_enqueue(buf, &pkt);
                }
            }

            /* Packet from SRT Destination → return to Caller */
            if ((pfds[1].revents & POLLIN) && cfg->bypass_client_active) {
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                pkt.direction = 1;
                pkt.length    = 0;
                ssize_t n = recvfrom(cfg->bypass_fd, pkt.data, MAX_PACKET_SIZE,
                                     0, (struct sockaddr *)&from, &flen);
                if (n > 0) {
                    pkt.length   = (int)n;
                    pkt.src_addr = from;
                    pkt.dst_addr = cfg->bypass_client_addr;
                    clock_gettime(CLOCK_MONOTONIC, &pkt.recv_time);
                    /* Return path: enqueue with direction=1 — injector forwards back */
                    packet_buffer_enqueue(buf, &pkt);
                }
            }
        }

    } else if (cfg->input_type == STREAM_SRT) {
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
