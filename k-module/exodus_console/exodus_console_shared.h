#ifndef EXODUS_CONSOLE_SHARED_H
#define EXODUS_CONSOLE_SHARED_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <stdint.h>
#endif

#define EXCON_MAGIC 'E'

#define EXCON_MAX_ROWS       256
#define EXCON_MAX_COLS       512
#define EXCON_DEFAULT_ROWS    24
#define EXCON_DEFAULT_COLS    80
#define EXCON_INPUT_BUF_SIZE 4096
#define EXCON_MAX_SCROLL     10000

typedef struct {
    uint16_t rows;
    uint16_t cols;
    uint16_t cursor_row;
    uint16_t cursor_col;
    uint32_t flags;
    uint32_t fg_color;
    uint32_t bg_color;
    uint32_t dirty_seq;
    uint32_t scroll_offset;
    uint32_t scroll_lines;
    uint8_t  _pad[16];
} excon_header_t;

typedef struct {
    char     ch;
    uint8_t  attr;
} excon_cell_t;

typedef struct {
    uint16_t rows;
    uint16_t cols;
} excon_create_t;

typedef struct {
    uint16_t row;
    uint16_t col;
} excon_cursor_t;

typedef struct {
    int32_t  lines;
    uint16_t region_top;
    uint16_t region_bottom;
} excon_scroll_t;

typedef struct {
    uint16_t rows;
    uint16_t cols;
} excon_resize_t;

typedef struct {
    uint8_t  fg;
    uint8_t  bg;
    uint8_t  bold;
    uint8_t  blink;
} excon_attr_t;

typedef struct {
    uint32_t len;
    char     data[256];
} excon_write_t;

typedef struct {
    uint32_t len;
    char     data[256];
} excon_input_t;

#define EXCON_FLAG_CURSOR_VISIBLE (1 << 0)
#define EXCON_FLAG_WRAP_MODE      (1 << 1)
#define EXCON_FLAG_DIRTY          (1 << 2)

#define EXCON_ATTR_BOLD   (1 << 3)
#define EXCON_ATTR_BLINK  (1 << 7)
#define EXCON_ATTR_FG_MASK  0x07
#define EXCON_ATTR_BG_MASK  0x70
#define EXCON_ATTR_BG_SHIFT 4

#define EXCON_CREATE      _IOW(EXCON_MAGIC, 1, excon_create_t)
#define EXCON_CLEAR       _IO(EXCON_MAGIC, 2)
#define EXCON_WRITE_DATA  _IOW(EXCON_MAGIC, 3, excon_write_t)
#define EXCON_SET_CURSOR  _IOW(EXCON_MAGIC, 4, excon_cursor_t)
#define EXCON_GET_SIZE    _IOR(EXCON_MAGIC, 5, excon_create_t)
#define EXCON_SCROLL      _IOW(EXCON_MAGIC, 6, excon_scroll_t)
#define EXCON_SET_ATTR    _IOW(EXCON_MAGIC, 7, excon_attr_t)
#define EXCON_PUSH_INPUT  _IOW(EXCON_MAGIC, 8, excon_input_t)
#define EXCON_READ_INPUT  _IOWR(EXCON_MAGIC, 9, excon_input_t)
#define EXCON_RESIZE      _IOW(EXCON_MAGIC, 10, excon_resize_t)

#endif
