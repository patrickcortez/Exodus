/* ckg.c - simple standalone package manager client
 * Protocol (simple line-based):
 *  - Client connects to server and sends a command line ending with '\n'.
 *  - Commands: UPDATE, LIST, INSTALL <pkg>
 *  - Server responds with either "OK\n<content-length>\n" followed by <content-length> bytes
 *    or "ERROR <message>\n".
 *
 *  For UPDATE: server sends manifest.txt content.
 *  For INSTALL <pkg>: server sends a lightweight archive:
 *      repeated: <path> '\n' <size> '\n' <raw-bytes>
 *    The client will create directories as needed and write files into the tools folder.
 *
 * Environment variables (all optional):
 *  CKG_SERVER   default: 127.0.0.1:9000
 *  CKG_TOOLS_DIR default: ./tools
 *  CKG_DATA_DIR  default: ./data
 *
 * Build: gcc -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200112L -o ckg ckg.c
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <limits.h>
#ifndef PATH_MAX
/* fallback if system doesn't provide PATH_MAX */
#define PATH_MAX 4096
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#define BACKLOG 5
#define BUFSIZE 8192

static const char *default_server = "127.0.0.1:9000";
static const char *default_tools = "../tools";
static const char *default_data  = "../data";

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static void perror_die(const char *msg) {
    perror(msg);
    exit(1);
}

static int make_dirs_recursive(const char *path) {
    char tmp[PATH_MAX];
    char *p;
    size_t len;

    if (!path || !*path) return -1;
    strncpy(tmp, path, sizeof(tmp)); tmp[sizeof(tmp)-1] = '\0';
    len = strlen(tmp);
    if (tmp[len-1] == '/') tmp[len-1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST) return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST) return -1;
    }
    return 0;
}

static int remove_dir_recursive(const char *path) {
    DIR *d = opendir(path);
    size_t path_len = strlen(path);
    int r = 0;
    if (!d) return -1;
    struct dirent *p;
    while ((p = readdir(d)) != NULL) {
        if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, p->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) { r = -1; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (remove_dir_recursive(full) != 0) r = -1;
            if (rmdir(full) != 0) r = -1;
        } else {
            if (unlink(full) != 0) r = -1;
        }
    }
    closedir(d);
    return r;
}

/* network helpers */
static int connect_hostport(const char *host, const char *port) {
    struct addrinfo hints, *res, *rp;
    int sfd, err;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((err = getaddrinfo(host, port, &hints, &res)) != 0) {
        return -1;
    }
    sfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) continue;
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sfd); sfd = -1;
    }
    freeaddrinfo(res);
    return sfd; /* -1 if failed */
}

/* read a line from socket into buf (buflen), stopping at '\n' (consumes '\n').
 * returns length without newline, or -1 on error/EOF.
 */
