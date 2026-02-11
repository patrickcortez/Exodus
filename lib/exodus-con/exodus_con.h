#ifndef EXODUS_CON_H
#define EXODUS_CON_H

#include "exodus_console_shared.h"

int excon_open(void);
int excon_create(int fd, int rows, int cols);
int excon_clear(int fd);
int excon_write(int fd, const char *str, int len);
int excon_set_cursor(int fd, int row, int col);
int excon_set_attr(int fd, uint8_t fg, uint8_t bg, uint8_t bold, uint8_t blink);
int excon_scroll(int fd, int lines, int region_top, int region_bottom);
int excon_resize(int fd, int rows, int cols);
int excon_get_size(int fd, int *rows, int *cols);
int excon_push_input(int fd, const char *data, int len);
int excon_read_input(int fd, char *buf, int *len);
void *excon_mmap_buffer(int fd, int rows, int cols);
int excon_unmap_buffer(void *ptr, int rows, int cols);

#endif
