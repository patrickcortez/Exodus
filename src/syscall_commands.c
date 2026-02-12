/* gcc -Wall -Wextra -O2 -c syscall_commands.c -o syscall_commands.o -Iinclude */
#include "syscall_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static repl_value_t make_int(long v) {
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_INT;
    r.int_val = v;
    snprintf(r.str_val, sizeof(r.str_val), "%ld", v);
    r.str_len = (int)strlen(r.str_val);
    return r;
}

static repl_value_t make_string(const char *s, int len) {
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_STRING;
    if (len < 0) len = (int)strlen(s);
    if (len >= REPL_MAX_VAR_VALUE) len = REPL_MAX_VAR_VALUE - 1;
    memcpy(r.str_val, s, (size_t)len);
    r.str_val[len] = '\0';
    r.str_len = len;
    return r;
}

static repl_value_t make_error(const char *syscall_name) {
    (void)syscall_name; // unused
    return make_int(-1);
}

static repl_value_t make_status(int ret, const char *name) {
    if (ret < 0)
        return make_error(name);
    return make_int(ret);
}

static int parse_open_flags(const char *s) {
    int flags = 0;
    if (strstr(s, "O_RDONLY")) flags |= O_RDONLY;
    if (strstr(s, "O_WRONLY")) flags |= O_WRONLY;
    if (strstr(s, "O_RDWR"))   flags |= O_RDWR;
    if (strstr(s, "O_CREAT"))  flags |= O_CREAT;
    if (strstr(s, "O_TRUNC"))  flags |= O_TRUNC;
    if (strstr(s, "O_APPEND")) flags |= O_APPEND;
    if (strstr(s, "O_EXCL"))   flags |= O_EXCL;
    if (flags == 0 && s[0] >= '0' && s[0] <= '9')
        flags = (int)strtol(s, NULL, 0);
    return flags;
}

static mode_t parse_mode(const char *s) {
    return (mode_t)strtol(s, NULL, 8);
}

static int parse_signal(const char *s) {
    if (strcmp(s, "SIGTERM") == 0) return SIGTERM;
    if (strcmp(s, "SIGKILL") == 0) return SIGKILL;
    if (strcmp(s, "SIGINT") == 0)  return SIGINT;
    if (strcmp(s, "SIGHUP") == 0)  return SIGHUP;
    if (strcmp(s, "SIGUSR1") == 0) return SIGUSR1;
    if (strcmp(s, "SIGUSR2") == 0) return SIGUSR2;
    if (strcmp(s, "SIGSTOP") == 0) return SIGSTOP;
    if (strcmp(s, "SIGCONT") == 0) return SIGCONT;
    return (int)strtol(s, NULL, 0);
}

static repl_value_t cmd_sys_open(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-open <path> <flags> [mode]", -1);
    int flags = parse_open_flags(argv[2]);
    mode_t mode = (argc > 3) ? parse_mode(argv[3]) : 0644;
    int fd = open(argv[1], flags, mode);
    return make_status(fd, "open");
}

static repl_value_t cmd_sys_read(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-read <fd> <count>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    int count = (int)strtol(argv[2], NULL, 0);
    if (count > REPL_MAX_VAR_VALUE - 1) count = REPL_MAX_VAR_VALUE - 1;
    char buf[count + 1];
    ssize_t n = read(fd, buf, (size_t)count);
    if (n < 0) return make_error("read");
    buf[n] = '\0';
    return make_string(buf, (int)n);
}

static repl_value_t cmd_sys_write(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-write <fd> <data>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    ssize_t n = write(fd, argv[2], strlen(argv[2]));
    if (n < 0) return make_error("write");
    return make_int(n);
}

static repl_value_t cmd_sys_close(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-close <fd>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    return make_status(close(fd), "close");
}

