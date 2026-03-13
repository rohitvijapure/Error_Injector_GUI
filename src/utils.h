#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

void     log_info(const char *fmt, ...);
void     log_error(const char *fmt, ...);
void     log_warn(const char *fmt, ...);
uint64_t get_time_ms(void);
void     setup_signals(void (*handler)(int));

#endif /* UTILS_H */
