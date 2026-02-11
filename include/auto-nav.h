#ifndef AUTO_NAV_H
#define AUTO_NAV_H

#include <termios.h>

void shell_enable_raw_mode_nav(struct termios* orig_termios);
void shell_disable_raw_mode_nav(struct termios* orig_termios);
int check_conflict(const char* input);
int shell_resolve_conflict(const char* input);

#endif
