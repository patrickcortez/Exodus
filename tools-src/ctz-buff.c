/*
 * ctz-buff.c
 * Stream Buffer Implementation
 */

#define _POSIX_C_SOURCE 200809L
#include "ctz-buff.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

static void ctz_itoa(long long n, char* buf) {
    if (n == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    
    int i = 0;
    int sign = 0;
    if (n < 0) {
        sign = 1;
        n = -n;
    }
    
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    
    if (sign) buf[i++] = '-';
    buf[i] = 0;
    
    // Reverse
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
}

static void ctz_ftoa(double n, char* buf, int precision) {
    long long int_part = (long long)n;
    double frac_part = n - int_part;
    if (frac_part < 0) frac_part = -frac_part;
    
    ctz_itoa(int_part, buf);
    
    if (precision > 0) {
        int len = strlen(buf);
        buf[len++] = '.';
        
        for (int i = 0; i < precision; i++) {
            frac_part *= 10;
            int digit = (int)frac_part;
            buf[len++] = digit + '0';
            frac_part -= digit;
        }
        buf[len] = 0;
    }
}

static int refill(CtzBuff* b) {
    if (b->eof || b->mode != CTZ_MODE_READ) return 0;
    
    // Shift unconsumed data to beginning
    if (b->pos > 0 && b->pos < b->len) {
        size_t remaining = b->len - b->pos;
        memmove(b->buffer, b->buffer + b->pos, remaining);
        b->len = remaining;
        b->pos = 0;
    } else if (b->pos >= b->len) {
        b->len = 0;
        b->pos = 0;
    }
    
    size_t space = b->buf_size - b->len;
    if (space == 0) return 0; // Buffer full
    
    if (b->fd != -1) {
        // File mode
        ssize_t n = read(b->fd, b->buffer + b->len, space);
        if (n <= 0) {
            b->eof = 1;
            return (b->len > 0); // Return true if we still have data
        }
        b->len += (size_t)n;
        return 1;
    } else {
        // String mode
        if (b->str_pos >= b->str_len) {
            b->eof = 1;
            return (b->len > 0);
        }
        
        size_t remaining_str = b->str_len - b->str_pos;
        size_t chunk = (remaining_str > space) ? space : remaining_str;
        
        memcpy(b->buffer + b->len, b->str_source + b->str_pos, chunk);
        b->len += chunk;
        b->str_pos += chunk;
        return 1;
    }
}

int ctz_buff_flush(CtzBuff* b) {
    if (!b || b->mode != CTZ_MODE_WRITE || b->fd == -1) return -1;
    if (b->pos == 0) return 0;
    
    ssize_t written = write(b->fd, b->buffer, b->pos);
    if (written != (ssize_t)b->pos) {
        b->error = 1;
        return -1;
    }
    b->pos = 0;
    return 0;
}

int ctz_buff_init_file(CtzBuff* b, const char* path, const char* mode, size_t buf_size) {
    if (!b || !path || !mode) return -1;
    
    int flags = 0;
    if (strcmp(mode, "r") == 0) {
        flags = O_RDONLY;
        b->mode = CTZ_MODE_READ;
    } else if (strcmp(mode, "w") == 0) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        b->mode = CTZ_MODE_WRITE;
    } else if (strcmp(mode, "a") == 0) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        b->mode = CTZ_MODE_WRITE;
    } else {
        return -1;
    }
    
    b->fd = open(path, flags, 0644);
    if (b->fd < 0) return -1;
    
    b->buf_size = buf_size > 0 ? buf_size : 4096;
    b->buffer = malloc(b->buf_size);
    if (!b->buffer) {
        close(b->fd);
        return -1;
    }
    
    b->pos = 0;
    b->len = 0;
    b->eof = 0;
    b->error = 0;
    b->own_fd = 1; // We opened it, we own it
    b->line = 1;
    b->col = 1;
    b->str_source = NULL;
    
    // Initial fill only for read mode
    if (b->mode == CTZ_MODE_READ) {
        refill(b);
    }
    return 0;
}