static repl_value_t cmd_sys_stat(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-stat <path>", -1);
    struct stat st;
    if (stat(argv[1], &st) < 0) return make_error("stat");
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_STRING;
    r.str_len = snprintf(r.str_val, sizeof(r.str_val),
        "mode=%o size=%ld uid=%d gid=%d links=%ld inode=%ld",
        st.st_mode, (long)st.st_size, st.st_uid, st.st_gid,
        (long)st.st_nlink, (long)st.st_ino);
    return r;
}

static repl_value_t cmd_sys_mkdir(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-mkdir <path> [mode]", -1);
    mode_t mode = (argc > 2) ? parse_mode(argv[2]) : 0755;
    return make_status(mkdir(argv[1], mode), "mkdir");
}

static repl_value_t cmd_sys_unlink(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-unlink <path>", -1);
    return make_status(unlink(argv[1]), "unlink");
}

static repl_value_t cmd_sys_rename(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-rename <old> <new>", -1);
    return make_status(rename(argv[1], argv[2]), "rename");
}

static repl_value_t cmd_sys_chmod(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-chmod <path> <mode>", -1);
    return make_status(chmod(argv[1], parse_mode(argv[2])), "chmod");
}

static repl_value_t cmd_sys_chown(int argc, char **argv) {
    if (argc < 4) return make_string("[error] usage: sys-chown <path> <uid> <gid>", -1);
    uid_t uid = (uid_t)strtol(argv[2], NULL, 0);
    gid_t gid = (gid_t)strtol(argv[3], NULL, 0);
    return make_status(chown(argv[1], uid, gid), "chown");
}

static repl_value_t cmd_sys_link(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-link <target> <linkname>", -1);
    return make_status(link(argv[1], argv[2]), "link");
}

static repl_value_t cmd_sys_symlink(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-symlink <target> <linkname>", -1);
    return make_status(symlink(argv[1], argv[2]), "symlink");
}

static repl_value_t cmd_sys_lseek(int argc, char **argv) {
    if (argc < 4) return make_string("[error] usage: sys-lseek <fd> <offset> <whence>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    off_t offset = (off_t)strtol(argv[2], NULL, 0);
    int whence = SEEK_SET;
    if (strcmp(argv[3], "SEEK_CUR") == 0) whence = SEEK_CUR;
    if (strcmp(argv[3], "SEEK_END") == 0) whence = SEEK_END;
    off_t result = lseek(fd, offset, whence);
    if (result < 0) return make_error("lseek");
    return make_int(result);
}

static repl_value_t cmd_sys_truncate(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-truncate <path> <length>", -1);
    off_t len = (off_t)strtol(argv[2], NULL, 0);
    return make_status(truncate(argv[1], len), "truncate");
}

static repl_value_t cmd_sys_readlink(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-readlink <path>", -1);
    char buf[REPL_MAX_VAR_VALUE];
    ssize_t n = readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) return make_error("readlink");
    buf[n] = '\0';
    return make_string(buf, (int)n);
}

struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

static repl_value_t cmd_sys_getdents(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-getdents <fd>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    char buf[4096];
    long nread = syscall(SYS_getdents64, fd, buf, sizeof(buf));
    if (nread < 0) return make_error("getdents64");

    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_STRING;
    r.str_len = 0;

    long pos = 0;
    while (pos < nread) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
        if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0) {
            int remaining = REPL_MAX_VAR_VALUE - r.str_len - 2;
            if (remaining <= 0) break;
            int written = snprintf(r.str_val + r.str_len, (size_t)remaining, "%s%s",
                                   r.str_len > 0 ? "\n" : "", d->d_name);
            if (written > 0) r.str_len += written;
        }
        pos += d->d_reclen;
    }
    return r;
}

static repl_value_t cmd_sys_brk(int argc, char **argv) {
    void *addr = NULL;
    if (argc >= 2)
        addr = (void *)(uintptr_t)strtol(argv[1], NULL, 0);
    void *result = (void *)syscall(SYS_brk, addr);
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_INT;
    r.int_val = (long)(uintptr_t)result;
    snprintf(r.str_val, sizeof(r.str_val), "0x%lx", (unsigned long)(uintptr_t)result);
    r.str_len = (int)strlen(r.str_val);
    return r;
}

