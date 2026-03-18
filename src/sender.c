#include "sender.h"
#include "utils.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>

int sender_init(app_config_t *cfg)
{
    /* --- SRT output --- */
    if (cfg->output_type == STREAM_SRT) {
        SRTSOCKET s = srt_create_socket();
        if (s == SRT_INVALID_SOCK) {
            log_error("srt output socket: %s", srt_getlasterror_str());
            return -1;
        }

        int tt = SRTT_LIVE;
        srt_setsockflag(s, SRTO_TRANSTYPE, &tt, sizeof(tt));

        int lat = cfg->srt_latency;
        srt_setsockflag(s, SRTO_RCVLATENCY,  &lat, sizeof(lat));
        srt_setsockflag(s, SRTO_PEERLATENCY, &lat, sizeof(lat));

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(cfg->output_port);

        if (cfg->srt_output_mode == SRT_MODE_LISTENER) {
            sa.sin_addr.s_addr = INADDR_ANY;
            if (srt_bind(s, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR ||
                srt_listen(s, 1) == SRT_ERROR) {
                log_error("SRT output listen: %s", srt_getlasterror_str());
                srt_close(s);
                return -1;
            }
            log_info("SRT output listening on port %d (waiting for caller...)",
                     cfg->output_port);

            struct sockaddr_in ca;
            int calen = sizeof(ca);
            SRTSOCKET accepted = srt_accept(s, (struct sockaddr *)&ca, &calen);
            if (accepted == SRT_INVALID_SOCK) {
                log_error("SRT output accept: %s", srt_getlasterror_str());
                srt_close(s);
                return -1;
            }
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof(ip));
            log_info("SRT output caller connected from %s:%d",
                     ip, ntohs(ca.sin_port));

            cfg->srt_output_sock          = s;
            cfg->srt_accepted_output_sock = accepted;
        } else {
            int conntimeo = (cfg->srt_conntimeo > 0) ? cfg->srt_conntimeo : 10000;
            srt_setsockflag(s, SRTO_CONNTIMEO, &conntimeo, sizeof(conntimeo));
            inet_pton(AF_INET, cfg->output_addr, &sa.sin_addr);
            if (srt_connect(s, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
                log_error("SRT output connect: %s", srt_getlasterror_str());
                srt_close(s);
                return -1;
            }
            log_info("SRT output connected to %s:%d",
                     cfg->output_addr, cfg->output_port);
            cfg->srt_output_sock          = s;
            cfg->srt_accepted_output_sock = SRT_INVALID_SOCK;
        }
        return 0;
    }

    /* --- UDP / Multicast output --- */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_error("output socket: %s", strerror(errno));
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    if (cfg->output_iface[0])
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                   cfg->output_iface, strlen(cfg->output_iface) + 1);

    if (cfg->output_type == STREAM_MULTICAST) {
        unsigned char ttl = (unsigned char)cfg->output_ttl;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        if (cfg->output_iface[0]) {
            struct ip_mreqn mr;
            memset(&mr, 0, sizeof(mr));
            mr.imr_ifindex = if_nametoindex(cfg->output_iface);
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof(mr));
        }
    }

    memset(&cfg->output_sockaddr, 0, sizeof(cfg->output_sockaddr));
    cfg->output_sockaddr.sin_family = AF_INET;
    cfg->output_sockaddr.sin_port   = htons(cfg->output_port);
    inet_pton(AF_INET, cfg->output_addr, &cfg->output_sockaddr.sin_addr);

    cfg->output_fd = fd;
    log_info("Output %s to %s:%d",
             cfg->output_type == STREAM_MULTICAST ? "multicast" : "UDP",
             cfg->output_addr, cfg->output_port);
    return 0;
}

void sender_cleanup(app_config_t *cfg)
{
    if (cfg->output_type == STREAM_SRT) {
        if (cfg->srt_accepted_output_sock != SRT_INVALID_SOCK) {
            srt_close(cfg->srt_accepted_output_sock);
            cfg->srt_accepted_output_sock = SRT_INVALID_SOCK;
        }
        if (cfg->srt_output_sock != SRT_INVALID_SOCK) {
            srt_close(cfg->srt_output_sock);
            cfg->srt_output_sock = SRT_INVALID_SOCK;
        }
    } else {
        if (cfg->output_fd >= 0) {
            close(cfg->output_fd);
            cfg->output_fd = -1;
        }
    }
}

int sender_send(app_config_t *cfg, const packet_t *pkt)
{
    int ret;

    if (cfg->output_type == STREAM_SRT) {
        SRTSOCKET send_sock = (cfg->srt_output_mode == SRT_MODE_LISTENER &&
                               cfg->srt_accepted_output_sock != SRT_INVALID_SOCK)
                              ? cfg->srt_accepted_output_sock
                              : cfg->srt_output_sock;
        ret = srt_sendmsg(send_sock,
                          (const char *)pkt->data, pkt->length, -1, 1);
        if (ret == SRT_ERROR) {
            log_error("srt_sendmsg: %s", srt_getlasterror_str());
            return -1;
        }
    } else {
        ret = (int)sendto(cfg->output_fd, pkt->data, pkt->length, 0,
                          (struct sockaddr *)&cfg->output_sockaddr,
                          sizeof(cfg->output_sockaddr));
        if (ret < 0)
            return -1;
    }

    atomic_fetch_add(&cfg->stats.pkts_forwarded, 1);
    atomic_fetch_add(&cfg->stats.bytes_forwarded, (uint64_t)pkt->length);
    return 0;
}
