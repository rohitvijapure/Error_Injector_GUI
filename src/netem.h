#ifndef NETEM_H
#define NETEM_H

#include "config.h"

int  netem_apply(netem_config_t *nc);
int  netem_clear(const char *iface);
int  netem_query(const char *iface, char *buf, int buflen);

#endif /* NETEM_H */