static repl_value_t cmd_sys_pipe(int argc, char **argv) {
    (void)argc; (void)argv;
    int pipefd[2];
    if (pipe(pipefd) < 0) return make_error("pipe");
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_STRING;
    r.str_len = snprintf(r.str_val, sizeof(r.str_val), "read=%d write=%d", pipefd[0], pipefd[1]);
    r.int_val = pipefd[0];
    return r;
}

static repl_value_t cmd_sys_dup2(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-dup2 <oldfd> <newfd>", -1);
    int oldfd = (int)strtol(argv[1], NULL, 0);
    int newfd = (int)strtol(argv[2], NULL, 0);
    int ret = dup2(oldfd, newfd);
    return make_status(ret, "dup2");
}

static repl_value_t cmd_sys_getcwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return make_error("getcwd");
    return make_string(buf, -1);
}

static repl_value_t cmd_sys_chdir(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-chdir <path>", -1);
    return make_status(chdir(argv[1]), "chdir");
}

static repl_value_t cmd_sys_fork(int argc, char **argv) {
    (void)argc; (void)argv;
    pid_t pid = fork();
    if (pid < 0) return make_error("fork");
    return make_int(pid);
}

static repl_value_t cmd_sys_kill(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-kill <pid> <signal>", -1);
    pid_t pid = (pid_t)strtol(argv[1], NULL, 0);
    int sig = parse_signal(argv[2]);
    return make_status(kill(pid, sig), "kill");
}

static repl_value_t cmd_sys_wait(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-wait <pid>", -1);
    pid_t pid = (pid_t)strtol(argv[1], NULL, 0);
    int status;
    pid_t result = waitpid(pid, &status, 0);
    if (result < 0) return make_error("waitpid");
    return make_int(WEXITSTATUS(status));
}

static repl_value_t cmd_sys_getpid(int argc, char **argv) {
    (void)argc; (void)argv;
    return make_int(getpid());
}

static repl_value_t cmd_sys_getuid(int argc, char **argv) {
    (void)argc; (void)argv;
    return make_int(getuid());
}

static repl_value_t cmd_sys_nice(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-nice <increment>", -1);
    int inc = (int)strtol(argv[1], NULL, 0);
    errno = 0;
    int result = nice(inc);
    if (result == -1 && errno != 0) return make_error("nice");
    return make_int(result);
}

static repl_value_t cmd_sys_mmap(int argc, char **argv) {
    if (argc < 5) return make_string("[error] usage: sys-mmap <length> <prot> <flags> <fd> [offset]", -1);
    size_t length = (size_t)strtol(argv[1], NULL, 0);
    int prot = 0;
    if (strstr(argv[2], "PROT_READ"))  prot |= PROT_READ;
    if (strstr(argv[2], "PROT_WRITE")) prot |= PROT_WRITE;
    if (strstr(argv[2], "PROT_EXEC"))  prot |= PROT_EXEC;
    int flags_val = 0;
    if (strstr(argv[3], "MAP_SHARED"))  flags_val |= MAP_SHARED;
    if (strstr(argv[3], "MAP_PRIVATE")) flags_val |= MAP_PRIVATE;
    if (strstr(argv[3], "MAP_ANON"))    flags_val |= MAP_ANONYMOUS;
    int fd = (int)strtol(argv[4], NULL, 0);
    off_t offset = argc > 5 ? (off_t)strtol(argv[5], NULL, 0) : 0;
    void *ptr = mmap(NULL, length, prot, flags_val, fd, offset);
    if (ptr == MAP_FAILED) return make_error("mmap");
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_INT;
    r.int_val = (long)(uintptr_t)ptr;
    snprintf(r.str_val, sizeof(r.str_val), "0x%lx", (unsigned long)(uintptr_t)ptr);
    r.str_len = (int)strlen(r.str_val);
    return r;
}

