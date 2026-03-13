#ifndef SENDER_H
#define SENDER_H

#include "config.h"

int  sender_init(app_config_t *cfg);
void sender_cleanup(app_config_t *cfg);
int  sender_send(app_config_t *cfg, const packet_t *pkt);

#endif /* SENDER_H */
