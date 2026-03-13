#ifndef CONSOLE_H
#define CONSOLE_H

#include "config.h"

void  tui_init(void);
void  tui_shutdown(void);
void  tui_refresh(app_config_t *cfg);
void  tui_draw_input(const char *line, int cursor_pos);
void  tui_log(const char *fmt, ...);

#endif /* CONSOLE_H */