static repl_value_t cmd_sys_munmap(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-munmap <addr> <length>", -1);
    void *addr = (void *)(uintptr_t)strtol(argv[1], NULL, 0);
    size_t length = (size_t)strtol(argv[2], NULL, 0);
    return make_status(munmap(addr, length), "munmap");
}

static repl_value_t cmd_sys_socket(int argc, char **argv) {
    if (argc < 4) return make_string("[error] usage: sys-socket <domain> <type> <protocol>", -1);
    int domain = AF_INET;
    if (strcmp(argv[1], "AF_INET6") == 0) domain = AF_INET6;
    if (strcmp(argv[1], "AF_UNIX") == 0)  domain = AF_UNIX;
    int type_val = SOCK_STREAM;
    if (strcmp(argv[2], "SOCK_DGRAM") == 0) type_val = SOCK_DGRAM;
    if (strcmp(argv[2], "SOCK_RAW") == 0)   type_val = SOCK_RAW;
    int proto = (int)strtol(argv[3], NULL, 0);
    int fd = socket(domain, type_val, proto);
    return make_status(fd, "socket");
}

static repl_value_t cmd_sys_connect(int argc, char **argv) {
    if (argc < 4) return make_string("[error] usage: sys-connect <fd> <addr> <port>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(argv[3]));
    inet_pton(AF_INET, argv[2], &addr.sin_addr);
    return make_status(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), "connect");
}

static repl_value_t cmd_sys_bind(int argc, char **argv) {
    if (argc < 4) return make_string("[error] usage: sys-bind <fd> <addr> <port>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(argv[3]));
    inet_pton(AF_INET, argv[2], &addr.sin_addr);
    return make_status(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), "bind");
}

static repl_value_t cmd_sys_listen(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-listen <fd> <backlog>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    int backlog = (int)strtol(argv[2], NULL, 0);
    return make_status(listen(fd, backlog), "listen");
}

static repl_value_t cmd_sys_accept(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-accept <fd>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int newfd = accept(fd, (struct sockaddr *)&addr, &len);
    return make_status(newfd, "accept");
}

static repl_value_t cmd_sys_send(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-send <fd> <data>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    ssize_t n = send(fd, argv[2], strlen(argv[2]), 0);
    if (n < 0) return make_error("send");
    return make_int(n);
}

static repl_value_t cmd_sys_recv(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-recv <fd> <len>", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    int count = (int)strtol(argv[2], NULL, 0);
    if (count > REPL_MAX_VAR_VALUE - 1) count = REPL_MAX_VAR_VALUE - 1;
    char buf[count + 1];
    ssize_t n = recv(fd, buf, (size_t)count, 0);
    if (n < 0) return make_error("recv");
    buf[n] = '\0';
    return make_string(buf, (int)n);
}

static repl_value_t cmd_sys_ioctl(int argc, char **argv) {
    if (argc < 3) return make_string("[error] usage: sys-ioctl <fd> <request> [arg]", -1);
    int fd = (int)strtol(argv[1], NULL, 0);
    unsigned long request = strtoul(argv[2], NULL, 0);
    unsigned long arg_val = argc > 3 ? strtoul(argv[3], NULL, 0) : 0;
    int ret = ioctl(fd, request, arg_val);
    return make_status(ret, "ioctl");
}

static repl_value_t cmd_sys_sysinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sysinfo si;
    if (sysinfo(&si) < 0) return make_error("sysinfo");
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_STRING;
    unsigned long total_mb = si.totalram * si.mem_unit / (1024 * 1024);
    unsigned long free_mb = si.freeram * si.mem_unit / (1024 * 1024);
    unsigned long used_mb = total_mb - free_mb;
    r.str_len = snprintf(r.str_val, sizeof(r.str_val),
        "uptime: %lds | RAM: %luM/%luM (used %luM) | procs: %d | load: %.2f %.2f %.2f",
        si.uptime, used_mb, total_mb, used_mb, si.procs,
        si.loads[0] / 65536.0, si.loads[1] / 65536.0, si.loads[2] / 65536.0);
    return r;
}

