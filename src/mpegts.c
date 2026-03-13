#include "mpegts.h"
#include <string.h>

uint16_t mpegts_get_pid(const uint8_t *ts_pkt)
{
    return (uint16_t)(((ts_pkt[1] & 0x1F) << 8) | ts_pkt[2]);
}

int mpegts_find_sync(const uint8_t *data, int len)
{
    for (int i = 0; i <= len - TS_PACKET_SIZE; i++) {
        if (data[i] != TS_SYNC_BYTE)
            continue;
        /* confirm with next sync byte when possible */
        if (i + TS_PACKET_SIZE < len) {
            if (data[i + TS_PACKET_SIZE] == TS_SYNC_BYTE)
                return i;
        } else {
            return i;
        }
    }
    return -1;
}

static void nullify_ts_packet(uint8_t *pkt)
{
    memset(pkt, 0xFF, TS_PACKET_SIZE);
    pkt[0] = TS_SYNC_BYTE;
    pkt[1] = 0x1F;          /* PID high bits → 0x1FFF (null PID) */
    pkt[2] = 0xFF;           /* PID low bits */
    pkt[3] = 0x10;           /* adaptation_field_control = payload only */
}

int mpegts_nullify_pids(uint8_t *data, int len,
                        const uint16_t *pids, int pid_count,
                        uint64_t *per_pid_counts)
{
    int nullified = 0;
    int off = mpegts_find_sync(data, len);
    if (off < 0)
        return 0;

    while (off + TS_PACKET_SIZE <= len) {
        if (data[off] != TS_SYNC_BYTE) {
            int s = mpegts_find_sync(data + off, len - off);
            if (s < 0) break;
            off += s;
            continue;
        }

        uint16_t pid = mpegts_get_pid(data + off);
        for (int i = 0; i < pid_count; i++) {
            if (pid == pids[i]) {
                nullify_ts_packet(data + off);
                nullified++;
                if (per_pid_counts)
                    per_pid_counts[i]++;
                break;
            }
        }
        off += TS_PACKET_SIZE;
    }
    return nullified;
}