static ssize_t sock_readline(int sfd, char *buf, size_t buflen) {
    size_t pos = 0;
    while (pos + 1 < buflen) {
        char c;
        ssize_t r = recv(sfd, &c, 1, 0);
        if (r == 0) return -1; /* EOF */
        if (r < 0) return -1;
        if (c == '\n') {
            buf[pos] = '\0';
            return pos;
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static ssize_t read_n_bytes(int sfd, void *buf, size_t n) {
    size_t total = 0;
    char *p = buf;
    while (total < n) {
        ssize_t r = recv(sfd, p + total, n - total, 0);
        if (r == 0) return -1; /* EOF */
        if (r < 0) return -1;
        total += r;
    }
    return total;
}

/* Parse server string host:port */
static void split_hostport(const char *server, char *host, size_t hostlen, char *port, size_t portlen) {
    const char *colon = strchr(server, ':');
    if (!colon) {
        strncpy(host, server, hostlen-1); host[hostlen-1] = '\0';
        strncpy(port, "9000", portlen-1); port[portlen-1] = '\0';
    } else {
        size_t hlen = colon - server;
        if (hlen >= hostlen) hlen = hostlen - 1;
        strncpy(host, server, hlen); host[hlen] = '\0';
        strncpy(port, colon+1, portlen-1); port[portlen-1] = '\0';
    }
}

static int ensure_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    /* create recursively */
    return make_dirs_recursive(path);
}

/* command: update */
static void cmd_update(const char *server, const char *data_dir) {
    char host[256], port[32]; split_hostport(server, host, sizeof(host), port, sizeof(port));
    int sfd = connect_hostport(host, port);
    if (sfd < 0) {
        fprintf(stderr, "Sorry the server is Down, try again later.\n");
        return;
    }
    char out[256]; snprintf(out, sizeof(out), "UPDATE\n");
    if (send(sfd, out, strlen(out), 0) < 0) { close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    char line[256]; if (sock_readline(sfd, line, sizeof(line)) < 0) { close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    if (strncmp(line, "OK", 2) != 0) {
        fprintf(stderr, "Server error: %s\n", line + (line[0] ? 0 : 0));
        close(sfd);
        return;
    }
    if (sock_readline(sfd, line, sizeof(line)) < 0) { close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    long len = atol(line);
    if (len <= 0) {
        fprintf(stderr, "Server sent empty manifest\n"); close(sfd); return;
    }
    if (ensure_dir_exists(data_dir) != 0) { fprintf(stderr, "Failed to create data dir %s\n", data_dir); close(sfd); return; }
    char tmpfile[PATH_MAX]; snprintf(tmpfile, sizeof(tmpfile), "%s/manifest.txt.tmp", data_dir);
    char final[PATH_MAX]; snprintf(final, sizeof(final), "%s/manifest.txt", data_dir);
    FILE *f = fopen(tmpfile, "wb");
    if (!f) { perror("fopen"); close(sfd); return; }
    long remaining = len; char buf[BUFSIZE];
    while (remaining > 0) {
        ssize_t toread = remaining > BUFSIZE ? BUFSIZE : remaining;
        ssize_t r = read_n_bytes(sfd, buf, toread);
        if (r <= 0) { fclose(f); close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
        fwrite(buf, 1, r, f);
        remaining -= r;
    }
    fclose(f);
    /* atomic replace */
    if (rename(tmpfile, final) != 0) { perror("rename"); }
    printf("manifest updated at %s\n", final);
    close(sfd);
}

/* command: list */
static void cmd_list(const char *data_dir) {
    char manifest[PATH_MAX]; snprintf(manifest, sizeof(manifest), "%s/manifest.txt", data_dir);
    FILE *f = fopen(manifest, "r");
    if (!f) {
        fprintf(stderr, "No manifest found. please do a ckg update first\n");
        return;
    }
    char line[1024];
    printf("Available packages:\n");
    while (fgets(line, sizeof(line), f)) {
        /* trim newline */
        char *p = strchr(line, '\n'); if (p) *p = '\0';
        if (strlen(line) == 0) continue;
        printf("  %s\n", line);
    }
    fclose(f);
}

/* helper: ensure parent dirs exist for a file path */
static int ensure_parent_dirs(const char *filepath) {
    char tmp[PATH_MAX]; strncpy(tmp, filepath, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
    char *p = strrchr(tmp, '/');
    if (!p) return 0; /* no dirs */
    *p = '\0';
    return make_dirs_recursive(tmp);
}

/* command: install <pkg> */
static void cmd_install(const char *server, const char *tools_dir, const char *pkg) {
    /* check manifest exists locally */
    char manifest[PATH_MAX]; snprintf(manifest, sizeof(manifest), "%s/manifest.txt", default_data);
    FILE *mf = fopen(manifest, "r");
    if (!mf) { fprintf(stderr, "No manifest found. please do a ckg update first\n"); return; }
    fclose(mf);

    char host[256], port[32]; split_hostport(server, host, sizeof(host), port, sizeof(port));
    int sfd = connect_hostport(host, port);
    if (sfd < 0) { fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    char out[512]; snprintf(out, sizeof(out), "INSTALL %s\n", pkg);
    if (send(sfd, out, strlen(out), 0) < 0) { close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    char line[256]; if (sock_readline(sfd, line, sizeof(line)) < 0) { close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    if (strncmp(line, "OK", 2) != 0) { fprintf(stderr, "Server error: %s\n", line); close(sfd); return; }
    if (sock_readline(sfd, line, sizeof(line)) < 0) { close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    long len = atol(line);
    if (len <= 0) { fprintf(stderr, "Server sent empty package\n"); close(sfd); return; }
    /* read all payload into memory (simple approach) */
    char *payload = malloc(len);
    if (!payload) { fprintf(stderr, "Out of memory\n"); close(sfd); return; }
    if (read_n_bytes(sfd, payload, len) != len) { free(payload); close(sfd); fprintf(stderr, "Sorry the server is Down, try again later.\n"); return; }
    /* parse payload: repeated entries: <path>\n<size>\n<bytes>
     * write files under tools_dir/<path>
     */
    size_t offset = 0;
    while (offset < (size_t)len) {
        /* read path line */
        size_t start = offset;
        while (offset < (size_t)len && payload[offset] != '\n') offset++;
        if (offset >= (size_t)len) break;
        size_t pathlen = offset - start;
        char pathbuf[PATH_MAX];
        if (pathlen >= sizeof(pathbuf)) { fprintf(stderr, "path too long\n"); break; }
        memcpy(pathbuf, payload + start, pathlen); pathbuf[pathlen] = '\0';
        offset++; /* skip '\n' */
        /* read size line */
        start = offset; while (offset < (size_t)len && payload[offset] != '\n') offset++;
        if (offset >= (size_t)len) break;
        char numbuf[64]; size_t numlen = offset - start; if (numlen >= sizeof(numbuf)) { fprintf(stderr, "size too long\n"); break; }
        memcpy(numbuf, payload + start, numlen); numbuf[numlen] = '\0';
        long fsize = atol(numbuf);
        offset++; /* skip '\n' */
        /* pathbuf is relative path inside package. final path: tools_dir/pathbuf */
        char final[PATH_MAX]; snprintf(final, sizeof(final), "%s/%s", tools_dir, pathbuf);
        /* ensure parent dir exists */
        if (ensure_parent_dirs(final) != 0) {
            fprintf(stderr, "Failed to create directory for %s\n", final);
            /* continue anyway */
        }
        /* write the file */
        FILE *f = fopen(final, "wb");
        if (!f) { perror("fopen"); /* skip bytes */ offset += fsize; continue; }
        if (fsize > 0) {
            if (offset + fsize > (size_t)len) { fprintf(stderr, "Unexpected end of payload\n"); fclose(f); break; }
            size_t wrote = fwrite(payload + offset, 1, fsize, f);
            (void)wrote; /* ignore */
            offset += fsize;
        }
        fclose(f);
        printf("installed %s\n", final);
    }
    free(payload);
    close(sfd);
}

/* command: uninstall <pkg> */
static void cmd_uninstall(const char *tools_dir, const char *pkg) {
    char target[PATH_MAX]; snprintf(target, sizeof(target), "%s/%s", tools_dir, pkg);
    struct stat st;
    if (stat(target, &st) != 0) { fprintf(stderr, "Package %s not found in tools\n", pkg); return; }
    if (!S_ISDIR(st.st_mode)) { /* if file, just remove */
        if (unlink(target) != 0) perror("unlink"); else printf("uninstalled %s\n", target);
        return;
    }
    if (remove_dir_recursive(target) != 0) { fprintf(stderr, "Failed to remove %s\n", target); return; }
    if (rmdir(target) != 0) { perror("rmdir"); }
    printf("uninstalled %s\n", pkg);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s update\n", prog);
    fprintf(stderr, "  %s list\n", prog);
    fprintf(stderr, "  %s install <package>\n", prog);
    fprintf(stderr, "  %s uninstall <package>\n", prog);
}

int main(int argc, char **argv) {
    const char *server = getenv("CKG_SERVER"); if (!server) server = default_server;
    const char *tools  = getenv("CKG_TOOLS_DIR"); if (!tools) tools = default_tools;
    const char *data   = getenv("CKG_DATA_DIR"); if (!data) data = default_data;

    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "update") == 0) {
        cmd_update(server, data);
    } else if (strcmp(cmd, "list") == 0) {
        cmd_list(data);
    } else if (strcmp(cmd, "install") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        if (ensure_dir_exists(tools) != 0) { fprintf(stderr, "Failed to create tools dir %s\n", tools); return 1; }
        cmd_install(server, tools, argv[2]);
    } else if (strcmp(cmd, "uninstall") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        cmd_uninstall(tools, argv[2]);
    } else {
        usage(argv[0]); return 1;
    }
    return 0;
}