static repl_value_t cmd_sys_uname(int argc, char **argv) {
    (void)argc; (void)argv;
    struct utsname un;
    if (uname(&un) < 0) return make_error("uname");
    repl_value_t r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_STRING;
    r.str_len = snprintf(r.str_val, sizeof(r.str_val),
        "%s %s %s %s %s",
        un.sysname, un.nodename, un.release, un.version, un.machine);
    return r;
}

static repl_value_t cmd_sys_mount(int argc, char **argv) {
    if (argc < 5) return make_string("[error] usage: sys-mount <source> <target> <fstype> <flags>", -1);
    unsigned long mflags = strtoul(argv[4], NULL, 0);
    return make_status(mount(argv[1], argv[2], argv[3], mflags, NULL), "mount");
}

static repl_value_t cmd_sys_umount(int argc, char **argv) {
    if (argc < 2) return make_string("[error] usage: sys-umount <target>", -1);
    return make_status(umount(argv[1]), "umount");
}

static syscall_cmd_entry_t cmd_table[] = {
    {"sys-open",      cmd_sys_open},
    {"sys-read",      cmd_sys_read},
    {"sys-write",     cmd_sys_write},
    {"sys-close",     cmd_sys_close},
    {"sys-stat",      cmd_sys_stat},
    {"sys-getdents",  cmd_sys_getdents},
    {"sys-mkdir",     cmd_sys_mkdir},
    {"sys-unlink",    cmd_sys_unlink},
    {"sys-rename",    cmd_sys_rename},
    {"sys-chmod",     cmd_sys_chmod},
    {"sys-chown",     cmd_sys_chown},
    {"sys-link",      cmd_sys_link},
    {"sys-symlink",   cmd_sys_symlink},
    {"sys-lseek",     cmd_sys_lseek},
    {"sys-truncate",  cmd_sys_truncate},
    {"sys-readlink",  cmd_sys_readlink},
    {"sys-fork",      cmd_sys_fork},
    {"sys-kill",      cmd_sys_kill},
    {"sys-wait",      cmd_sys_wait},
    {"sys-getpid",    cmd_sys_getpid},
    {"sys-getuid",    cmd_sys_getuid},
    {"sys-nice",      cmd_sys_nice},
    {"sys-mmap",      cmd_sys_mmap},
    {"sys-munmap",    cmd_sys_munmap},
    {"sys-brk",       cmd_sys_brk},
    {"sys-pipe",      cmd_sys_pipe},
    {"sys-dup2",      cmd_sys_dup2},
    {"sys-getcwd",    cmd_sys_getcwd},
    {"sys-chdir",     cmd_sys_chdir},
    {"sys-socket",    cmd_sys_socket},
    {"sys-connect",   cmd_sys_connect},
    {"sys-bind",      cmd_sys_bind},
    {"sys-listen",    cmd_sys_listen},
    {"sys-accept",    cmd_sys_accept},
    {"sys-send",      cmd_sys_send},
    {"sys-recv",      cmd_sys_recv},
    {"sys-ioctl",     cmd_sys_ioctl},
    {"sys-sysinfo",   cmd_sys_sysinfo},
    {"sys-uname",     cmd_sys_uname},
    {"sys-mount",     cmd_sys_mount},
    {"sys-umount",    cmd_sys_umount},
    {NULL, NULL}
};

int is_syscall_command(const char *cmd) {
    for (int i = 0; cmd_table[i].name; i++) {
        if (strcmp(cmd, cmd_table[i].name) == 0)
            return 1;
    }
    return 0;
}

repl_value_t dispatch_syscall(const char *name, int argc, char **argv) {
    for (int i = 0; cmd_table[i].name; i++) {
        if (strcmp(name, cmd_table[i].name) == 0)
            return cmd_table[i].func(argc, argv);
    }
    return make_string("[error] unknown syscall command", -1);
}

void syscall_commands_init(void) {
}