int ctz_buff_init_fd(CtzBuff* b, int fd, const char* mode, size_t buf_size, int own_fd) {
    if (!b || !mode) return -1;
    
    if (strcmp(mode, "r") == 0) {
        b->mode = CTZ_MODE_READ;
    } else if (strcmp(mode, "w") == 0 || strcmp(mode, "a") == 0) {
        b->mode = CTZ_MODE_WRITE;
    } else {
        return -1;
    }
    
    b->fd = fd;
    b->buf_size = buf_size > 0 ? buf_size : 4096;
    b->buffer = malloc(b->buf_size);
    if (!b->buffer) {
        return -1;
    }
    
    b->pos = 0;
    b->len = 0;
    b->eof = 0;
    b->error = 0;
    b->own_fd = own_fd;
    b->line = 1;
    b->col = 1;
    b->str_source = NULL;
    
    if (b->mode == CTZ_MODE_READ) {
        refill(b);
    }
    return 0;
}

void ctz_buff_init_string(CtzBuff* b, const char* str) {
    if (!b || !str) return;
    
    b->fd = -1;
    b->str_source = str;
    b->str_len = strlen(str);
    b->str_pos = 0;
    
    b->buf_size = 4096; // Default size for string buffering
    b->buffer = malloc(b->buf_size);
    
    b->pos = 0;
    b->len = 0;
    b->eof = 0;
    b->error = 0;
    b->own_fd = 0;
    b->mode = CTZ_MODE_READ;
    b->line = 1;
    b->col = 1;
    
    refill(b);
}

int ctz_buff_peek(CtzBuff* b) {
    if (!b || b->mode != CTZ_MODE_READ) return -1;
    if (b->pos >= b->len) {
        if (!refill(b)) return -1;
    }
    return (unsigned char)b->buffer[b->pos];
}

int ctz_buff_peek_at(CtzBuff* b, int offset) {
    if (!b || b->mode != CTZ_MODE_READ || offset < 0) return -1;
    
    while (b->pos + offset >= b->len) {
        if (!refill(b)) {
            // If refill didn't add enough data, check if we have it now
            if (b->pos + offset >= b->len) return -1;
            break;
        }
        // If buffer is full and we still can't reach offset, it's an error (offset too big for buffer)
        if (b->len == b->buf_size && b->pos + offset >= b->len) return -1;
    }
    return (unsigned char)b->buffer[b->pos + offset];
}

int ctz_buff_next(CtzBuff* b) {
    int c = ctz_buff_peek(b);
    if (c != -1) {
        b->pos++;
        if (c == '\n') {
            b->line++;
            b->col = 1;
        } else {
            b->col++;
        }
    }
    return c;
}

int ctz_buff_getc(CtzBuff* b) {
    return ctz_buff_next(b);
}

static void skip_whitespace(CtzBuff* b) {
    while (1) {
        int c = ctz_buff_peek(b);
        if (c == -1 || (c != ' ' && c != '\t' && c != '\n' && c != '\r')) break;
        ctz_buff_next(b);
    }
}

