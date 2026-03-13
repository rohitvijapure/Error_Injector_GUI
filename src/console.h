#ifndef CONSOLE_H
#define CONSOLE_H

#include "config.h"

typedef struct {
    app_config_t *cfg;
} console_ctx_t;

void  console_setup(void);
void  console_restore(void);
void *console_thread(void *arg);

#endif /* CONSOLE_H */
