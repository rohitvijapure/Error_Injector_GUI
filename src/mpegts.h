#ifndef MPEGTS_H
#define MPEGTS_H

#include "config.h"

uint16_t mpegts_get_pid(const uint8_t *ts_pkt);
int      mpegts_find_sync(const uint8_t *data, int len);
int      mpegts_nullify_pids(uint8_t *data, int len,
                             const uint16_t *pids, int pid_count,
                             uint64_t *per_pid_counts);

#endif /* MPEGTS_H */
