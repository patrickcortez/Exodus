/* gcc -Wall -Wextra -O2 -c excon_io.c -o excon_io.o -Iinclude -Ik-module */
#include "excon_io.h"
#include "exodus_console/exodus_console_shared.h"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

static int excon_fd = -1;
static int stdout_pipe[2] = {-1, -1};
static int stderr_pipe[2] = {-1, -1};
static int original_stdout = -1;
static int original_stderr = -1;
static pthread_t stdout_thread;
static pthread_t stderr_thread;
static volatile int redirect_running = 0;

static void excon_write_raw(const char *data, int len) {
    while (len > 0) {
        excon_write_t wr;
        int chunk = len > (int)sizeof(wr.data) ? (int)sizeof(wr.data) : len;
        wr.len = (uint32_t)chunk;
        memcpy(wr.data, data, (size_t)chunk);
        ioctl(excon_fd, EXCON_WRITE_DATA, &wr);
        data += chunk;
        len -= chunk;
    }
}

static void *pipe_reader_thread(void *arg) {
    int read_fd = *(int *)arg;
    char buf[512];
    while (redirect_running) {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n <= 0) break;
        excon_write_raw(buf, (int)n);
    }
    return NULL;
}

int excon_io_init(void) {
    if (!getenv("EXODUS_EXCON"))
        return -1;

    const char *fd_str = getenv("EXCON_FD");
    if (fd_str) {
        excon_fd = atoi(fd_str);
        if (excon_fd < 0) return -1;
    } else {
        excon_fd = open("/dev/excon0", O_RDWR);
        if (excon_fd < 0) return -1;

        excon_create_t info;
        info.rows = 40;
        info.cols = 120;
        int ret = ioctl(excon_fd, EXCON_CREATE, &info);
        if (ret < 0) {
            close(excon_fd);
            excon_fd = -1;
            return -1;
        }
    }

    original_stdout = dup(STDOUT_FILENO);
    original_stderr = dup(STDERR_FILENO);

    if (pipe(stdout_pipe) == 0) {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[1]);
        stdout_pipe[1] = -1;
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    if (pipe(stderr_pipe) == 0) {
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stderr_pipe[1]);
        stderr_pipe[1] = -1;
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    redirect_running = 1;
    pthread_create(&stdout_thread, NULL, pipe_reader_thread, &stdout_pipe[0]);
    pthread_create(&stderr_thread, NULL, pipe_reader_thread, &stderr_pipe[0]);

    return 0;
}

void excon_io_shutdown(void) {
    if (excon_fd < 0) return;

    redirect_running = 0;

    if (original_stdout >= 0) {
        dup2(original_stdout, STDOUT_FILENO);
        close(original_stdout);
        original_stdout = -1;
    }
    if (original_stderr >= 0) {
        dup2(original_stderr, STDERR_FILENO);
        close(original_stderr);
        original_stderr = -1;
    }

    if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); stdout_pipe[0] = -1; }
    if (stderr_pipe[0] >= 0) { close(stderr_pipe[0]); stderr_pipe[0] = -1; }

    pthread_join(stdout_thread, NULL);
    pthread_join(stderr_thread, NULL);

    excon_fd = -1;
}

int excon_io_active(void) {
    return excon_fd >= 0;
}

int excon_io_get_fd(void) {
    return excon_fd;
}

int excon_io_write(const char *data, int len) {
    if (excon_fd < 0)
        return -1;
    excon_write_raw(data, len);
    return 0;
}

int excon_io_write_str(const char *str) {
    return excon_io_write(str, (int)strlen(str));
}

int excon_io_read_input(char *buf, int bufsize) {
    if (excon_fd < 0)
        return -1;

    excon_input_t inp;
    inp.len = 0;
    int ret = ioctl(excon_fd, EXCON_READ_INPUT, &inp);
    if (ret < 0)
        return ret;

    int copy = (int)inp.len;
    if (copy > bufsize) copy = bufsize;
    memcpy(buf, inp.data, (size_t)copy);
    return copy;
}

static char read_buf[256];
static int read_buf_pos = 0;
static int read_buf_len = 0;

int shell_write(const char *data, int len) {
    if (excon_fd >= 0)
        return excon_io_write(data, len);
    return (int)write(STDOUT_FILENO, data, (size_t)len);
}

int shell_read_byte(char *c) {
    if (excon_fd >= 0) {
        while (1) {
            if (read_buf_pos < read_buf_len) {
                *c = read_buf[read_buf_pos++];
                return 1;
            }
            int n = excon_io_read_input(read_buf, (int)sizeof(read_buf));
            if (n > 0) {
                read_buf_pos = 0;
                read_buf_len = n;
                continue;
            }
            usleep(1000);
        }
    }
    return (int)read(STDIN_FILENO, c, 1);
}
