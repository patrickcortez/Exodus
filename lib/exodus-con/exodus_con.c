/* gcc -Wall -Wextra -O2 -c exodus_con.c -o exodus_con.o -I../../k-module/exodus_console */
#include "exodus_con.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int excon_open(void) {
    return open("/dev/excon0", O_RDWR);
}

int excon_create(int fd, int rows, int cols) {
    excon_create_t info;
    info.rows = (uint16_t)rows;
    info.cols = (uint16_t)cols;
    return ioctl(fd, EXCON_CREATE, &info);
}

int excon_clear(int fd) {
    return ioctl(fd, EXCON_CLEAR);
}

int excon_write(int fd, const char *str, int len) {
    while (len > 0) {
        excon_write_t wr;
        int chunk = len > (int)sizeof(wr.data) ? (int)sizeof(wr.data) : len;
        wr.len = (uint32_t)chunk;
        memcpy(wr.data, str, (size_t)chunk);
        int ret = ioctl(fd, EXCON_WRITE_DATA, &wr);
        if (ret < 0)
            return ret;
        str += chunk;
        len -= chunk;
    }
    return 0;
}

int excon_set_cursor(int fd, int row, int col) {
    excon_cursor_t cur;
    cur.row = (uint16_t)row;
    cur.col = (uint16_t)col;
    return ioctl(fd, EXCON_SET_CURSOR, &cur);
}

int excon_set_attr(int fd, uint8_t fg, uint8_t bg, uint8_t bold, uint8_t blink) {
    excon_attr_t at;
    at.fg = fg;
    at.bg = bg;
    at.bold = bold;
    at.blink = blink;
    return ioctl(fd, EXCON_SET_ATTR, &at);
}

int excon_scroll(int fd, int lines, int region_top, int region_bottom) {
    excon_scroll_t sc;
    sc.lines = (int32_t)lines;
    sc.region_top = (uint16_t)region_top;
    sc.region_bottom = (uint16_t)region_bottom;
    return ioctl(fd, EXCON_SCROLL, &sc);
}

int excon_resize(int fd, int rows, int cols) {
    excon_resize_t rs;
    rs.rows = (uint16_t)rows;
    rs.cols = (uint16_t)cols;
    return ioctl(fd, EXCON_RESIZE, &rs);
}

int excon_get_size(int fd, int *rows, int *cols) {
    excon_create_t sz;
    int ret = ioctl(fd, EXCON_GET_SIZE, &sz);
    if (ret == 0) {
        if (rows) *rows = sz.rows;
        if (cols) *cols = sz.cols;
    }
    return ret;
}

int excon_push_input(int fd, const char *data, int len) {
    excon_input_t inp;
    int total = 0;

    while (len > 0) {
        int chunk = len > (int)sizeof(inp.data) ? (int)sizeof(inp.data) : len;
        inp.len = (uint32_t)chunk;
        memcpy(inp.data, data, (size_t)chunk);
        int ret = ioctl(fd, EXCON_PUSH_INPUT, &inp);
        if (ret < 0)
            return ret;
        total += ret;
        data += chunk;
        len -= chunk;
    }
    return total;
}

int excon_read_input(int fd, char *buf, int *len) {
    excon_input_t inp;
    inp.len = 0;
    int ret = ioctl(fd, EXCON_READ_INPUT, &inp);
    if (ret < 0)
        return ret;
    memcpy(buf, inp.data, inp.len);
    *len = (int)inp.len;
    return 0;
}

void *excon_mmap_buffer(int fd, int rows, int cols) {
    size_t size = sizeof(excon_header_t) + (size_t)(rows * cols) * sizeof(excon_cell_t);
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size = (size + page_size - 1) & ~(page_size - 1);

    void *ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    return ptr;
}

int excon_unmap_buffer(void *ptr, int rows, int cols) {
    size_t size = sizeof(excon_header_t) + (size_t)(rows * cols) * sizeof(excon_cell_t);
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size = (size + page_size - 1) & ~(page_size - 1);
    return munmap(ptr, size);
}
