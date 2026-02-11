/* gcc -Wall -Wextra -O2 -c utils.c -o utils.o -Iinclude */
#include "utils.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#include "exodus_console/exodus_console_shared.h"

static int try_excon_clear(void) {
    int fd = open("/dev/excon0", O_RDWR);
    if (fd < 0)
        return -1;
    int ret = ioctl(fd, EXCON_CLEAR);
    close(fd);
    return ret;
}

static int get_vt_number(void) {
    char *tty = ttyname(STDIN_FILENO);
    if (!tty)
        return -1;

    if (strncmp(tty, "/dev/tty", 8) == 0 && tty[8] >= '1' && tty[8] <= '9')
        return tty[8] - '0';

    return -1;
}

static int clear_vt_buffer(int vt_num) {
    char vcsa_path[32];
    snprintf(vcsa_path, sizeof(vcsa_path), "/dev/vcsa%d", vt_num);

    int rfd = open(vcsa_path, O_RDONLY);
    if (rfd < 0)
        return -1;

    unsigned char header[4];
    if (read(rfd, header, 4) != 4) {
        close(rfd);
        return -1;
    }
    close(rfd);

    int rows = header[0];
    int cols = header[1];
    int total_cells = rows * cols;
    int buf_size = 4 + (total_cells * 2);

    unsigned char buf[buf_size];
    buf[0] = (unsigned char)rows;
    buf[1] = (unsigned char)cols;
    buf[2] = 0;
    buf[3] = 0;

    for (int i = 0; i < total_cells; i++) {
        buf[4 + (i * 2)]     = ' ';
        buf[4 + (i * 2) + 1] = 0x07;
    }

    int wfd = open(vcsa_path, O_WRONLY);
    if (wfd < 0)
        return -1;

    write(wfd, buf, (size_t)buf_size);
    close(wfd);
    return 0;
}

static void clear_pty(void) {
    int tty_fd = open("/dev/tty", O_WRONLY);
    if (tty_fd < 0)
        tty_fd = STDOUT_FILENO;

    write(tty_fd, "\033[H\033[2J\033[3J", 11);

    if (tty_fd != STDOUT_FILENO)
        close(tty_fd);
}

void exodus_clear_screen(void) {
    if (try_excon_clear() == 0)
        return;

    int vt_num = get_vt_number();
    if (vt_num > 0 && clear_vt_buffer(vt_num) == 0)
        return;

    clear_pty();
}
