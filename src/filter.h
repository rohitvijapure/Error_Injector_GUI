#ifndef FILTER_H
#define FILTER_H

#include "config.h"

int filter_matches(const filter_config_t *f, const packet_t *pkt);
int filter_parse_addr(const char *str, uint32_t *ip, uint16_t *port);

#endif /* FILTER_H */
