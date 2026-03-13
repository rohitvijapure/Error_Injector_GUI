#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <netinet/in.h>
#include <srt/srt.h>

#define MAX_PACKET_SIZE    9000
#define RING_BUFFER_SIZE   2048
#define MAX_DELAY_QUEUE    200000
#define MAX_TS_PIDS        16
#define TS_PACKET_SIZE     188
#define TS_SYNC_BYTE       0x47
#define TS_NULL_PID        0x1FFF

typedef enum {
    STREAM_UDP = 0,
    STREAM_MULTICAST,
    STREAM_RTP,
    STREAM_SRT
} stream_type_t;

typedef enum {
    SRT_MODE_LISTENER = 0,
    SRT_MODE_CALLER
} srt_mode_t;

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    int      active;
} filter_config_t;

typedef struct {
    int      delay_enabled;
    int      delay_seconds;      /* 1-15 */
    int      delay_period;       /* cycle length in seconds */
    int      delay_burst;        /* burst window in seconds */

    int      drop_enabled;
    int      drop_mode;          /* 0 = count, 1 = percent */
    int      drop_count;         /* 1-500 packets/sec */
    int      drop_percent;       /* 1-100 */

    int      corrupt_enabled;
    int      corrupt_percent;    /* 1-95 (% of bytes per packet) */

    int      pid_drop_enabled;
    uint16_t drop_pids[MAX_TS_PIDS];
    int      drop_pid_count;
} injection_config_t;

typedef struct {
    atomic_uint_fast64_t pkts_received;
    atomic_uint_fast64_t pkts_forwarded;
    atomic_uint_fast64_t pkts_dropped;
    atomic_uint_fast64_t pkts_corrupted;
    atomic_uint_fast64_t pkts_delayed;
    atomic_uint_fast64_t pkts_pid_nullified;
    atomic_uint_fast64_t bytes_received;
    atomic_uint_fast64_t bytes_forwarded;
    atomic_uint_fast64_t pid_drop_counts[MAX_TS_PIDS];
} stats_t;

typedef struct {
    uint8_t            data[MAX_PACKET_SIZE];
    int                length;
    struct sockaddr_in src_addr;
    struct sockaddr_in dst_addr;
    struct timespec    recv_time;
} packet_t;

typedef struct {
    /* Input */
    stream_type_t      input_type;
    char               input_addr[256];
    int                input_port;
    char               input_iface[64];
    srt_mode_t         srt_mode;
    int                srt_latency;

    /* Output */
    stream_type_t      output_type;
    char               output_addr[256];
    int                output_port;
    char               output_iface[64];
    int                output_ttl;

    /* Runtime config (protected by config_lock) */
    filter_config_t    filter;
    injection_config_t injection;

    /* Statistics (lock-free atomics) */
    stats_t            stats;

    /* Synchronisation */
    pthread_rwlock_t   config_lock;
    volatile int       running;

    /* Sockets */
    int                input_fd;
    int                output_fd;
    SRTSOCKET          srt_input_sock;
    SRTSOCKET          srt_output_sock;
    SRTSOCKET          srt_accepted_sock;
    struct sockaddr_in output_sockaddr;
} app_config_t;

#endif /* CONFIG_H */