int ctz_buff_in(CtzBuff* b, const char* fmt, ...) {
    if (!b || b->mode != CTZ_MODE_READ) return -1;
    
    va_list args;
    va_start(args, fmt);
    int matched = 0;
    
    while (*fmt) {
        if (*fmt == ' ') {
            skip_whitespace(b);
            fmt++;
            continue;
        }
        
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd') {
                skip_whitespace(b);
                // Parse int
                char buf[64];
                int i = 0;
                int c = ctz_buff_peek(b);
                if (c == '-' || c == '+') {
                    buf[i++] = c;
                    ctz_buff_next(b);
                    c = ctz_buff_peek(b);
                }
                while (c >= '0' && c <= '9' && i < 63) {
                    buf[i++] = c;
                    ctz_buff_next(b);
                    c = ctz_buff_peek(b);
                }
                buf[i] = 0;
                if (i > 0 && (buf[0] >= '0' || (i > 1))) {
                    int* ptr = va_arg(args, int*);
                    *ptr = atoi(buf);
                    matched++;
                } else {
                    break;
                }
            } else if (*fmt == 'f') {
                skip_whitespace(b);
                // Parse float (simple)
                char buf[64];
                int i = 0;
                int c = ctz_buff_peek(b);
                if (c == '-' || c == '+') {
                    buf[i++] = c;
                    ctz_buff_next(b);
                    c = ctz_buff_peek(b);
                }
                while ((c >= '0' && c <= '9') || c == '.') {
                    if (i < 63) buf[i++] = c;
                    ctz_buff_next(b);
                    c = ctz_buff_peek(b);
                }
                buf[i] = 0;
                if (i > 0) {
                    double* ptr = va_arg(args, double*);
                    *ptr = atof(buf);
                    matched++;
                } else {
                    break;
                }
            } else if (*fmt == 's') {
                skip_whitespace(b);
                char* ptr = va_arg(args, char*);
                int c = ctz_buff_peek(b);
                while (c != -1 && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    *ptr++ = c;
                    ctz_buff_next(b);
                    c = ctz_buff_peek(b);
                }
                *ptr = 0;
                matched++;
            } else if (*fmt == 'c') {
                // %c does NOT skip whitespace by default in scanf
                char* ptr = va_arg(args, char*);
                int c = ctz_buff_next(b);
                if (c != -1) {
                    *ptr = c;
                    matched++;
                } else {
                    break;
                }
            }
            fmt++;
        } else {
            // Literal match?
            int c = ctz_buff_next(b);
            if (c != *fmt) {
                break; 
            }
            fmt++;
        }
    }
    
    va_end(args);
    return matched;
}

int ctz_buff_putc(CtzBuff* b, char c) {
    if (!b || b->mode != CTZ_MODE_WRITE) return -1;
    
    if (b->pos >= b->buf_size) {
        if (ctz_buff_flush(b) != 0) return -1;
    }
    
    b->buffer[b->pos++] = c;
    return 0;
}

int ctz_buff_write(CtzBuff* b, const void* data, size_t len) {
    if (!b || b->mode != CTZ_MODE_WRITE) return -1;
    const char* p = (const char*)data;
    size_t written = 0;
    
    while (len > 0) {
        size_t space = b->buf_size - b->pos;
        if (space == 0) {
            if (ctz_buff_flush(b) != 0) return -1;
            space = b->buf_size;
        }
        
        size_t chunk = (len > space) ? space : len;
        memcpy(b->buffer + b->pos, p, chunk);
        b->pos += chunk;
        p += chunk;
        len -= chunk;
        written += chunk;
    }
    return (int)written;
}

int ctz_buff_out(CtzBuff* b, const char* fmt, ...) {
    if (!b || b->mode != CTZ_MODE_WRITE) return -1;
    
    va_list args;
    va_start(args, fmt);
    int written = 0;
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd') {
                int val = va_arg(args, int);
                char buf[32];
                ctz_itoa(val, buf);
                ctz_buff_write(b, buf, strlen(buf));
                written += strlen(buf);
            } else if (*fmt == 'f') {
                double val = va_arg(args, double);
                char buf[64];
                ctz_ftoa(val, buf, 6); // Default precision 6
                ctz_buff_write(b, buf, strlen(buf));
                written += strlen(buf);
            } else if (*fmt == 's') {
                char* val = va_arg(args, char*);
                if (val) {
                    ctz_buff_write(b, val, strlen(val));
                    written += strlen(val);
                } else {
                    ctz_buff_write(b, "(null)", 6);
                    written += 6;
                }
            } else if (*fmt == 'c') {
                int val = va_arg(args, int); // char promotes to int in varargs
                ctz_buff_putc(b, (char)val);
                written++;
            } else if (*fmt == '%') {
                ctz_buff_putc(b, '%');
                written++;
            }
            fmt++;
        } else {
            ctz_buff_putc(b, *fmt);
            written++;
            fmt++;
        }
    }
    
    va_end(args);
    return written;
}

void ctz_buff_close(CtzBuff* b) {
    if (!b) return;
    if (b->mode == CTZ_MODE_WRITE) {
        ctz_buff_flush(b);
    }
    if (b->buffer) free(b->buffer);
    if (b->fd != -1 && b->own_fd) close(b->fd);
    memset(b, 0, sizeof(CtzBuff));
}
