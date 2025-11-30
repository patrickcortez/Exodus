/*
 * ctz-buff.h
 * Stream Buffer for Cortez Parser
 */

#ifndef CTZ_BUFF_H
#define CTZ_BUFF_H

#include <stddef.h>

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTZ_MODE_READ  1
#define CTZ_MODE_WRITE 2

typedef struct {
    char* buffer;       // Internal buffer
    size_t buf_size;    // Size of buffer
    size_t pos;         // Current position in buffer
    size_t len;         // Current data length in buffer (read mode) / used (write mode)
    
    int fd;             // File descriptor (-1 for string mode)
    int eof;            // EOF flag
    int mode;           // CTZ_MODE_READ or CTZ_MODE_WRITE
    int error;          // Error flag
    int own_fd;         // If 1, close fd on cleanup
    
    // Position tracking (Read Mode)
    int line;
    int col;
    
    // String mode specific
    const char* str_source; // Read source
    size_t str_len;
    size_t str_pos;
} CtzBuff;

// Initialize buffer from file
// Mode: "r" for read, "w" for write, "a" for append
// Returns 0 on success, -1 on failure
int ctz_buff_init_file(CtzBuff* b, const char* path, const char* mode, size_t buf_size);

// Initialize buffer from existing file descriptor
// own_fd: if 1, ctz_buff_close will close the fd
int ctz_buff_init_fd(CtzBuff* b, int fd, const char* mode, size_t buf_size, int own_fd);

// Initialize buffer from string (Read Only)
void ctz_buff_init_string(CtzBuff* b, const char* str);

// --- Input API ---

// Peek current character (returns -1 on EOF)
int ctz_buff_peek(CtzBuff* b);

// Peek character at offset from current position (0 = current, 1 = next, etc.)
// Returns -1 on EOF or error
int ctz_buff_peek_at(CtzBuff* b, int offset);

// Consume and return current character (returns -1 on EOF)
int ctz_buff_getc(CtzBuff* b);

// Formatted input (scanf-style)
// Supports: %d (int), %f (double), %s (string), %c (char)
// Returns number of items matched or -1 on error/EOF
int ctz_buff_in(CtzBuff* b, const char* fmt, ...);

// --- Output API ---

// Write a single character
// Returns 0 on success, -1 on error
int ctz_buff_putc(CtzBuff* b, char c);

// Write raw data
// Returns bytes written or -1 on error
int ctz_buff_write(CtzBuff* b, const void* data, size_t len);

// Formatted output (printf-style)
// Returns bytes written or -1 on error
int ctz_buff_out(CtzBuff* b, const char* fmt, ...);

// Flush buffer to file
// Returns 0 on success, -1 on error
int ctz_buff_flush(CtzBuff* b);

// Clean up resources
void ctz_buff_close(CtzBuff* b);

#ifdef __cplusplus
}
#endif

#endif // CTZ_BUFF_H
