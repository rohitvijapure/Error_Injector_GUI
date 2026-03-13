#include "filter.h"
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>

int filter_matches(const filter_config_t *f, const packet_t *pkt)
{
    if (!f->active)
        return 1;
    if (f->src_ip   && pkt->src_addr.sin_addr.s_addr != f->src_ip)
        return 0;
    if (f->src_port && ntohs(pkt->src_addr.sin_port) != f->src_port)
        return 0;
    if (f->dst_ip   && pkt->dst_addr.sin_addr.s_addr != f->dst_ip)
        return 0;
    if (f->dst_port && ntohs(pkt->dst_addr.sin_port) != f->dst_port)
        return 0;
    return 1;
}

int filter_parse_addr(const char *str, uint32_t *ip, uint16_t *port)
{
    char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    *ip   = 0;
    *port = 0;

    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = '\0';
        *port = (uint16_t)atoi(colon + 1);
    }
    if (buf[0]) {
        struct in_addr a;
        if (inet_pton(AF_INET, buf, &a) != 1)
            return -1;
        *ip = a.s_addr;
    }
    return 0;
}
