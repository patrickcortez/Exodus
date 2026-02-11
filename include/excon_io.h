#ifndef EXCON_IO_H
#define EXCON_IO_H

int excon_io_init(void);
void excon_io_shutdown(void);
int excon_io_active(void);
int excon_io_write(const char *data, int len);
int excon_io_write_str(const char *str);
int excon_io_read_input(char *buf, int bufsize);
int excon_io_get_fd(void);

int shell_write(const char *data, int len);
int shell_read_byte(char *c);

#endif
