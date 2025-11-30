/* src/cortez_backend.c
 *
 * Build:
 *     gcc -o cortez_backend cortez_backend.c -lutil -lpthread
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#include <fnmatch.h>
#include <ctype.h>
#include "cokernel/kernel.h"
#include "cokernel/syscalls.h"
#include "cokernel/interrupts.h"
#include "cokernel/drivers.h"
#include "nodefs/nodefs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_COMMAND_LEN 4096

typedef struct EnvVar {
    char *key;
    char *value;
    struct EnvVar *next;
} EnvVar;

static EnvVar *g_env_vars = NULL;

//static pid_t g_video_audio_pid = -1;
static const char *get_fs_root(void) {
    static char root[PATH_MAX] = {0};
    if (root[0]) return root;

    char exec_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
    if (len < 0) {
        getcwd(root, sizeof(root));
        return root;
    }
    exec_path[len] = '\0';

    strcpy(root, exec_path);
    char *slash = strrchr(root, '/');
    if (slash) *slash = '\0';

    char *last_slash = strrchr(root, '/');
    if (last_slash && strcmp(last_slash + 1, "bin") == 0) {
        *last_slash = '\0';
    }

    if (root[0] == '\0') root[0] = '/';
    return root;
}


static const char *get_projects_file(void) {
    static char path[PATH_MAX] = {0};
    if (!path[0]) snprintf(path, sizeof(path), "%s/data/projects.json", get_fs_root());
    return path;
}

static const char *get_module_dir(void) {
    static char dir[PATH_MAX] = {0};
    if (!dir[0]) snprintf(dir, sizeof(dir), "%s/modules", get_fs_root());
    return dir;
}

static const char *get_tools_dir(void) {
    static char dir[PATH_MAX] = {0};
    if (!dir[0]) snprintf(dir, sizeof(dir), "%s/tools", get_fs_root());
    return dir;
}

static const char *get_process_file(void) {
    static char path[PATH_MAX] = {0};
    if (!path[0]) snprintf(path, sizeof(path), "%s/data/processdata.json", get_fs_root());
    return path;
}

#define MAXLINE 8192
#define BUF_SIZE 4096
#define MEMMGR_BIN_FORMAT "%s/cortez_memmgr"
#define MEMMGR_IMG_FORMAT "%s/data.img"
#define PM_BIN_FORMAT "%s/cortez_pm"


/* defaults (change if you want other defaults) */
#define MEMMGR_DEFAULT_IMG_SIZE_MB 1024
#define MEMMGR_DEFAULT_MEM_MB      512
#define MEMMGR_DEFAULT_SWAP_MB     1024


static char **build_memmgr_wrapped_argv(const char *tools_dir,
                                        const char *prog_path, char **prog_args, int prog_argc,
                                        size_t img_size_mb, size_t mem_mb, size_t swap_mb)
{
    char memmgr_bin[PATH_MAX];
    char memmgr_img[PATH_MAX];
    snprintf(memmgr_bin, sizeof(memmgr_bin), "%s/cortez_memmgr", tools_dir);
    snprintf(memmgr_img, sizeof(memmgr_img), "%s/data.img", tools_dir);
    int extra = 12;
    int total = 1 + 2 + 2 + 2 + 2 + 1 + prog_argc + extra; /* safe upper bound */
    char **out = calloc(total, sizeof(char*));
    if (!out) return NULL;
    int idx = 0;

    out[idx++] = strdup(memmgr_bin);
    out[idx++] = strdup("--img-path");
    out[idx++] = strdup(memmgr_img);

    out[idx++] = strdup("--img-size-mb");
    char tmp[64]; snprintf(tmp, sizeof(tmp), "%zu", img_size_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--mem-mb");
    snprintf(tmp, sizeof(tmp), "%zu", mem_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--swap-mb");
    snprintf(tmp, sizeof(tmp), "%zu", swap_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--");

    /* append program + its args */
    if (prog_path) out[idx++] = strdup(prog_path);
    for (int i = 0; i < prog_argc; ++i) {
        if (prog_args && prog_args[i]) out[idx++] = strdup(prog_args[i]);
    }

    out[idx] = NULL;
    return out;
}

static char **build_pm_wrapped_argv(const char *tools_dir,
                                    const char *prog_path, char **prog_args, int prog_argc,
                                    size_t img_size_mb, size_t mem_mb, size_t swap_mb)
{
    char pm_bin[PATH_MAX];
    char memmgr_bin[PATH_MAX];
    char memmgr_img[PATH_MAX];

    snprintf(pm_bin, sizeof(pm_bin), "%s/cortez_pm", tools_dir);
    snprintf(memmgr_bin, sizeof(memmgr_bin), "%s/cortez_memmgr", tools_dir);
    snprintf(memmgr_img, sizeof(memmgr_img), "%s/data.img", tools_dir);

    /* estimate size: pm + start + memmgr + args... + prog + prog_args + NULL */
    int extra = 16;
    int total = 1 + 1 + 1 + 6 + prog_argc + extra;
    char **out = calloc(total, sizeof(char*));
    if (!out) return NULL;
    int idx = 0;

    out[idx++] = strdup(pm_bin);         /* cortez_pm */
    out[idx++] = strdup("start");        /* subcommand */
    out[idx++] = strdup(memmgr_bin);     /* pass memmgr path so pm execs it */

    out[idx++] = strdup("--img-path");
    out[idx++] = strdup(memmgr_img);

    out[idx++] = strdup("--img-size-mb");
    char tmp[64]; snprintf(tmp, sizeof(tmp), "%zu", img_size_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--mem-mb");
    snprintf(tmp, sizeof(tmp), "%zu", mem_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--swap-mb");
    snprintf(tmp, sizeof(tmp), "%zu", swap_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--"); /* end of memmgr args forwarded by pm */

    /* append program + args */
    if (prog_path) out[idx++] = strdup(prog_path);
    for (int i = 0; i < prog_argc; ++i) {
        if (prog_args && prog_args[i]) out[idx++] = strdup(prog_args[i]);
    }

    out[idx] = NULL;
    return out;
}



/* --- base64 table & encoder --- */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *data, size_t in_len, size_t *out_len) {
    if (!data) return NULL;
    size_t olen = 4 * ((in_len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i < in_len) {
        unsigned int a = data[i++];
        unsigned int b = i < in_len ? data[i++] : 0;
        unsigned int c = i < in_len ? data[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[o++] = b64_table[(triple >> 18) & 0x3F];
        out[o++] = b64_table[(triple >> 12) & 0x3F];
        out[o++] = (i - 1 <= in_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[o++] = (i <= in_len) ? b64_table[triple & 0x3F] : '=';
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

/* --- emit helpers --- */
static void emitf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ck_syscall(CK_SYS_PRINT, buf, NULL, NULL);
    fprintf(stdout, "%s\n", buf);
    fflush(stdout);
}
static void emit_ok(const char *msg) { emitf("OK %s", msg ? msg : ""); }
static void emit_err(const char *msg) { emitf("ERR %s", msg ? msg : ""); }
static void emit_out(const char *msg) { emitf("OUT %s", msg ? msg : ""); }

/* --- simple tokenizer (supports double quotes like the Python version) --- */
static char **tokenize(const char *line, int *tokc) {
    *tokc = 0;
    if (!line) return NULL;
    char *buf = strdup(line);
    if (!buf) return NULL;
    char **arr = NULL;
    int cap = 0, cnt = 0;
    char *p = buf;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (!*p) break;
        char *start;
        int quoted = 0;
        if (*p == '"') { quoted = 1; p++; start = p; }
        else { start = p; }
        char *tok = NULL;
        if (quoted) {
            char *q = start;
            while (*q && *q != '"') {
                if (*q == '\\' && *(q+1)) q++; /* allow backslash escaping */
                q++;
            }
            size_t len = q - start;
            tok = malloc(len + 1);
            if (!tok) break;
            strncpy(tok, start, len);
            tok[len] = '\0';
            p = (*q == '"') ? q + 1 : q;
        } else {
            char *q = start;
            while (*q && *q != ' ' && *q != '\t') q++;
            size_t len = q - start;
            tok = malloc(len + 1);
            if (!tok) break;
            strncpy(tok, start, len);
            tok[len] = '\0';
            p = q;
        }
        if (cnt + 1 >= cap) {
            cap = cap ? cap * 2 : 8;
            arr = realloc(arr, cap * sizeof(char*));
        }
        arr[cnt++] = tok;
    }
    free(buf);
    if (!arr) { *tokc = 0; return NULL; }
    arr[cnt] = NULL;
    *tokc = cnt;
    return arr;
}
static void free_tokens(char **toks, int c) {
    if (!toks) return;
    for (int i = 0; i < c; ++i) free(toks[i]);
    free(toks);
}

/* --- safe file read / write --- */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}


/* --- helper: find registered process path by name from PROCESS_FILE
 *   Returns 0 on success and writes path into out (null-terminated).
 *   Returns -1 if not found or on error.
 */
static int get_registered_process_path(const char *name, char *out, size_t outcap)
{
    char *s = read_whole_file(get_process_file());
    if (!s) return -1;

    /* naive search similar to project parsing used elsewhere:
       find the name, then find the "path" value in the same object */
    char *p = strstr(s, name);
    if (!p) { free(s); return -1; }

    char *path_key = strstr(p, "\"path\"");
    if (!path_key) { free(s); return -1; }

    /* find the first quote that begins the path value */
    char *q = strchr(path_key, '"');
    if (!q) { free(s); return -1; }
    q = strchr(q + 1, '"'); if (!q) { free(s); return -1; }
    q = strchr(q + 1, '"'); if (!q) { free(s); return -1; }
    char *q2 = strchr(q + 1, '"'); if (!q2) { free(s); return -1; }

    size_t len = q2 - (q + 1);
    if (len >= outcap) { free(s); return -1; }
    strncpy(out, q + 1, len);
    out[len] = '\0';

    free(s);
    return 0;
}

static int write_whole_file(const char *path, const char *data) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t len = strlen(data);
    if (fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fflush(f); fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* forward declarations */
static int path_is_executable(const char *path);
static void free_argv_array(char **argv);

/* ----- proc (process manager) helpers ----- */

int is_numeric(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    strtol(s, &end, 10);    // allows + or - prefixes
    return *end == '\0';
}

/* reader thread: forwards cortez_pm stdout/stderr lines to emit_out */
struct pm_reader_args {
    int fd;
    pid_t child;
};

static void *pm_reader_thread(void *v)
{
    struct pm_reader_args *a = (struct pm_reader_args*)v;
    int fd = a->fd;
    (void)a->child; /* reserved if you want to use child pid later */
    free(a);

    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return NULL;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t r;
    while ((r = getline(&line, &len, f)) != -1) {
        /* strip newline */
        while (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) line[--r] = '\0';
        if (r == 0) continue;
        emit_out(line);
    }
    free(line);
    fclose(f);
    return NULL;
}

static const char *get_bin_dir(void);
/* proc --list: run tools/cortez_pm list and send output back */
static void cmd_proc_list(void)
{
    char pm_path[PATH_MAX];
    snprintf(pm_path, sizeof(pm_path), "%s/cortez_pm", get_tools_dir());

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "%s list 2>&1", pm_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) { emit_err("proc list failed"); return; }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* strip newline */
        size_t L = strlen(line);
        while (L && (line[L-1]=='\n' || line[L-1]=='\r')) line[--L] = '\0';
        emit_out(line);
    }
    pclose(fp);
    emit_ok("proc list");
}

/* proc --kill <pid-or-jobid> : numeric => kill(pid), otherwise forward to cortez_pm stop <jobid> */
static void cmd_proc_kill(const char *arg)
{
    if (!arg) { emit_err("proc kill needs argument"); return; }

    if (is_numeric(arg)) {
        int pid = atoi(arg);
        if (pid <= 0) { emit_err("invalid pid"); return; }
        if (kill(pid, SIGTERM) == 0) {
            emit_ok("killed");
        } else {
            emit_err(strerror(errno));
        }
        return;
    }

    char pm_path[PATH_MAX];
    snprintf(pm_path, sizeof(pm_path), "%s/cortez_pm", get_tools_dir());

    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "%s stop %s 2>&1", pm_path, arg);
    int rc = system(cmd);
    if (rc == 0) emit_ok("proc stop");
    else emit_err("proc stop failed");
}

//ls - behaves exactly like ls but standalone
static void cmd_ls(void) {
    DIR *d = opendir(".");
    if (!d) {
        emit_err("cannot open current directory");
        return;
    }

    struct dirent *ent;
    char path[PATH_MAX];
    char output_line[PATH_MAX + 10];
    struct stat st;

    while ((ent = readdir(d)) != NULL) {
        /* Filter out . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        snprintf(path, sizeof(path), "./%s", ent->d_name);
        if (lstat(path, &st) == 0) {
            snprintf(output_line, sizeof(output_line), "%s", ent->d_name);
            if (S_ISDIR(st.st_mode)) {
                strcat(output_line, "/");
            } else if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR || st.st_mode & S_IXGRP || st.st_mode & S_IXOTH)) {
                strcat(output_line, "*");
            }
            emit_out(output_line);
        }
    }
    closedir(d);
    
    emit_ok("ls");
}

/* proc --start behavior:
   - "proc --start <name> <path>"  => register (add/update) name->path in PROCESS_FILE
   - "proc --start <name>"         => start the registered process (via cortez_pm)
*/
static void cmd_proc_start(int argc, char **argv)
{
    /* argc is the count of tokens after "--start" (as used in your caller) */
    if (argc < 1) {
        emit_err("proc start usage: proc --start <name> [<path>]");
        return;
    }

    const char *name = argv[0];

    /* If user supplied a path (argc >= 2) -> register / update */
    if (argc >= 2) {
        const char *path = argv[1];

        /* Read existing file (if any) */
        char *s = read_whole_file(get_process_file());
        if (!s) {
            /* create new file with single entry */
            char tmp[65536];
            snprintf(tmp, sizeof(tmp),
                     "{\n  \"%s\": { \"path\": \"%s\" }\n}\n",
                     name, path);
            if (write_whole_file(get_process_file(), tmp) == 0) emit_ok("proc registered");
            else emit_err("write failed");
            return;
        }

        /* sanitize trailing spaces/newlines */
        size_t len = strlen(s);
        while (len && (s[len-1]=='\n' || s[len-1]==' ' || s[len-1]=='\t' || s[len-1]=='\r')) { s[--len]=0; }

        char out[131072]; /* large buffer for resulting file */
        if (strcmp(s, "{}") == 0 || strcmp(s, "{ }") == 0) {
            snprintf(out, sizeof(out),
                     "{\n  \"%s\": { \"path\": \"%s\" }\n}\n",
                     name, path);
        } else {
            /* If name already present, do a very small update: locate name and replace path.
               Simpler approach: if name exists, return an error to avoid complicated in-place replace.
               But we will prefer to append if not present. */
            if (strstr(s, name) != NULL) {
                /* naive: replace path for existing entry is tricky; inform user to remove then add instead
                   (keeps implementation simple and robust). */
                free(s);
                emit_err("name already exists; remove and re-add to change path");
                return;
            }

            /* append new entry before the final '}' */
            char *pos = strrchr(s, '}');
            if (!pos) { free(s); emit_err("malformed processdata.json"); return; }
            size_t pre_len = pos - s;
            if (pre_len + 4096 > sizeof(out)) { free(s); emit_err("processdata.json too large"); return; }
            strncpy(out, s, pre_len);
            out[pre_len] = 0;
            /* add trailing comma and new entry */
            strcat(out, ",\n  \"");
            strcat(out, name);
            strcat(out, "\": { \"path\": \"");
            strcat(out, path);
            strcat(out, "\" }\n}\n");
        }

        if (write_whole_file(get_process_file(), out) == 0) emit_ok("proc registered");
        else emit_err("write failed");

        free(s);
        return;
    }

    /* Otherwise argc == 1 => start the named registered process */
    char exe_path[PATH_MAX];
    if (get_registered_process_path(name, exe_path, sizeof(exe_path)) != 0) {
        emit_err("process not found; add with: proc --start <name> <path>");
        return;
    }

    /* verify executable */
    if (!path_is_executable(exe_path)) {
        emit_err("executable not found or not executable");
        return;
    }

    /* Build cortez_pm invocation: pm_path start memmgr_path --img-path <img> --img-size-mb N --mem-mb M --swap-mb S -- <exe> */
    char pm_path[PATH_MAX];
    char memmgr_path[PATH_MAX];
    char imgpath[PATH_MAX];
    snprintf(pm_path, sizeof(pm_path), "%s/cortez_pm", get_process_file());
    snprintf(memmgr_path, sizeof(memmgr_path), "%s/cortez_memmgr", get_tools_dir());
    snprintf(imgpath, sizeof(imgpath), "%s/data.img", get_tools_dir);

    int reserve = 32 + 2; /* small fixed reserve (no extra runtime args in this mode) */
    char **pargv = calloc(reserve, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    int idx = 0;
    pargv[idx++] = strdup(pm_path);
    pargv[idx++] = strdup("start");
    pargv[idx++] = strdup(memmgr_path);

    pargv[idx++] = strdup("--img-path");
    pargv[idx++] = strdup(imgpath);

    pargv[idx++] = strdup("--img-size-mb");
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%zu", (size_t)MEMMGR_DEFAULT_IMG_SIZE_MB);
    pargv[idx++] = strdup(tmp);

    pargv[idx++] = strdup("--mem-mb");
    snprintf(tmp, sizeof(tmp), "%zu", (size_t)MEMMGR_DEFAULT_MEM_MB);
    pargv[idx++] = strdup(tmp);

    pargv[idx++] = strdup("--swap-mb");
    snprintf(tmp, sizeof(tmp), "%zu", (size_t)MEMMGR_DEFAULT_SWAP_MB);
    pargv[idx++] = strdup(tmp);

    pargv[idx++] = strdup("--");

    /* program to run (the registered executable) */
    pargv[idx++] = strdup(exe_path);
    pargv[idx] = NULL;

    /* Create a pipe to capture cortez_pm stdout/stderr and fork the pm runner (same flow as before) */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free_argv_array(pargv);
        emit_err("pipe failed");
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]); close(pipefd[1]);
        free_argv_array(pargv);
        emit_err("fork failed");
        return;
    }

    if (child == 0) {
        /* child: redirect stdout+stderr -> pipe write and exec cortez_pm */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(pargv[0], pargv);
        _exit(127);
    }

    /* parent */
    close(pipefd[1]); /* keep pipefd[0] to read from cortez_pm */

    /* spawn reader thread to forward cortez_pm output to backend frontend pipeline */
    struct pm_reader_args *ra = malloc(sizeof(*ra));
    if (!ra) {
        close(pipefd[0]);
        emit_err("alloc failed");
        free_argv_array(pargv);
        return;
    }
    ra->fd = pipefd[0];
    ra->child = child;

    pthread_t thr;
    if (pthread_create(&thr, NULL, pm_reader_thread, ra) != 0) {
        close(pipefd[0]);
        free(ra);
        emit_err("pthread_create failed");
        /* let child run */
    } else {
        pthread_detach(thr);
    }

    free_argv_array(pargv);

    emitf("OK proc start requested pid=%d name=%s", (int)child, name);
}



/* --- module/project helpers (simple operations matched to Python) --- */
static void cmd_module_list(void) {
    DIR *d = opendir(get_module_dir());
    if (!d) {
        emit_err("Cannot open module dir");
        return;
    }

    /* dynamic array of strings for unique module names */
    char **names = NULL;
    int ncap = 0, ncnt = 0;

    auto_add_name:
    /* helper to add unique name */
    ;
    /* - implemented below as a static inline block for clarity - */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *dname = ent->d_name;
        if (dname[0] == '.') continue; /* skip hidden entries */

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", get_module_dir(), dname);

        struct stat st;
        if (stat(path, &st) != 0) {
            /* can't stat — skip */
            continue;
        }

        /* candidate module name we might emit */
        char candidate[PATH_MAX];
        candidate[0] = 0;

        if (S_ISREG(st.st_mode)) {
            /* regular file: if ends with .c -> base name; else if executable -> name */
            size_t L = strlen(dname);
            if (L > 2 && strcmp(dname + L - 2, ".c") == 0) {
                /* strip .c */
                strncpy(candidate, dname, L - 2);
                candidate[L - 2] = 0;
            } else if (st.st_mode & S_IXUSR) {
                /* executable file */
                strncpy(candidate, dname, sizeof(candidate)-1);
                candidate[sizeof(candidate)-1] = 0;
            }
        } else if (S_ISDIR(st.st_mode)) {
            /* directory: consider it a module if it contains a same-named executable or a .c file or any executable */
            /* 1) check MODULE_DIR/dname/dname (executable) */
            char subpath[PATH_MAX];
            snprintf(subpath, sizeof(subpath), "%s/%s/%s", get_module_dir(), dname, dname);
            if (path_is_executable(subpath)) {
                strncpy(candidate, dname, sizeof(candidate)-1);
                candidate[sizeof(candidate)-1] = 0;
            } else {
                /* 2) scan directory for *.c or any executable file */
                DIR *sd = opendir(path);
                if (sd) {
                    struct dirent *s;
                    while ((s = readdir(sd)) != NULL) {
                        if (s->d_name[0] == '.') continue;
                        size_t SL = strlen(s->d_name);
                        char sp[PATH_MAX];
                        snprintf(sp, sizeof(sp), "%s/%s/%s", get_module_dir(), dname, s->d_name);
                        struct stat sst;
                        if (stat(sp, &sst) == 0) {
                            if (S_ISREG(sst.st_mode)) {
                                if (SL > 2 && strcmp(s->d_name + SL - 2, ".c") == 0) {
                                    strncpy(candidate, dname, sizeof(candidate)-1);
                                    candidate[sizeof(candidate)-1] = 0;
                                    break;
                                }
                                if (sst.st_mode & S_IXUSR) {
                                    strncpy(candidate, dname, sizeof(candidate)-1);
                                    candidate[sizeof(candidate)-1] = 0;
                                    break;
                                }
                            }
                        }
                    }
                    closedir(sd);
                }
            }
        }

        if (candidate[0] == 0) continue; /* nothing recognized as a module for this entry */

        /* add unique */
        int found = 0;
        for (int i = 0; i < ncnt; ++i) {
            if (strcmp(names[i], candidate) == 0) { found = 1; break; }
        }
        if (!found) {
            if (ncnt + 1 >= ncap) {
                ncap = ncap ? ncap * 2 : 32;
                names = realloc(names, ncap * sizeof(char*));
            }
            names[ncnt++] = strdup(candidate);
        }
    }
    closedir(d);

    if (ncnt == 0) {
        emit_out("(no modules)");
    } else {
        /* optional: sort names alphabetically for stable output */
        /* simple insertion sort for small lists */
        for (int i = 1; i < ncnt; ++i) {
            char *key = names[i];
            int j = i - 1;
            while (j >= 0 && strcmp(names[j], key) > 0) { names[j+1] = names[j]; j--; }
            names[j+1] = key;
        }
        for (int i = 0; i < ncnt; ++i) {
            emit_out(names[i]);
            free(names[i]);
        }
    }
    free(names);

    emit_ok("module list done");
}

static void cmd_ksay(int argc, char **argv) {
    if (argc < 1) {
        emit_err("Usage: Ksay <message>");
        return;
    }

    char buf[4096] = {0};
    for (int i = 0; i < argc; ++i) {
        strncat(buf, argv[i], sizeof(buf) - strlen(buf) - 2);
        if (i < argc - 1) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
    }

    ck_syscall(CK_SYS_PRINT, buf, NULL, NULL);
    emit_ok("Ksay sent to kernel");
}

static void cmd_n_ls(void) {
    nodefs_list_dir(nodefs_get_current_node());
    emit_ok("n-ls");
}

static void cmd_n_create(const char* name, const char* type_str, const char* ext) {
    if (!name || !type_str) {
        emit_err("Usage: n-create <name> <type: FILE|DIR|LINK> [ext]");
        return;
    }
    NodeType type = NODE_TYPE_FILE;
    if (strcmp(type_str, "LINK") == 0) type = NODE_TYPE_LINK;
    if (strcmp(type_str, "DIR") == 0) type = NODE_TYPE_DIR;
    
    char final_name[64];
    if (ext && strlen(ext) > 0) {
        snprintf(final_name, sizeof(final_name), "%s.%s", name, ext);
    } else {
        strncpy(final_name, name, sizeof(final_name)-1);
        final_name[sizeof(final_name)-1] = '\0';
    }
    
    int id = nodefs_create_node(nodefs_get_current_node(), final_name, type);
    if (id < 0) {
        emit_err("create failed");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Created node [%d] %s", id, final_name);
        emit_ok(msg);
    }
}

static void cmd_n_link(const char* id_str, const char* name) {
    if (!id_str || !name) {
        emit_err("Usage: n-link <target_id> <name>");
        return;
    }
    int target_id = atoi(id_str);
    if (nodefs_link(nodefs_get_current_node(), target_id, name) == 0) {
        emit_ok("Node linked");
    } else {
        emit_err("Failed to link node");
    }
}

static void cmd_n_cd(const char* id_str) {
    if (!id_str) {
        emit_err("Usage: n-cd <target_id>");
        return;
    }
    int target_id = atoi(id_str);
    // In real impl, verify link exists first
    if (nodefs_set_current_node(target_id) == 0) {
        emit_ok("Changed current node");
    } else {
        emit_err("Failed to change node (Not a directory?)");
    }
}

static void cmd_n_cedit(int argc, char** argv) {
    if (argc < 2) {
        emit_err("Usage: n-cedit <id> <content...>");
        return;
    }
    int id = atoi(argv[0]);
    
    // Join remaining args
    char content[1024] = "";
    for (int i = 1; i < argc; ++i) {
        strcat(content, argv[i]);
        if (i < argc - 1) strcat(content, " ");
    }
    
    if (nodefs_write_data(id, content, strlen(content)) == 0) {
        emit_ok("Node updated");
    } else {
        emit_err("Failed to write data");
    }
}

static void cmd_n_read(const char* id_str) {
    if (!id_str) {
        emit_err("Usage: n-read <node_id>");
        return;
    }
    int id = atoi(id_str);
    char buf[1024];
    int len = nodefs_read_data(id, 0, buf, sizeof(buf)-1);
    if (len >= 0) {
        buf[len] = '\0';
        emit_out(buf);
    } else {
        emit_err("Failed to read node");
    }
}

static void cmd_n_unlink(const char* id_str) {
    if (!id_str) {
        emit_err("Usage: n-unlink <node_id>");
        return;
    }
    int id = atoi(id_str);
    if (nodefs_unlink_node(nodefs_get_current_node(), id) == 0) {
        emit_ok("Link removed");
    } else {
        emit_err("Failed to unlink node");
    }
}

static void cmd_n_delete(const char* id_str) {
    if (!id_str) {
        emit_err("Usage: n-delete <node_id>");
        return;
    }
    int id = atoi(id_str);
    if (nodefs_delete_node(id) == 0) {
        emit_ok("Node deleted");
    } else {
        emit_err("Failed to delete node");
    }
}

static void cmd_nedit_cli(const char* id_str) {
    if (!id_str) {
        emit_err("Usage: nedit-cli <node_id>");
        return;
    }
    int id = atoi(id_str);
    nedit_run(id);
}

static void cmd_n_write(int argc, char** argv) {
    if (argc < 2) { emit_err("Usage: n-write <id> <size>"); return; }
    int id = atoi(argv[0]);
    int size = atoi(argv[1]);
    
    char* buf = malloc(size);
    if (!buf) { emit_err("Memory allocation failed"); return; }
    
    // Read exactly size bytes from stdin
    // Since we are in a line-based loop, we need to be careful.
    // However, the frontend will send "n-write ID SIZE\nDATA"
    // The fgets in main loop consumes the newline after SIZE.
    // So we can read raw bytes now.
    
    int total = 0;
    while (total < size) {
        int r = fread(buf + total, 1, size - total, stdin);
        if (r <= 0) break;
        total += r;
    }
    
    if (nodefs_write_data(id, buf, total) == 0) {
        emit_ok("Node updated");
    } else {
        emit_err("Failed to write data");
    }
    free(buf);
}

static void cmd_gnedit(const char* id_str) {
    if (!id_str) { emit_err("Usage: gnedit <id>"); return; }
    int id = atoi(id_str);
    
    // Read node content
    // We need to send it to frontend.
    // Protocol:
    // CMD_EDIT_NODE <id>
    // <content lines...>
    // CMD_EDIT_END
    
    char* buf = malloc(1024 * 1024); // 1MB limit for now
    int len = nodefs_read_data(id, 0, buf, 1024 * 1024);
    if (len < 0) len = 0;
    
    printf("CMD_EDIT_NODE %d\n", id);
    if (len > 0) {
        // We need to ensure content doesn't contain our protocol markers?
        // Or just write it.
        // fwrite(buf, 1, len, stdout);
        // But we need to ensure it ends with newline if we want line-based parsing in frontend?
        // Actually, frontend reads lines.
        // So we should split by newline and print.
        char* ptr = buf;
        char* end = buf + len;
        char* line_start = ptr;
        while (ptr < end) {
            if (*ptr == '\n') {
                *ptr = 0;
                printf("%s\n", line_start);
                line_start = ptr + 1;
            }
            ptr++;
        }
        if (line_start < end) {
            // Print last chunk
            char tmp[1024];
            int chunk_len = end - line_start;
            if (chunk_len > 1023) chunk_len = 1023;
            memcpy(tmp, line_start, chunk_len);
            tmp[chunk_len] = 0;
            printf("%s\n", tmp);
        }
    }
    printf("CMD_EDIT_END\n");
    fflush(stdout);
    free(buf);
}


static void cmd_module_build(const char *name) {
    char src[PATH_MAX], out[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s.c", get_module_dir(), name);
    snprintf(out, sizeof(out), "%s/%s", get_module_dir(), name);
    struct stat st;
    if (stat(src, &st) != 0) { emit_err("source not found"); return; }
    pid_t p = fork();
    if (p == 0) {
        execlp("gcc", "gcc", "-Wall", "-O2", "-o", out, src, (char*)NULL);
        _exit(127);
    } else if (p > 0) {
        int status=0; waitpid(p, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) emit_ok("build succeeded");
        else emit_err("gcc failed");
    } else emit_err("fork failed");
}

static void cmd_module_add(const char *name, const char *srcpath) {
    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/%s", get_module_dir(), name);
    FILE *in = fopen(srcpath, "rb");
    if (!in) { emit_err("source not found"); return; }
    FILE *out = fopen(dest, "wb");
    if (!out) { fclose(in); emit_err("cannot create dest"); return; }
    char buf[4096]; size_t r;
    while ((r = fread(buf,1,sizeof(buf),in)) > 0) fwrite(buf,1,r,out);
    fclose(in); fclose(out);
    chmod(dest, 0755);
    emit_ok("integrated");
}

static void cmd_module_remove(const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", get_module_dir(), name);
    if (unlink(path) == 0) emit_ok("removed"); else emit_err("remove failed");
}

static void cmd_project_list(void) {
    char *s = read_whole_file(get_projects_file());
    if (!s) { emit_out("(no projects.json)"); emit_ok("done"); return; }
    emit_out(s); free(s); emit_ok("done");
}




static void cmd_project_add(const char *name, const char *path) {
    char *s = read_whole_file(get_projects_file());
    if (!s) {
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "{\n  \"%s\": { \"path\": \"%s\" }\n}\n", name, path);
        if (write_whole_file(get_projects_file(), tmp) == 0) emit_ok("added"); else emit_err("write failed");
        return;
    }
    size_t len = strlen(s);
    while (len && (s[len-1]=='\n' || s[len-1]==' ' || s[len-1]=='\t' || s[len-1]=='\r')) { s[len-1]=0; len--; }
    if (len == 0) { free(s); cmd_project_add(name, path); return; }
    char out[65536];
    if (strcmp(s, "{}")==0 || strcmp(s, "{ }")==0) {
        snprintf(out, sizeof(out), "{\n  \"%s\": { \"path\": \"%s\" }\n}\n", name, path);
    } else {
        char *pos = strrchr(s, '}');
        if (!pos) { free(s); emit_err("malformed projects.json"); return; }
        size_t pre_len = pos - s;
        if (pre_len + 1024 > sizeof(out)) { free(s); emit_err("too large"); return; }
        strncpy(out, s, pre_len); out[pre_len]=0;
        strcat(out, ",\n  \""); strcat(out, name); strcat(out, "\": { \"path\": \""); strcat(out, path); strcat(out, "\" }\n}\n");
    }
    if (write_whole_file(get_projects_file(), out) == 0) emit_ok("added"); else emit_err("write failed");
    free(s);
}

static void cmd_project_remove(const char *name) {
    char *s = read_whole_file(get_projects_file());
    if (!s) { emit_err("no projects.json"); return; }
    char *p = strstr(s, name);
    if (!p) { free(s); emit_err("not found"); return; }
    char *q = p;
    while (q > s && *q != '"') q--;
    if (q == s) { free(s); emit_err("malformed"); return; }
    char *colon = strstr(p, ":");
    if (!colon) { free(s); emit_err("malformed"); return; }
    char *brace = strchr(colon, '}');
    if (!brace) { free(s); emit_err("malformed"); return; }
    char *after = brace + 1;
    if (*after == ',') after++;
    size_t prefix = q - s;
    char newbuf[65536];
    if (prefix + strlen(after) + 1 > sizeof(newbuf)) { free(s); emit_err("too large"); return; }
    strncpy(newbuf, s, prefix); newbuf[prefix]=0; strcat(newbuf, after);
    if (write_whole_file(get_projects_file(), newbuf) == 0) emit_ok("removed"); else emit_err("write failed");
    free(s);
}

/* --- THREAD-BASED PTY STREAMING --- */
static pthread_t stream_thread;
static int stream_running = 0;
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t stream_child_pid = -1;
static int stream_master_fd = -1;

struct stream_args {
    char **argv; /* NULL-terminated */
};

static void free_argv_array(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; ++i) free(argv[i]);
    free(argv);
}

static void *stream_thread_func(void *v) {
    struct stream_args *sa = (struct stream_args*)v;
    char **argv = sa->argv;
    free(sa);

    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    if (pid < 0) {
        emit_err("forkpty failed");
        free_argv_array(argv);
        pthread_mutex_lock(&stream_lock); stream_running = 0; stream_child_pid = -1; stream_master_fd = -1; pthread_mutex_unlock(&stream_lock);
        return NULL;
    }

    if (pid == 0) {
        /* child */
        execvp(argv[0], argv);
        _exit(127);
    }

    /* parent: record running pid/fd */
    pthread_mutex_lock(&stream_lock);
    stream_running = 1;
    stream_child_pid = pid;
    stream_master_fd = master_fd;
    pthread_mutex_unlock(&stream_lock);

    emitf("STREAM_START %d", (int)pid);

    unsigned char buf[BUF_SIZE];
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds); FD_SET(master_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int rv = select(master_fd + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(master_fd, &rfds)) {
            ssize_t r = read(master_fd, buf, sizeof(buf));
            if (r <= 0) break;
            size_t outlen = 0;
            char *b64 = b64_encode(buf, (size_t)r, &outlen);
            if (b64) {
                emitf("STREAM_DATA %s", b64);
                free(b64);
            }
        }
        int status;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            int code = 0;
            if (WIFEXITED(status)) code = WEXITSTATUS(status);
            emitf("STREAM_END %d", code);
            break;
        }
    }

    /* ensure child reaped */
    int status;
    waitpid(pid, &status, 0);

    close(master_fd);

    pthread_mutex_lock(&stream_lock);
    stream_running = 0;
    stream_child_pid = -1;
    stream_master_fd = -1;
    pthread_mutex_unlock(&stream_lock);

    free_argv_array(argv);
    return NULL;
}

/* helper starts stream thread for given NULL-terminated argv array (ownership transferred) */
static int start_stream_thread(char **argv) {
    pthread_mutex_lock(&stream_lock);
    if (stream_running) {
        pthread_mutex_unlock(&stream_lock);
        return -1; /* already running */
    }
    stream_running = 1; stream_child_pid = -1; stream_master_fd = -1;
    pthread_mutex_unlock(&stream_lock);

    struct stream_args *sa = calloc(1, sizeof(*sa));
    if (!sa) { pthread_mutex_lock(&stream_lock); stream_running = 0; pthread_mutex_unlock(&stream_lock); return -1; }
    sa->argv = argv;
    if (pthread_create(&stream_thread, NULL, stream_thread_func, sa) != 0) {
        free(sa);
        pthread_mutex_lock(&stream_lock); stream_running = 0; pthread_mutex_unlock(&stream_lock);
        return -1;
    }
    pthread_detach(stream_thread);
    return 0;
}

/* send a signal to the running PTY child (if any) */
static void send_signal_to_stream_child(int sig) {
    pthread_mutex_lock(&stream_lock);
    pid_t pid = stream_child_pid;
    pthread_mutex_unlock(&stream_lock);
    if (pid <= 0) return;

    /* prefer signalling the process group of the child */
    pid_t pg = getpgid(pid);
    if (pg > 0) {
        if (kill(-pg, sig) != 0) {
            /* fallback: send to single pid */
            kill(pid, sig);
        }
    } else {
        /* fallback */
        kill(pid, sig);
    }
}

/* --- write into running stream's PTY (thread-safe) --- */
static int send_input_to_stream(const char *data, size_t len, int add_newline) {
    if (!data) return -1;
    pthread_mutex_lock(&stream_lock);
    int fd = stream_master_fd;
    int running = stream_running && fd >= 0;
    pthread_mutex_unlock(&stream_lock);
    if (!running) return -1;

    ssize_t written = 0;
    const char *ptr = data;
    size_t towrite = len;

    while (towrite > 0) {
        ssize_t w = write(fd, ptr + written, towrite);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += w;
        towrite -= w;
    }

    if (add_newline) {
        const char nl = '\n';
        ssize_t w = write(fd, &nl, 1);
        if (w < 0 && errno != EINTR) return -1;
    }
    return 0;
}

/* --- helpers to check/prepare executable paths --- */
static int path_is_executable(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    if (access(path, X_OK) == 0) return 1;
    return 0;
}

// Add this new function definition somewhere before main()
/* recursive removal helper */
static int rm_recursive_internal(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    /* if it's a symlink or regular file or other non-dir, just unlink */
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) == 0) return 0;
        return -1;
    }

    /* directory: iterate */
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char child[PATH_MAX];
        if ((size_t)snprintf(child, sizeof(child), "%s/%s", path, name) >= sizeof(child)) {
            rc = -1;
            errno = ENAMETOOLONG;
            break;
        }

        if (rm_recursive_internal(child) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    if (rc != 0) return -1;

    /* now remove the directory itself */
    if (rmdir(path) != 0) return -1;
    return 0;
}

/* rm command: supports -r, -d, -pr
 * Now accepts argc/argv (like main) so dispatcher can pass flags + path.
 */
static void cmd_rm(int argc, char **argv)
{
    if (argc < 1) {
        emit_err("rm usage: rm [-r|-d|-pr] <path>");
        return;
    }

    int flag_r = 0, flag_d = 0, flag_pr = 0;
    char *target = NULL;

    /* parse flags: flags can be combined like -rd or separate */
    int i = 0;
    for (; i < argc; ++i) {
        char *tok = argv[i];
        if (!tok) continue;
        if (tok[0] != '-') {
            /* first non-flag token is the target */
            target = strdup(tok);
            ++i;
            break;
        }
        /* token starts with '-' -> parse flags */
        if (strcmp(tok, "-r") == 0) { flag_r = 1; continue; }
        if (strcmp(tok, "-d") == 0) { flag_d = 1; continue; }
        if (strcmp(tok, "-pr") == 0) { flag_pr = 1; continue; }
        /* combined short flags like -rd or -pr inside */
        for (size_t j = 1; tok[j]; ++j) {
            if (tok[j] == 'r') flag_r = 1;
            else if (tok[j] == 'd') flag_d = 1;
            else if (tok[j] == 'p' && tok[j+1] == 'r') { flag_pr = 1; break; }
        }
        /* continue scanning for target after flags */
    }

    /* if target not found yet, maybe it's the last arg */
    if (!target && i < argc) {
        if (argv[i]) target = strdup(argv[i]);
    }

    if (!target) {
        emit_err("rm missing path");
        return;
    }

    int res = -1;

    if (flag_pr) {
        /* remove file in parent directory: ../<target> */
        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof(path), "../%s", target) >= sizeof(path)) {
            emit_err("path too long");
            goto out;
        }
        if (unlink(path) == 0) { emit_out("Removed file: "); emit_out(path); emit_ok("rm"); res = 0; }
        else { emit_err(strerror(errno)); res = -1; }
        goto out;
    }

    if (flag_r) {
        /* recursive delete (file or directory) */
        if (rm_recursive_internal(target) == 0) {
            emit_out("Removed recursively: "); emit_out(target); emit_ok("rm"); res = 0;
        } else {
            emit_err(strerror(errno));
            res = -1;
        }
        goto out;
    }

    if (flag_d) {
        /* remove empty directory only */
        if (rmdir(target) == 0) { emit_out("Removed directory: "); emit_out(target); emit_ok("rm"); res = 0; }
        else { emit_err(strerror(errno)); res = -1; }
        goto out;
    }

    /* default: remove file only (fail on directory) */
    {
        struct stat st;
        if (stat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
            emit_err("target is a directory; use -r to remove recursively or -d to remove empty dir");
            res = -1;
            goto out;
        }
        if (unlink(target) == 0) {
            emit_out("Removed file: ");
            emit_out(target);
            emit_ok("rm");
            res = 0;
        } else {
            emit_err(strerror(errno));
            res = -1;
        }
    }

out:
    free(target);
    (void)res;
}


// Plays Audio
static void cmd_play_audio(const char *filepath) {
    if (!filepath) {
        emit_err("play usage: play <path/to/mediafile>");
        return;
    }

    char resolved_path[PATH_MAX];
    if (realpath(filepath, resolved_path) == NULL) {
        emit_err("File not found or path is invalid");
        return;
    }

    // Emit a command for the frontend to handle audio playback
    emitf("CMD_PLAY_AUDIO %s", resolved_path);

    // The frontend will handle the actual playback, so we just confirm the command was sent.
    char ok_msg[PATH_MAX + 64];
    snprintf(ok_msg, sizeof(ok_msg), "Playback request sent for %s", resolved_path);
    emit_ok(ok_msg);
}


static void cmd_create(const char *filename) {
    if (!filename || filename[0] == '\0') {
        emit_err("create usage: create <filename>");
        return;
    }

    int fd = open(filename, O_CREAT | O_WRONLY, 0644);
    if (fd == -1) {
        emit_err(strerror(errno));
        return;
    }
    close(fd);

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        emit_err("cannot get current directory");
        return;
    }

    char message[PATH_MAX + 100];
    snprintf(message, sizeof(message), "%s saved in %s", filename, cwd);
    emit_out(message);
    emit_ok("create");
}

/* --- run ckg tool (blocking, capture output) --- */
static void cmd_ckg_run(int argc, char **argv) {
    char ckg_bin[PATH_MAX];
    snprintf(ckg_bin, sizeof(ckg_bin), "%s/ckg", get_tools_dir());
    if (access(ckg_bin, F_OK) != 0) {
        emit_err("ckg not found; put compiled tools/ckg in tools/");
        return;
    }
    struct stat st;
    if (stat(ckg_bin, &st) == 0) {
        if (!(st.st_mode & S_IXUSR)) {
            chmod(ckg_bin, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        }
    }

    /* build command string (simple shell invocation) */
    size_t buf_sz = 4096;
    for (int i = 0; i < argc; ++i) buf_sz += strlen(argv[i]) + 4;
    char *cmd = calloc(1, buf_sz);
    if (!cmd) { emit_err("alloc failed"); return; }
    snprintf(cmd, buf_sz, "%s", ckg_bin);
    for (int i = 0; i < argc; ++i) {
        strncat(cmd, " ", buf_sz - strlen(cmd) - 1);
        /* naive quoting: wrap in single quotes and escape single quotes inside arg */
        strncat(cmd, "'", buf_sz - strlen(cmd) - 1);
        for (const char *p = argv[i]; *p; ++p) {
            if (*p == '\'') strncat(cmd, "'\\''", buf_sz - strlen(cmd) - 1);
            else {
                char tmp[2] = { *p, '\0' };
                strncat(cmd, tmp, buf_sz - strlen(cmd) - 1);
            }
        }
        strncat(cmd, "'", buf_sz - strlen(cmd) - 1);
    }

    /* set environment vars so ckg knows where tools/data are */
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof(data_dir), "%s/data", get_fs_root());
    setenv("CKG_TOOLS_DIR", get_tools_dir(), 1);
    setenv("CKG_DATA_DIR", data_dir, 1);
    /* optional: user may have configured CKG_SERVER in the environment already;
       if you want to force a backend-configured server address, set it here:
       setenv("CKG_SERVER", "127.0.0.1:9000", 1);
     */

    /* run and stream output */
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        emit_err("failed to run ckg");
        free(cmd);
        return;
    }
    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        /* strip newline(s) */
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = '\0';
        emit_out(line);
    }
    int rc = pclose(fp);
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) emit_ok("ckg");
    else emit_err("ckg failed");
    free(cmd);
}


/* --- run net tool under PTY --- */
static void cmd_net_run(int argc, char **argv) {
    char net_tool[PATH_MAX];
    snprintf(net_tool, sizeof(net_tool), "%s/net-twerk", get_tools_dir());
    if (access(net_tool, F_OK) != 0) { emit_err("net tool not found; build tools/net-twerk.c"); return; }
    struct stat st;
    if (stat(net_tool, &st) == 0) {
        if (!(st.st_mode & S_IXUSR)) {
            chmod(net_tool, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        }
    }

    char **pargv = calloc(argc + 2, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    pargv[0] = strdup(net_tool);
    for (int i = 0; i < argc; ++i) pargv[i+1] = strdup(argv[i]);
    pargv[argc+1] = NULL;

    if (start_stream_thread(pargv) != 0) {
        free_argv_array(pargv);
        emit_err("another stream is running");
        return;
    }
    emit_ok("net started");
}

/* --- change directory (built-in) --- */
static void cmd_cd(const char *path) {
    const char *target = path;
    if (!target || target[0] == '\0') {
        const char *home = getenv("HOME");
        target = home ? home : ".";
    }
    if (chdir(target) == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            emit_out(cwd);    /* show new cwd */
            emit_ok("cd");
        } else {
            emit_ok("cd");
        }
    } else {
        emit_err(strerror(errno));
    }
}

// Add this new function definition somewhere before main()
static int make_path(char *path, int p_flag) {
    char *p, *slash;
    mode_t mode = 0755;
    int rc = 0;
    
    if (strcmp(path, "/") == 0 || strcmp(path, ".") == 0 || strcmp(path, "..") == 0) {
        errno = EEXIST;
        return -1;
    }

    if (p_flag) {
        p = strdup(path);
        if (!p) {
            emit_err("strdup failed");
            return -1;
        }
        slash = p;
        while ((slash = strchr(slash, '/')) != NULL) {
            if (slash != p) {
                *slash = '\0';
                if (mkdir(p, mode) != 0 && errno != EEXIST) {
                    emit_err(strerror(errno));
                    rc = -1;
                    break;
                }
                *slash = '/';
            }
            slash++;
        }
        free(p);
        if (rc != 0) return -1;
    }

    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        emit_err(strerror(errno));
        return -1;
    }
    
    return 0;
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 1) {
        emit_err("mkdir usage: mkdir [-p] <path>");
        return;
    }

    int p_flag = 1;
    int path_idx = 0;
    
    if (argc >= 2 && strcmp(argv[0], "-p") == 0) {
        p_flag = 1;
        path_idx = 1;
    }
    
    if (path_idx >= argc) {
        emit_err("mkdir usage: mkdir [-p] <path>");
        return;
    }

    if (make_path(argv[path_idx], p_flag) == 0) {
        emit_ok("directory created");
    }
}

/* --- run local net-runner (command name: netr) --- */
static void cmd_netr_run(int argc, char **argv) {
    char net_tool[PATH_MAX];

    /* prefer net-runner in the same directory as the backend executable */
    snprintf(net_tool, sizeof(net_tool), "%s/net-runner", get_bin_dir());
    if (access(net_tool, X_OK) != 0) {
        /* fallback to tools dir for compatibility */
        snprintf(net_tool, sizeof(net_tool), "%s/net-runner", get_tools_dir());
        if (access(net_tool, X_OK) != 0) {
            emit_err("net-runner not found in executable directory or tools/");
            return;
        }
    }

    /* ensure executable bit */
    struct stat st;
    if (stat(net_tool, &st) == 0) {
        if (!(st.st_mode & S_IXUSR)) {
            chmod(net_tool, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        }
    }

    /* build argv: net_tool + argv[0..argc-1] */
    char **pargv = calloc(argc + 2, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    pargv[0] = strdup(net_tool);
    for (int i = 0; i < argc; ++i) pargv[i+1] = strdup(argv[i]);
    pargv[argc+1] = NULL;

    if (start_stream_thread(pargv) != 0) {
        free_argv_array(pargv);
        emit_err("another stream is running");
        return;
    }
    emit_ok("netr started");
}

/* --- start cedit tool under PTY stream --- */
static void cmd_cedit(int argc, char **argv) {
    char cedit_tool[PATH_MAX];
    snprintf(cedit_tool, sizeof(cedit_tool), "%s/cedit", get_tools_dir());

    if (access(cedit_tool, X_OK) != 0) {
        emit_err("cedit not found in tools/");
        return;
    }

int cnt = argc + 1;
char **pargv = calloc(cnt + 1, sizeof(char*));
if (!pargv) { emit_err("alloc failed"); return; }
pargv[0] = strdup(cedit_tool);

/* get backend cwd once */
char cwd[PATH_MAX] = {0};
if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';

for (int i = 0; i < argc; ++i) {
    const char *a = argv[i];
    if (!a) {
        pargv[i+1] = strdup("");
        continue;
    }

    char resolved[PATH_MAX];
    /* absolute path or dot-relative or ~ expansion -> preserve/expand */
    if (a[0] == '/') {
        pargv[i+1] = strdup(a);
    } else if (a[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "";
        snprintf(resolved, sizeof(resolved), "%s%s", home, a + 1); /* skip ~ */
        pargv[i+1] = strdup(resolved);
    } else if (a[0] == '.' && (a[1] == '/' || (a[1]=='.' && a[2]=='/'))) {
        /* keep relative with dot segments — but make absolute by prefixing cwd */
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, a);
        pargv[i+1] = strdup(resolved);
    } else {
        /* plain relative filename -> make it absolute by prefixing backend cwd */
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, a);
        pargv[i+1] = strdup(resolved);
    }
}

pargv[cnt] = NULL;


    emit_ok("cedit started");
}


/* --- list directories (lsdir) in current directory --- */
static void cmd_lsdir(void) {
    DIR *d = opendir(".");
    if (!d) {
        emit_err("cannot open current directory");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* skip hidden by default */
        /* we want directories only */
        struct stat st;
        if (stat(ent->d_name, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                emit_out(ent->d_name);
            }
        }
    }
    closedir(d);
    emit_ok("lsdir");
}

/* --- start an interactive shell (or arbitrary command) in a PTY stream --- */
static void cmd_shell(int argc, char **argv) {
    char shell_tool[PATH_MAX];
    snprintf(shell_tool, sizeof(shell_tool), "%s/shell", get_tools_dir());

    if (access(shell_tool, X_OK) == 0) {
        /* Use tools/shell and pass through any user args.
           Build pargv: shell_tool [ user-args ... ] NULL
         */
        int cnt = argc + 1;
        char **pargv = calloc(cnt + 1, sizeof(char*));
        if (!pargv) { emit_err("alloc failed"); return; }
        pargv[0] = strdup(shell_tool);
        for (int i = 0; i < argc; ++i) pargv[i+1] = strdup(argv[i]);
        pargv[cnt] = NULL;
        if (start_stream_thread(pargv) != 0) {
            free_argv_array(pargv);
            emit_err("another stream is running");
            return;
        }
        emit_ok("shell started (tools/shell)");
        return;
    }

    /* fallback: if user provided explicit command, start it directly under PTY */
    if (argc >= 1 && argv && argv[0]) {
        char **pargv = calloc(argc + 1 + 1, sizeof(char*));
        if (!pargv) { emit_err("alloc failed"); return; }
        for (int i = 0; i < argc; ++i) pargv[i] = strdup(argv[i]);
        pargv[argc] = NULL;
        if (start_stream_thread(pargv) != 0) {
            free_argv_array(pargv);
            emit_err("another stream is running");
            return;
        }
        emit_ok("shell started");
        return;
    }

    /* No args: pick user's shell from env or fallback to /bin/sh and run it interactively */
    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";

    char **pargv = calloc(3, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    pargv[0] = strdup(shell);
    pargv[1] = strdup("-i");
    pargv[2] = NULL;

    if (start_stream_thread(pargv) != 0) {
        free_argv_array(pargv);
        emit_err("another stream is running");
        return;
    }
    emit_ok("shell started");
}


/* --- run tools/pwd and emit its stdout as OUT lines --- */
static void cmd_pwd(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        emit_out(cwd);
        emit_ok("pwd");
    } else {
        emit_err(strerror(errno));
    }
}


/* --- run module executable under PTY (given path) --- */
static void cmd_module_run_by_path(const char *path, int argc, char **argv) {
    /* If path not executable, bail */
    if (!path_is_executable(path)) { emit_err("not executable"); return; }

    /* wrap with process-manager which launches memmgr */
    char **wrapped = build_pm_wrapped_argv(get_tools_dir(), path, argv, argc,
                                           MEMMGR_DEFAULT_IMG_SIZE_MB,
                                           MEMMGR_DEFAULT_MEM_MB,
                                           MEMMGR_DEFAULT_SWAP_MB);
    if (!wrapped) { emit_err("alloc failed"); return; }

    if (start_stream_thread(wrapped) != 0) {
        /* start_stream_thread takes ownership on success; on failure we must free */
        free_argv_array(wrapped);
        emit_err("another stream is running or start failed");
        return;
    }
    emit_ok("module started (pm->vm)");
}




/* --- launch project by reading projects.json (naive parse like Python) --- */
static void cmd_project_launch(const char *name) {
    char *s = read_whole_file(get_projects_file());
    if (!s) { emit_err("no projects.json"); return; }
    char *p = strstr(s, name);
    if (!p) { free(s); emit_err("not found"); return; }
    char *path = strstr(p, "\"path\"");
    if (!path) { free(s); emit_err("no path entry"); return; }
    char *quote = strchr(path, '"');
    if (!quote) { free(s); emit_err("malformed"); return; }
    quote = strchr(quote+1, '"'); if (!quote) { free(s); emit_err("malformed"); return; }
    quote = strchr(quote+1, '"'); if (!quote) { free(s); emit_err("malformed"); return; }
    char *quote2 = strchr(quote+1, '"'); if (!quote2) { free(s); emit_err("malformed"); return; }
    size_t len = quote2 - (quote+1);
    char exe[PATH_MAX];
    if (len >= sizeof(exe)-1) { free(s); emit_err("path too long"); return; }
    strncpy(exe, quote+1, len); exe[len] = 0;
    free(s);

    /* If exe contains shell metacharacters, run with /bin/sh -c (still wrapped by memmgr) */
    if (strchr(exe, '|') || strchr(exe, '>') || strchr(exe, '<') || strchr(exe, '&')) {
        /* prepare prog argv: /bin/sh -c "<exe>" */
        char *prog_args_arr[2];
        prog_args_arr[0] = strdup("-c"); /* placeholder, will be provided in wrapper as separate strings */

        char **wrapped = build_pm_wrapped_argv(get_tools_dir(), "/bin/sh", (char**)NULL, 0,
                                               MEMMGR_DEFAULT_IMG_SIZE_MB,
                                               MEMMGR_DEFAULT_MEM_MB,
                                               MEMMGR_DEFAULT_SWAP_MB);

        if (!wrapped) { emit_err("alloc failed"); return; }

        /* we need to append "-c" and exe as additional args before NULL */
        /* compute current length */
        int cur = 0; while (wrapped[cur]) cur++;
        /* expand array */
        char **neww = realloc(wrapped, (cur + 3) * sizeof(char*));
        if (!neww) { free_argv_array(wrapped); emit_err("alloc failed"); return; }
        wrapped = neww;
        wrapped[cur++] = strdup("-c");
        wrapped[cur++] = strdup(exe);
        wrapped[cur] = NULL;

        if (start_stream_thread(wrapped) != 0) {
            free_argv_array(wrapped);
            emit_err("another stream is running");
            return;
        }
        emit_ok("project started (shell vm)");
        return;
    } else {
        if (!path_is_executable(exe)) { emit_err("executable not found or not executable"); return; }

        /* no shell meta chars: run exe directly, wrapped by memmgr */
        cmd_module_run_by_path(exe, 0, NULL);
        return;
    }
}

/* read command: read <filename> -- print file contents line-by-line */
static void cmd_read(const char *arg)
{
    if (!arg) {
        emit_err("read usage: read <filename>");
        return;
    }

    /* skip leading spaces */
    const char *p = arg;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') {
        emit_err("read usage: read <filename>");
        return;
    }

    /* check that target is not a directory */
    struct stat st;
    if (stat(p, &st) != 0) {
        emit_err(strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        emit_err("target is a directory");
        return;
    }

    FILE *f = fopen(p, "r");
    if (!f) {
        emit_err(strerror(errno));
        return;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    while ((nread = getline(&line, &len, f)) != -1) {
        /* strip trailing newline for cleaner output (optional) */
        if (nread > 0 && line[nread - 1] == '\n') line[nread - 1] = '\0';
        emit_out(line);
    }

    free(line);
    fclose(f);

    /* If there was an error other than EOF, report it */
    if (ferror(f)) {
        emit_err(strerror(errno));
        return;
    }

    emit_ok("read");
}



/* --- compile module helper used by commands that expect it --- */
static int compile_module_and_get_exe(const char *name, char *outpath, size_t outcap) {
    char src[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s.c", get_module_dir(), name);
    snprintf(outpath, outcap, "%s/%s", get_module_dir(), name);
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    pid_t p = fork();
    if (p == 0) {
        execlp("gcc", "gcc", "-Wall", "-O2", "-o", outpath, src, (char*)NULL);
        _exit(127);
    } else if (p > 0) {
        int status = 0; waitpid(p, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            chmod(outpath, 0755);
            return 0;
        }
        return -1;
    }
    return -1;
}

/* --- memory-list helper: find swap usage for tools/data.img (reads /proc/swaps) --- */
static int get_swap_usage_for_image(const char *imgpath, unsigned long long *out_used_kb, unsigned long long *out_size_kb)
{
    char resolved_img[PATH_MAX];
    if (realpath(imgpath, resolved_img) == NULL) {
        // Fallback if realpath fails, use the original path
        strncpy(resolved_img, imgpath, sizeof(resolved_img) - 1);
        resolved_img[sizeof(resolved_img) - 1] = '\0';
    }

    // 1. More robustly find the loop device associated with our image file.
    // We list all loop devices and check their backing file.
    char loopdev[PATH_MAX] = {0};
    // The command lists devices in "NAME BACK-FILE" format
    FILE *fp = popen("losetup -l -O NAME,BACK-FILE --noheadings", "r");
    if (fp) {
        char name[256], back_file[PATH_MAX];
        while (fscanf(fp, "%255s %4095s", name, back_file) == 2) {
            if (strcmp(back_file, resolved_img) == 0) {
                strncpy(loopdev, name, sizeof(loopdev) - 1);
                loopdev[sizeof(loopdev) - 1] = '\0';
                break; // Found it
            }
        }
        pclose(fp);
    }
    
    if (loopdev[0] == '\0') {
        // If we still can't find it, the swap is not active.
        return -1;
    }

    // 2. Parse /proc/swaps and look for the loop device we just found.
    FILE *sf = fopen("/proc/swaps", "r");
    if (!sf) return -1;
    
    char header[256];
    if (!fgets(header, sizeof(header), sf)) { // Skip header line
        fclose(sf);
        return -1;
    }

    char fname[PATH_MAX];
    char type[64];
    unsigned long long size_kb = 0, used_kb = 0;
    int prio = 0;
    int found = 0;
    
    while (fscanf(sf, "%s %s %llu %llu %d", fname, type, &size_kb, &used_kb, &prio) == 5) {
        if (strcmp(fname, loopdev) == 0) {
            found = 1;
            break;
        }
    }
    fclose(sf);

    if (!found) return -1;

    if (out_used_kb) *out_used_kb = used_kb;
    if (out_size_kb) *out_size_kb = size_kb;
    
    return 0;
}

/* --- lsmem command: emit how much of tools/data.img swap is used --- */
static void cmd_lsmem(void)
{
    char imgpath[PATH_MAX];
    snprintf(imgpath, sizeof(imgpath), "%s/data.img", get_tools_dir());

    unsigned long long used_kb = 0, size_kb = 0;
    if (get_swap_usage_for_image(imgpath, &used_kb, &size_kb) == 0 && size_kb > 0) {
        unsigned long long used_mb = (used_kb + 512) / 1024;
        unsigned long long total_mb = (size_kb + 512) / 1024;
        char out[128];
        snprintf(out, sizeof(out), "LSMEM %lluMB / %lluMB used (data.img)", used_mb, total_mb);
        emit_out(out);
    } else {
        /* if not active, report zero used and 1024MB total (match your 1GB image default) */
        emit_out("LSMEM 0MB / 1024MB used (data.img not active)");
    }
    emit_ok("lsmem");
}

static const char *get_bin_dir(void) {
    static char bindir[PATH_MAX] = {0};
    if (bindir[0]) return bindir;

    char exec_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
    if (len < 0) {
        // Fallback if readlink fails for some reason
        getcwd(bindir, sizeof(bindir));
        return bindir;
    }
    exec_path[len] = '\0';

    strcpy(bindir, exec_path);
    char *slash = strrchr(bindir, '/');
    if (slash) {
        *slash = '\0'; // Trim the executable name to get just the directory path
    } else {
        strcpy(bindir, "."); // Should not happen, but as a safe fallback
    }
    
    if (bindir[0] == '\0') bindir[0] = '/'; // Handle case where executable is in the root directory
    return bindir;
}




static void free_env_vars(void) {
    EnvVar *current = g_env_vars;
    while (current != NULL) {
        EnvVar *next = current->next;
        free(current->key);
        free(current->value);
        free(current);
        current = next;
    }
}

static const char* get_var_value(const char* key) {
    if (key == NULL) return NULL;
    // Skip a leading '$' if present
    if (key[0] == '$') {
        key++;
    }
    for (EnvVar *var = g_env_vars; var != NULL; var = var->next) {
        if (strcmp(var->key, key) == 0) {
            return var->value;
        }
    }
    return NULL;
}

// Updates an existing variable or creates a new one.
static void update_or_create_var(const char* key, const char* value) {
    // Skip a leading '$' if present
    if (key[0] == '$') {
        key++;
    }
    // Search for an existing key to update it
    for (EnvVar *var = g_env_vars; var != NULL; var = var->next) {
        if (strcmp(var->key, key) == 0) {
            free(var->value);
            var->value = strdup(value);
            return;
        }
    }

    // If not found, create a new variable
    EnvVar *new_var = (EnvVar*)malloc(sizeof(EnvVar));
    if (!new_var) {
        emit_err("memory allocation failed");
        return;
    }
    new_var->key = strdup(key);
    new_var->value = strdup(value);
    new_var->next = g_env_vars;
    g_env_vars = new_var;
}

// Handles the 'set' command
static void cmd_set(const char *arg) {
    if (!arg) {
        emit_err("usage: set KEY=VALUE or set KEY=\"VALUE\"");
        return;
    }

    char *key = strdup(arg);
    char *value;

    char *eq = strchr(key, '=');
    if (!eq) {
        free(key);
        emit_err("usage: set KEY=VALUE");
        return;
    }

    *eq = '\0'; // Null-terminate the key
    value = eq + 1;

    // Handle quoted values
    if (*value == '"') {
        value++;
        char *end_quote = strrchr(value, '"');
        if (end_quote) {
            *end_quote = '\0';
        }
    }

    // Search for an existing key to update it
    for (EnvVar *var = g_env_vars; var != NULL; var = var->next) {
        if (strcmp(var->key, key) == 0) {
            free(var->value);
            var->value = strdup(value);
            free(key);
            emit_ok("variable updated");
            return;
        }
    }

    // If not found, create a new variable
    EnvVar *new_var = (EnvVar*)malloc(sizeof(EnvVar));
    if (!new_var) {
        free(key);
        emit_err("memory allocation failed");
        return;
    }
    new_var->key = strdup(key); // Use the original key string
    new_var->value = strdup(value);
    new_var->next = g_env_vars;
    g_env_vars = new_var;

    free(key);
    emit_ok("variable set");
}

// Handles the 'unset' command
static void cmd_unset(const char *key) {
    if (!key) {
        emit_err("usage: unset <KEY>");
        return;
    }
    
    // The key might be "$VAR", so we skip the '$' if it exists
    if (key[0] == '$') {
        key++;
    }

    EnvVar *current = g_env_vars;
    EnvVar *prev = NULL;
    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            if (prev) {
                prev->next = current->next;
            } else {
                g_env_vars = current->next;
            }
            free(current->key);
            free(current->value);
            free(current);
            emit_ok("variable unset");
            return;
        }
        prev = current;
        current = current->next;
    }
}

static void expand_variables(char **tokens, int token_count) {
    for (int i = 0; i < token_count; i++) {
        char *current_token = tokens[i];
        // If there's no '$' in the token, we can skip it for performance.
        if (strchr(current_token, '$') == NULL) {
            continue;
        }

        char expanded_token[MAXLINE] = {0}; // Buffer for the new token
        char var_name[256];                 // Buffer for the variable name
        char *p = current_token;

        while (*p) {
            if (*p == '$') {
                p++; // Move past the '$'
                int j = 0;
                // Extract the variable name (alphanumeric characters)
                while (isalnum((unsigned char)*p) && j < sizeof(var_name) - 1) {
                    var_name[j++] = *p++;
                }
                var_name[j] = '\0';

                if (j > 0) { // If we found a valid variable name
                    const char *value = get_var_value(var_name);
                    if (value) {
                        // Append the variable's value to our new token
                        strncat(expanded_token, value, sizeof(expanded_token) - strlen(expanded_token) - 1);
                    }
                    // If value is not found, it's replaced with an empty string implicitly.
                } else {
                    // This was a lone '$' not followed by a name, so add it literally.
                    strncat(expanded_token, "$", sizeof(expanded_token) - strlen(expanded_token) - 1);
                }
            } else {
                // This is a regular character, so just append it.
                char temp[2] = {*p++, '\0'};
                strncat(expanded_token, temp, sizeof(expanded_token) - strlen(expanded_token) - 1);
            }
        }

        // Replace the original token with the newly created, expanded one.
        free(tokens[i]);
        tokens[i] = strdup(expanded_token);
    }
}

static void cmd_say(int argc, char **argv) {
    if (argc < 2) {
        // If it's just "say" with no arguments, print a newline like echo does
        emit_out("");
        emit_ok("say");
        return;
    }

    char output_line[MAXLINE];
    output_line[0] = '\0'; // Start with an empty string

    // Concatenate all arguments after "say" into one string
    for (int i = 1; i < argc; i++) {
        strncat(output_line, argv[i], sizeof(output_line) - strlen(output_line) - 1);
        // Add a space between arguments, but not after the last one
        if (i < argc - 1) {
            strncat(output_line, " ", sizeof(output_line) - strlen(output_line) - 1);
        }
    }

    emit_out(output_line);
    emit_ok("say");
}


static int handle_arithmetic_assignment(char **tok) {
    // Expected format: ["$VAR", "=", "$OP1", "+", "$OP2"]
    const char *target_key = tok[0];
    const char *operand1_str = tok[2];
    const char *operator_str = tok[3];
    const char *operand2_str = tok[4];

    // --- Resolve operand values ---
    const char *val1_str = (operand1_str[0] == '$') ? get_var_value(operand1_str) : operand1_str;
    const char *val2_str = (operand2_str[0] == '$') ? get_var_value(operand2_str) : operand2_str;

    if (val1_str == NULL || val2_str == NULL) {
        emit_err("one or more variables not found for calculation");
        return 1;
    }


    // --- Convert to numbers ---
    char *end1, *end2;
    double op1 = strtod(val1_str, &end1);
    double op2 = strtod(val2_str, &end2);

    if (*end1 != '\0' || *end2 != '\0') {
        emit_err("non-numeric value in arithmetic operation");
        return 1;
    }

    // --- Perform calculation ---
    double result = 0.0;
    switch (operator_str[0]) {
        case '+': result = op1 + op2; break;
        case '-': result = op1 - op2; break;
        case '*': result = op1 * op2; break;
        case '/':
            if (op2 == 0) {
                emit_err("division by zero");
                return 1;
            }
            result = op1 / op2;
            break;
        default:
            emit_err("invalid operator");
            return 1;
    }

    // --- Update the variable ---
    char result_str[64];
    snprintf(result_str, sizeof(result_str), "%g", result); // %g is good for floats
    update_or_create_var(target_key, result_str);
    
    // If you are using persistent variables, save them now
    // save_variables_to_disk(); 
    
    emit_ok("variable updated");
    return 0; // Success
}

// --- NLang Runtime ---
#include "nlang/nlang_defs.h"
#include "nlang/compiler_core.c"

#define VM_STACK_SIZE 1024
#define VM_MAX_VARS 100
#define VM_MAX_STRINGS 100

typedef struct {
    int type; // 0=INT, 1=STRING
    union {
        int32_t i;
        uint32_t s; // String ID
    } val;
} StackItem;

#define VM_CALL_STACK_SIZE 128

typedef struct {
    StackItem stack[VM_STACK_SIZE];
    int sp;
    uint32_t call_stack[VM_CALL_STACK_SIZE];
    int csp;
    int32_t vars[VM_MAX_VARS];
    char* strings[VM_MAX_STRINGS];
    uint8_t* code;
    uint32_t ip;
    uint32_t code_size;
    int running;
} VM;

static void vm_push_int(VM* vm, int32_t v) {
    if (vm->sp < VM_STACK_SIZE) {
        vm->stack[vm->sp].type = 0;
        vm->stack[vm->sp].val.i = v;
        vm->sp++;
    } else emit_err("VM Stack Overflow");
}

static void vm_push_str(VM* vm, uint32_t s) {
    if (vm->sp < VM_STACK_SIZE) {
        vm->stack[vm->sp].type = 1;
        vm->stack[vm->sp].val.s = s;
        vm->sp++;
    } else emit_err("VM Stack Overflow");
}

static StackItem vm_pop(VM* vm) {
    if (vm->sp > 0) return vm->stack[--vm->sp];
    emit_err("VM Stack Underflow");
    StackItem empty = {0}; return empty;
}

static void cmd_n_exec(const char* id_str) {
    uint32_t node_id = atoi(id_str);
    
    NLangHeader header;
    if (nodefs_read_data(node_id, 0, (char*)&header, sizeof(header)) != sizeof(header)) {
        emit_err("Failed to read binary header");
        return;
    }
    
    if (strncmp(header.magic, "NLNG", 4) != 0) {
        emit_err("Invalid binary format");
        return;
    }
    
    size_t max_size = 64 * 1024;
    uint8_t* buf = malloc(max_size);
    int len = nodefs_read_data(node_id, 0, (char*)buf, max_size);
    if (len < sizeof(header)) { free(buf); emit_err("File too small"); return; }

    VM vm;
    vm.sp = 0;
    vm.csp = 0;
    vm.ip = 0;
    vm.running = 1;
    memset(vm.vars, 0, sizeof(vm.vars));
    
    uint8_t* ptr = buf + sizeof(header);
    uint32_t str_count = *(uint32_t*)ptr; ptr += 4;
    
    for (uint32_t i = 0; i < str_count; ++i) {
        uint32_t slen = *(uint32_t*)ptr; ptr += 4;
        vm.strings[i] = (char*)ptr;
        ptr += slen;
    }
    
    vm.code = ptr;
    vm.code_size = header.code_size;
    
    emit_ok("Starting NLang VM...");

    while (vm.running && vm.ip < vm.code_size) {
        uint8_t op = vm.code[vm.ip++];
        switch (op) {
            case OP_HALT: vm.running = 0; break;
            case OP_PUSH_IMM: {
                int32_t val = *(int32_t*)(vm.code + vm.ip); vm.ip += 4;
                vm_push_int(&vm, val);
                break;
            }
            case OP_PUSH_STR: {
                uint32_t id = *(uint32_t*)(vm.code + vm.ip); vm.ip += 4;
                vm_push_str(&vm, id);
                break;
            }
            case OP_POP: vm_pop(&vm); break;
            case OP_LOAD: {
                uint32_t id = *(uint32_t*)(vm.code + vm.ip); vm.ip += 4;
                if (id < VM_MAX_VARS) vm_push_int(&vm, vm.vars[id]);
                break;
            }
            case OP_STORE: {
                uint32_t id = *(uint32_t*)(vm.code + vm.ip); vm.ip += 4;
                StackItem val = vm_pop(&vm);
                if (id < VM_MAX_VARS && val.type == 0) vm.vars[id] = val.val.i;
                break;
            }
            case OP_ADD: {
                StackItem b = vm_pop(&vm); StackItem a = vm_pop(&vm);
                if (a.type == 0 && b.type == 0) vm_push_int(&vm, a.val.i + b.val.i);
                break;
            }
            case OP_SUB: {
                StackItem b = vm_pop(&vm); StackItem a = vm_pop(&vm);
                if (a.type == 0 && b.type == 0) vm_push_int(&vm, a.val.i - b.val.i);
                break;
            }
            case OP_MUL: {
                StackItem b = vm_pop(&vm); StackItem a = vm_pop(&vm);
                if (a.type == 0 && b.type == 0) vm_push_int(&vm, a.val.i * b.val.i);
                break;
            }
            case OP_DIV: {
                StackItem b = vm_pop(&vm); StackItem a = vm_pop(&vm);
                if (a.type == 0 && b.type == 0 && b.val.i != 0) vm_push_int(&vm, a.val.i / b.val.i);
                break;
            }
            case OP_EQ: {
                StackItem b = vm_pop(&vm); StackItem a = vm_pop(&vm);
                if (a.type == 0 && b.type == 0) vm_push_int(&vm, a.val.i == b.val.i);
                break;
            }
            case OP_GT: {
                StackItem b = vm_pop(&vm); StackItem a = vm_pop(&vm);
                if (a.type == 0 && b.type == 0) vm_push_int(&vm, a.val.i > b.val.i);
                break;
            }
            case OP_LT: {
                StackItem b = vm_pop(&vm); StackItem a = vm_pop(&vm);
                if (a.type == 0 && b.type == 0) vm_push_int(&vm, a.val.i < b.val.i);
                break;
            }
            case OP_PRINT: {
                StackItem val = vm_pop(&vm);
                if (val.type == 0) {
                    char buf[32]; snprintf(buf, 32, "%d", val.val.i); emit_out(buf);
                } else {
                    emit_out(vm.strings[val.val.s]);
                }
                break;
            }
            case OP_INPUT: {
                emit_out("Input: ");
                char inbuf[64];
                if (fgets(inbuf, sizeof(inbuf), stdin)) {
                    vm_push_int(&vm, atoi(inbuf));
                } else {
                    vm_push_int(&vm, 0);
                }
                break;
            }
            case OP_JMP: {
                uint32_t addr = *(uint32_t*)(vm.code + vm.ip); vm.ip += 4;
                vm.ip = addr;
                break;
            }
            case OP_JMP_FALSE: {
                uint32_t addr = *(uint32_t*)(vm.code + vm.ip); vm.ip += 4;
                StackItem val = vm_pop(&vm);
                if (val.type == 0 && val.val.i == 0) vm.ip = addr;
                break;
            }
            case OP_CALL: {
                uint32_t addr = *(uint32_t*)(vm.code + vm.ip); vm.ip += 4;
                if (vm.csp < VM_CALL_STACK_SIZE) {
                    vm.call_stack[vm.csp++] = vm.ip;
                    vm.ip = addr;
                } else {
                    emit_err("VM Call Stack Overflow");
                    vm.running = 0;
                }
                break;
            }
            case OP_RET: {
                if (vm.csp > 0) {
                    vm.ip = vm.call_stack[--vm.csp];
                } else {
                    vm.running = 0;
                }
                break;
            }
        }
    }
    
    emit_ok("VM Halted.");
    free(buf);
}

static void cmd_n_compile(const char* src_id_str, const char* out_name) {
    uint32_t src_id = atoi(src_id_str);
    
    char* src = malloc(64 * 1024);
    int len = nodefs_read_data(src_id, 0, src, 64 * 1024);
    if (len < 0) { emit_err("Failed to read source"); free(src); return; }
    src[len] = '\0';
    
    uint8_t* bin_buf = NULL;
    uint32_t bin_size = 0;
    
    if (nlang_compile(src, &bin_buf, &bin_size) != 0) {
        emit_err("Compilation failed");
        free(src);
        return;
    }
    
    free(src);
    
    int current_dir = nodefs_get_current_node();
    int out_id = nodefs_create_node(current_dir, out_name, NODE_TYPE_FILE);
    if (out_id < 0) {
        emit_err("Failed to create output file");
        free(bin_buf);
        return;
    }
    
    if (nodefs_write_data(out_id, (char*)bin_buf, bin_size) < 0) {
        emit_err("Failed to write binary data");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Compiled to %s (ID: %d, Size: %d)", out_name, out_id, bin_size);
        emit_ok(msg);
    }
    
    free(bin_buf);
}


// Helper function to get the basename of a path
static const char* get_basename(const char* path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Helper to copy a single file's contents and mode
static int copy_file(const char *src_path, const char *dest_path) {
    FILE *src_file, *dest_file;
    char buffer[BUF_SIZE];
    size_t bytes;
    struct stat st;

    src_file = fopen(src_path, "rb");
    if (!src_file) {
        return -1;
    }

    dest_file = fopen(dest_path, "wb");
    if (!dest_file) {
        fclose(src_file);
        return -1;
    }

    while ((bytes = fread(buffer, 1, BUF_SIZE, src_file)) > 0) {
        if (fwrite(buffer, 1, bytes, dest_file) != bytes) {
            fclose(src_file);
            fclose(dest_file);
            unlink(dest_path); // Clean up partial file
            return -1;
        }
    }

    fclose(src_file);
    fclose(dest_file);

    // Copy permissions
    if (stat(src_path, &st) == 0) {
        chmod(dest_path, st.st_mode);
    }

    return 0;
}

// Recursive copy function for both files and directories
static int copy_recursive(const char *src, const char *dest) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dest, 0755) != 0 && errno != EEXIST) {
            return -1;
        }

        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *ent;
        int res = 0;

        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char src_child[PATH_MAX];
            char dest_child[PATH_MAX];
            snprintf(src_child, sizeof(src_child), "%s/%s", src, ent->d_name);
            snprintf(dest_child, sizeof(dest_child), "%s/%s", dest, ent->d_name);

            if (copy_recursive(src_child, dest_child) != 0) {
                res = -1;
                break;
            }
        }
        closedir(d);
        return res;
    } else { // It's a file or symlink
        return copy_file(src, dest);
    }
}


static void cmd_cp(int argc, char **argv) {
    if (argc != 2) {
        emit_err("cp usage: cp <source> <destination>");
        return;
    }
    char *src = argv[0];
    char *dest = argv[1];
    char final_dest[PATH_MAX];

    struct stat dest_st;
    // Check if the destination exists and is a directory
    if (stat(dest, &dest_st) == 0 && S_ISDIR(dest_st.st_mode)) {
        snprintf(final_dest, sizeof(final_dest), "%s/%s", dest, get_basename(src));
    } else {
        strncpy(final_dest, dest, sizeof(final_dest) - 1);
        final_dest[sizeof(final_dest) - 1] = '\0';
    }

    if (copy_recursive(src, final_dest) == 0) {
        emit_ok("cp completed");
    } else {
        emit_err(strerror(errno));
    }
}

static void cmd_mv(int argc, char **argv) {
    if (argc != 2) {
        emit_err("mv usage: mv <source> <destination_or_new_name>");
        return;
    }
    char *src = argv[0];
    char *dest = argv[1];
    char final_dest[PATH_MAX];

    struct stat dest_st;
    // Check if destination is an existing directory to move source inside it
    if (stat(dest, &dest_st) == 0 && S_ISDIR(dest_st.st_mode)) {
        snprintf(final_dest, sizeof(final_dest), "%s/%s", dest, get_basename(src));
    } else {
        strncpy(final_dest, dest, sizeof(final_dest) - 1);
        final_dest[sizeof(final_dest) - 1] = '\0';
    }

    // Attempt the fast rename operation first
    if (rename(src, final_dest) == 0) {
        emit_ok("mv completed");
        return;
    }

    // If rename failed with EXDEV, it's a cross-device move.
    if (errno == EXDEV) {
        if (copy_recursive(src, final_dest) == 0) {
            // If copy succeeded, remove the original recursively
            if (rm_recursive_internal(src) == 0) {
                emit_ok("mv completed (cross-device)");
            } else {
                emit_err("failed to remove original after cross-device move");
            }
        } else {
            emit_err("copy failed during cross-device move");
        }
    } else {
        // Some other error occurred with rename()
        emit_err(strerror(errno));
    }
}


static void cmd_su(int argc, char **argv) {
    if (argc < 1) {
        emit_err("su usage: su <command> [args...]");
        return;
    }

    char helper_path[PATH_MAX];
    snprintf(helper_path, sizeof(helper_path), "%s/cortez_su_helper", get_tools_dir());

    if (access(helper_path, X_OK) != 0) {
        emit_err("cortez_su_helper not found or not executable. Please compile and set permissions.");
        return;
    }

    char command_path[PATH_MAX];
    char *command_to_exec = argv[0]; // By default, use the command name as is

    // Check if the command exists as an executable in our tools directory
    snprintf(command_path, sizeof(command_path), "%s/%s", get_tools_dir(), argv[0]);
    if (access(command_path, X_OK) == 0) {
        // It exists! Use the full, absolute path instead of just the name.
        command_to_exec = command_path;
    }

    char **pargv = calloc(argc + 2, sizeof(char*));
    if (!pargv) {
        emit_err("memory allocation failed");
        return;
    }

    pargv[0] = strdup(helper_path);
    pargv[1] = strdup(command_to_exec); // Use the potentially resolved path
    for (int i = 1; i < argc; ++i) { // Note: loop starts at 1 now
        pargv[i + 1] = strdup(argv[i]);
    }
    pargv[argc + 1] = NULL;

    // Execute the helper in the PTY stream
    if (start_stream_thread(pargv) != 0) {
        free_argv_array(pargv);
        emit_err("another stream is already running or start failed");
        return;
    }

    emit_ok("su command initiated");
}

static void cmd_about(){
    emit_out("CRT Terminal Made by:\nPatricj Andrew Cortez\nIn Oct 20 ,2025");
}

static void check_filesystem(void) {
    const char* disk_path = "cortez_drive.img";
    if (access(disk_path, F_OK) != 0) {
        emit_out("WARNING: No Cortez File System detected.");
        emit_out("Initialize NodeFS (5GB)? [Y/n]");
        
        char response[10];
        if (fgets(response, sizeof(response), stdin)) {
            if (response[0] == 'n' || response[0] == 'N') {
                emit_out("Skipping filesystem initialization.");
                return;
            }
        }
        
        if (nodefs_format(disk_path, 5120) == 0) { // 5GB
            emit_ok("NodeFS Formatted Successfully.");
        } else {
            emit_err("Failed to format NodeFS.");
            return;
        }
    }
    
    int mount_res = nodefs_mount(disk_path);
    if (mount_res == -2) {
        emit_out("WARNING: NodeFS Version Mismatch. Performing Smart Update...");
        
        // Backup
        char backup_path[256];
        snprintf(backup_path, sizeof(backup_path), "%s.bak", disk_path);
        emit_out("Backing up old drive to .bak...");
        
        FILE* src = fopen(disk_path, "rb");
        FILE* dst = fopen(backup_path, "wb");
        if (src && dst) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                fwrite(buf, 1, n, dst);
            }
            emit_ok("Backup successful.");
        } else {
            emit_err("Backup failed! Proceeding with caution...");
        }
        if (src) fclose(src);
        if (dst) fclose(dst);

        emit_out("Auto-updating (Reformatting) to Version 4...");
        if (nodefs_format(disk_path, 5120) == 0) {
             emit_ok("NodeFS Updated Successfully.");
             nodefs_mount(disk_path);
        } else {
             emit_err("Failed to update NodeFS.");
        }
    } else if (mount_res != 0) {
        emit_out("WARNING: Filesystem corruption or error.");
        emit_out("Re-initialize NodeFS (Wipe Data)? [Y/n]");
        
        char response[10];
        if (fgets(response, sizeof(response), stdin)) {
            if (response[0] == 'n' || response[0] == 'N') {
                emit_out("Skipping filesystem initialization.");
                return;
            }
        }
        
        if (nodefs_format(disk_path, 5120) == 0) {
             emit_ok("NodeFS Formatted Successfully.");
             nodefs_mount(disk_path);
        }
    }
}

/* --- main loop --- */
int main(int argc, char **argv) {
    ck_init();
    ck_drivers_init();
    ck_interrupts_init();
    
    check_filesystem(); // NEW CHECK

    setvbuf(stdout, NULL, _IOLBF, 0);
    char line[MAXLINE];

    emit_ok("backend ready");
    cmd_lsmem();

    while (fgets(line, sizeof(line), stdin)) {
        /* trim newline */
        size_t ln = strlen(line);
        while (ln && (line[ln-1]=='\n' || line[ln-1]=='\r')) { line[--ln]=0; }

        if (ln == 0) continue;

        /* Special control commands sent by frontends */
        if (strcmp(line, "SIGINT") == 0) {
            send_signal_to_stream_child(SIGINT);
            emit_ok("sent SIGINT");
            continue;
        }
        if (strcmp(line, "SIGTERM") == 0) {
            send_signal_to_stream_child(SIGTERM);
            emit_ok("sent SIGTERM");
            continue;
        }

        int tokc = 0;
        char **tok = tokenize(line, &tokc);
        if (!tok || tokc == 0) { emit_err("parse error"); free_tokens(tok, tokc); continue; }

        if (tokc == 5 && tok[0][0] == '$' && strcmp(tok[1], "=") == 0 && strlen(tok[3]) == 1) {
            // This pattern matches "$VAR = OP1 OPR OP2", handle it separately.
            handle_arithmetic_assignment(tok);
            free_tokens(tok, tokc);
            continue; // Skip normal command processing
        }

        expand_variables(tok, tokc);

        if (strcmp(tok[0], "help") == 0 || strcmp(tok[0], "?") == 0) {
            emit_out("Commands {Run Commands with su for admin previledge}:");
            emit_out("  help                   - show this help");
            emit_out("  load [options] <proj>  - launch a project (must be added first) | --add,--list, --remove");
            emit_out("  module ...             - module commands (build/list/run/integrate)");
            emit_out("  net <args...>          - run tools/net-twerk with provided args | --show,--connect,--disconnect)");
            emit_out("  netr <args...>         - Much advanced networking commands | wget, curl, showp,ftp");
            emit_out("  wipe                   - clears the screen");
            emit_out("  pwd                    - displays your present working dir");
            emit_out("  ls                     - lists every file in the current dir");
            emit_out("  create <filename.ext>  - creates the file in the current dir");
            emit_out("  rm                     - removes/deletes files | -r (recursively delete) | -d (delete empty dir)");
            emit_out("  cp <src> <dest>        - copies files or directories");
            emit_out("  mv <src> <dest>        - moves or renames files or directories");
            emit_out("  shell                  - use normal shell");
            emit_out("  shutdown                   - quit backend");
            emit_out("  lsmem                  - Displays how much vmemory is being used");
            emit_out("  proc --start [-- <prog> [args...]] - start a background job (via tools/cortez_pm)");
            emit_out("  proc --list                           - list background jobs (via tools/cortez_pm)");
            emit_out("  proc --kill <pid|jobid>               - kill a job by pid or jobid");
            emit_out("  cedit                  - Text Editor for editing files | cedit <filename>");
            emit_out("  mkdir <dir/subdir>     - Makes directories and their subdir.");
            emit_out("  show </path/to/image>  - Displays pixelated image to terminal | -v (Displays Video)");
            emit_out("  play <path-to-audio>   - Plays Audio");
            emit_out("  ckg <args...>          - A package manager to download packages | options: install <package>, list,update,uninstall <package>");
            emit_out("  sysinfo                - Displays your systems key information");
            emit_out("  find <pattern>         - finds files by name");
            emit_out("  file-digest <filename> - shows checksum of a file");
            emit_out("  encrypt <key_file> <input_plaintext_file> <output_ciphertext_file> - Encrypts a file");
            emit_out("  decrypt <key_file> <input_ciphertext_file> <output_plaintext_file> - Decrypts a file");
            emit_out("  set Var=<value>        - sets environmental variable");
            emit_out("  unset Var              - deletes environmental variable");
            emit_out("  say <message>           - displays the sentence");
            emit_out("  help-p2                - 2nd Page of Help");
            emit_ok("<--------------------------------------------------------------------------------------->");
        }else if (strcmp(tok[0], "help-p2") == 0 || strcmp(tok[0], "?") == 0){
            emit_out("Commands Page 2:");
            emit_out("  lsexec                 - Tests the Cortez Tunnel IPC System");
            emit_out("  cartridge <game.gb>    - a game boy emulator that plays any .gb file.");
            emit_out("  compile [language: c,c++,java] <source> <options> - compiles codes into executable programs."); 
            emit_out("  syscan                 - scans for any suspicious open ports and processes");
            emit_out("  digest-filter <filename> <target-word> -scans the file for checksum and finds the word in the file");
            emit_out("  exodus <options> <args...> - scans the uploaded file and finds how many instances of a word or change a specific word");
            emit_out("  mesh <options>         - Checks if the mesh ipc systems health");
            emit_out("  about                  - all about the dev T_T"); //Some info about me -_-
    
            emit_out("NodeFS Commands:");
            emit_out("  n-ls                   - List directory contents");
            emit_out("  n-create <name> [type] - Create node (FILE|DIR|LINK)");
            emit_out("  n-link <id> <name>     - Create hard link to node ID");
            emit_out("  n-cd <id>              - Change directory to node ID");
            emit_out("  n-read <id>     - Read node content");
            emit_out("  n-unlink <id>   - Unlink (delete) entry from directory");
            emit_out("  n-delete <id>   - Delete node (and data)");
            emit_out("  nedit <id>      - Open GUI editor for node");
            emit_out("  nedit-cli <id>  - Open text-based editor for node");
            emit_out("  n-cedit <id> <content> - Set node content directly");
            emit_out("  n-compile <src_id> <out> - Compile NLang source to binary");
            emit_out("  n-exec <id>     - Execute NLang binary");
            emit_ok("<--------------------------------------------------------------------------------------->");
        }else if (strcmp(tok[0], "su") == 0) {
            cmd_su(tokc - 1, &tok[1]);
        }else if (strcmp(tok[0], "Ksay") == 0 || strcmp(tok[0], "ksay") == 0) {
            cmd_ksay(tokc - 1, &tok[1]);
        }else if (strcmp(tok[0], "n-ls") == 0) {
            cmd_n_ls();
        }else if (strcmp(tok[0], "n-create") == 0) {
            if (tokc >= 3) cmd_n_create(tok[1], tok[2], NULL); else emit_err("Usage: n-create <name> <type>");
        }else if (strcmp(tok[0], "n-link") == 0) {
            if (tokc >= 3) cmd_n_link(tok[1], tok[2]); else emit_err("Usage: n-link <id> <name>");
        }else if (strcmp(tok[0], "n-cd") == 0) {
            if (tokc >= 2) cmd_n_cd(tok[1]); else emit_err("Usage: n-cd <id>");
        }else if (strcmp(tok[0], "n-cedit") == 0) {
            if (tokc >= 3) cmd_n_cedit(tokc - 1, &tok[1]); else emit_err("Usage: n-cedit <id> <content...>");
        }else if (strcmp(tok[0], "n-exec") == 0) {
            if (tokc >= 2) cmd_n_exec(tok[1]); else emit_err("Usage: n-exec <id>");
        }else if (strcmp(tok[0], "n-compile") == 0) {
            if (tokc >= 3) cmd_n_compile(tok[1], tok[2]); else emit_err("Usage: n-compile <src_id> <out_name>");
        }else if (strcmp(tok[0], "n-read") == 0) {
            if (tokc >= 2) cmd_n_read(tok[1]); else emit_err("Usage: n-read <id>");
        }else if (strcmp(tok[0], "n-unlink") == 0) {
            if (tokc >= 2) cmd_n_unlink(tok[1]); else emit_err("Usage: n-unlink <id>");
        }else if (strcmp(tok[0], "n-delete") == 0) {
            if (tokc >= 2) cmd_n_delete(tok[1]); else emit_err("Usage: n-delete <id>");
        }else if (strcmp(tok[0], "nedit") == 0) {
            if (tokc >= 2) cmd_gnedit(tok[1]); else emit_err("Usage: nedit <id>");
        }else if (strcmp(tok[0], "nedit-cli") == 0) {
            if (tokc >= 2) cmd_nedit_cli(tok[1]); else emit_err("Usage: nedit-cli <id>");
        }else if (strcmp(tok[0], "gnedit") == 0) {
            if (tokc >= 2) cmd_gnedit(tok[1]); else emit_err("Usage: gnedit <id>");
        }else if (strcmp(tok[0], "n-write") == 0) {
            if (tokc >= 3) cmd_n_write(tokc - 1, &tok[1]); else emit_err("Usage: n-write <id> <size>");
        }else if (strcmp(tok[0], "set") == 0) {
            const char* arg = strchr(line, ' ');
            if (arg) cmd_set(arg + 1); else cmd_set(NULL);
        } else if (strcmp(tok[0], "unset") == 0) {
            if (tokc >= 2) cmd_unset(tok[1]); else cmd_unset(NULL);
        } else if (strcmp(tok[0], "say") == 0) {
            cmd_say(tokc, tok);
        } else if (strcmp(tok[0], "about") == 0) {
            cmd_about(tokc, tok);
        }else if (strcmp(tok[0], "shutdown") == 0) {
            emit_ok("shutting down...");
            free_tokens(tok, tokc);
            break;
        } else if (strcmp(tok[0], "load") == 0) {
            if (tokc >= 2 && strcmp(tok[1], "--list") == 0) {
                cmd_project_list();
            } else if (tokc >= 3 && strcmp(tok[1], "--add") == 0) {
                cmd_project_add(tok[2], tokc >= 4 ? tok[3] : "");
            } else if (tokc >= 3 && strcmp(tok[1], "--remove") == 0) {
                cmd_project_remove(tok[2]);
            } else if (tokc >= 2) {
                cmd_project_launch(tok[1]);
            } else {
                emit_err("load usage");
            }
        } else if (strcmp(tok[0], "module") == 0) {
            if (tokc >= 2 && (strcmp(tok[1], "--list") == 0)) {
                cmd_module_list();
            } else if (tokc >= 3 && strcmp(tok[1], "--build") == 0) {
                cmd_module_build(tok[2]);
            } else if (tokc >= 4 && (strcmp(tok[1], "--add") == 0 || strcmp(tok[1], "add") == 0)) {
                cmd_module_add(tok[2], tok[3]);
            } else if (tokc >= 3 && (strcmp(tok[1], "--remove") == 0 || strcmp(tok[1], "remove") == 0)) {
                cmd_module_remove(tok[2]);
            } else if (tokc >= 2) {
                /* module <name> [args...] -> run module/<name> (compile if .c exists) */
                char exe[PATH_MAX];
                snprintf(exe, sizeof(exe), "%s/%s", get_module_dir(), tok[1]);
                struct stat st;
                if (stat(exe, &st) != 0) {
                    /* try compile if .c exists */
                    char src[PATH_MAX];
                    snprintf(src, sizeof(src), "%s/%s.c", get_module_dir(), tok[1]);
                    if (stat(src, &st) == 0) {
                        if (compile_module_and_get_exe(tok[1], exe, sizeof(exe)) != 0) {
                            emit_err("build failed");
                            free_tokens(tok, tokc);
                            continue;
                        }
                    } else {
                        emit_err("module not found");
                        free_tokens(tok, tokc);
                        continue;
                    }
                }
                /* build argv of arguments after name */
                int argn = tokc - 2;
                char **args = NULL;
                if (argn > 0) {
                    args = calloc(argn, sizeof(char*));
                    for (int i = 0; i < argn; ++i) args[i] = strdup(tok[2 + i]);
                }
                cmd_module_run_by_path(exe, argn, args);
                if (args) { for (int i=0;i<argn;i++) free(args[i]); free(args); }
            } else {
                emit_err("module usage");
            }
        } else if (strcmp(tok[0], "net") == 0) {
            if (tokc == 1) {
                emit_out("Usage: net <args...>   (this will run tools/net-twerk with the args)");
                emit_ok("net usage");
            } else {
                cmd_net_run(tokc - 1, &tok[1]);
            }
                } else if (strcmp(tok[0], "STDIN") == 0) {
            /* send remainder of the original line as input + newline */
            const char *rest = line + strlen(tok[0]);
            while (*rest == ' ') rest++;
            if (!*rest) {
                emit_err("no input");
            } else {
                if (send_input_to_stream(rest, strlen(rest), 1) == 0) emit_ok("stdin sent");
                else emit_err("no stream");
            }
        } else if (strcmp(tok[0], "WRITE") == 0) {
            /* send remainder raw (no newline) */
            const char *rest = line + strlen(tok[0]);
            while (*rest == ' ') rest++;
            if (!*rest) {
                emit_err("no input");
            } else {
                if (send_input_to_stream(rest, strlen(rest), 0) == 0) emit_ok("write sent");
                else emit_err("no stream");
            } 
        } else if (strcmp(tok[0], "wipe") == 0 || strcmp(tok[0], "clear") == 0) {
            /* ANSI: clear screen + move cursor home */
            const char *clr = "\x1b[2J\x1b[H";

            pthread_mutex_lock(&stream_lock);
            int running = stream_running && stream_master_fd >= 0;
            int fd = stream_master_fd;
            pthread_mutex_unlock(&stream_lock);

            if (running) {
                /* write directly to PTY so the child terminal clears */
                ssize_t r = write(fd, clr, strlen(clr));
                if (r >= 0) emit_ok("wiped");
                else emit_err("write failed");
            } else {
                /* no PTY -> emit the control sequence as OUT so frontends that honor ANSI will clear */
                emit_out(clr);
                emit_ok("wiped");
            }
        }  else if (strcmp(tok[0], "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(tok[0], "cd") == 0) {
    /* cd [path] */
    if (tokc >= 2) cmd_cd(tok[1]);
    else cmd_cd(NULL);
} else if (strcmp(tok[0], "lsdir") == 0) {
    cmd_lsdir();
} else if (strcmp(tok[0], "shell") == 0) {

    if (tokc >= 2) {
        cmd_shell(tokc - 1, &tok[1]);
    } else {
        cmd_shell(0, NULL);
    }
} else if (strcmp(tok[0], "netr") == 0) {
            if (tokc == 1) {
                emit_out("Usage: netr <options: wget,curl,ftp> <args>");
                emit_ok("netr usage");
            } else {
                /* pass arguments after the command name into cmd_netr_run */
                cmd_netr_run(tokc - 1, &tok[1]);
            }
         } else if(strcmp(tok[0], "cd") == 0) {
        if (tokc >= 2) cmd_cd(tok[1]);
        else cmd_cd(NULL);
    } else if (strcmp(tok[0], "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(tok[0], "lsdir") == 0) {
        cmd_lsdir();
    } else if(strcmp(tok[0],"lsmem") == 0){
    cmd_lsmem();
} else if (strcmp(tok[0], "proc") == 0) {
    if (tokc < 2) {
        emit_err("proc usage");
    } else if (strcmp(tok[1], "--list") == 0) {
        cmd_proc_list();
    } else if (strcmp(tok[1], "--kill") == 0) {
        if (tokc >= 3) cmd_proc_kill(tok[2]);
        else emit_err("proc --kill needs arg");
    } else if (strcmp(tok[1], "--start") == 0) {
        /* pass remaining tokens as argv to cmd_proc_start */
        int subc = tokc - 2;
        char **subv = NULL;
        if (subc > 0) {
            subv = &tok[2]; /* safe: tokenize memory remains until free_tokens called later */
        }
        cmd_proc_start(subc, subv);
    } else {
        emit_err("proc usage");
    }
}else if(strcmp(tok[0], "ls") == 0){
    cmd_ls();
} else if (strcmp(tok[0], "create") == 0) {
        if (tokc >= 2) {
            cmd_create(tok[1]);
        } else {
            emit_err("create usage: create <filename>");
        }
    }else if (strcmp(tok[0], "rm") == 0) {
        if (tokc >= 2) {
             cmd_rm(tokc - 1, &tok[1]);
        } else {
            emit_err("rm usage: rm <filename>");
        }
    } else if (strcmp(tok[0], "cp") == 0) {
    if (tokc >= 3) {
        cmd_cp(tokc - 1, &tok[1]);
    } else {
        emit_err("cp usage: cp <source> <destination>");
    }
} else if (strcmp(tok[0], "mv") == 0) {
    if (tokc >= 3) {
        cmd_mv(tokc - 1, &tok[1]);
    } else {
        emit_err("mv usage: mv <source> <destination>");
    }
}  else if (strcmp(tok[0], "read") == 0) {
        if (tokc >= 2) cmd_read(tok[1]);
        else emit_err("read usage: read <filename>");
} else if (strcmp(tok[0],"cedit") == 0){
     if (tokc >= 2) {
            cmd_cedit(tokc - 1, &tok[1]);
        } else {
            emit_err("cedit usage: cedit <filename>");
        }
} else if (strcmp(tok[0], "mkdir") == 0) {
        cmd_mkdir(tokc - 1, &tok[1]);
    } else if (strcmp(tok[0], "show") == 0) {
    if (tokc < 2) {
        emit_err("show usage: show [-v] <path>");
    } else if (strcmp(tok[0], "show") == 0) {
    if (tokc < 2) {
        emit_err("show usage: show [-v] <path>");
    } else if (tokc >= 3 && strcmp(tok[1], "-v") == 0) {
        
        char resolved_path[PATH_MAX];
        if (realpath(tok[2], resolved_path) == NULL) {
            emit_err("File not found or path is invalid");
        } else {
            emitf("CMD_SHOW_VIDEO %s", resolved_path);
        }
        
    } else {

        char resolved_path[PATH_MAX];
        if (realpath(tok[1], resolved_path) != NULL) {
            emitf("CMD_SHOW_IMAGE %s", resolved_path);
        } else {
            emit_err("File not found or path is invalid");
        }
    }
 } else {
        // Image command: show <path>
        char resolved_path[PATH_MAX];
        if (realpath(tok[1], resolved_path) != NULL) {
            emitf("CMD_SHOW_IMAGE %s", resolved_path);
        } else {
            emit_err("File not found or path is invalid");
        }
    }
} else if (strcmp(tok[0], "play") == 0) {
    cmd_play_audio(tokc > 1 ? tok[1] : NULL);
} else if (strcmp(tok[0], "ckg") == 0) {
    if (tokc == 1) {
        emit_out("Usage: ckg <args...>   (runs tools/ckg -- e.g. ckg update | ckg list | ckg install <pkg>)");
        emit_ok("ckg usage");
    } else {
        cmd_ckg_run(tokc - 1, &tok[1]);
    }
} else {
    /*
     * If no PTY stream is running and the command is not a built-in,
     * treat it as a potential executable in the tools/ directory.
     */
    char tool_path[PATH_MAX];
    snprintf(tool_path, sizeof(tool_path), "%s/%s", get_tools_dir(), tok[0]);

    if (path_is_executable(tool_path)) {
        /* The command exists as an executable in tools/. Prepare to run it. */
        
        // Build the argument vector: tool_path followed by user args.
        char **pargv = calloc(tokc + 1, sizeof(char*));
        if (!pargv) {
            emit_err("memory allocation failed");
        } else {
            pargv[0] = strdup(tool_path);
            for (int i = 1; i < tokc; ++i) {
                pargv[i] = strdup(tok[i]);
            }
            pargv[tokc] = NULL; // The array must be NULL-terminated.

            // Start the executable in a new PTY stream.
            if (start_stream_thread(pargv) != 0) {
                free_argv_array(pargv); // Clean up if starting fails.
                emit_err("another stream is already running or start failed");
            } else {
                // Create a dynamic success message with the command name
                char ok_msg[MAXLINE];
                snprintf(ok_msg, sizeof(ok_msg), "%s initiated", tok[0]);
                emit_ok(ok_msg);
            }
        }
    } else if (strncmp(tok[0], "./", 2) == 0) {
        /* Handle direct execution of local binaries */
        char exec_path[PATH_MAX];
        /* Resolve full path relative to current directory */
        /* We are in the backend process, so CWD is correct */
        if (realpath(tok[0], exec_path) == NULL) {
            char err_msg[MAXLINE];
            snprintf(err_msg, sizeof(err_msg), "%s: file not found", tok[0]);
            emit_err(err_msg);
        } else {
            /* Check if executable */
            if (path_is_executable(exec_path)) {
                /* Build argv */
                char **pargv = calloc(tokc + 1, sizeof(char*));
                if (!pargv) {
                    emit_err("memory allocation failed");
                } else {
                    pargv[0] = strdup(exec_path);
                    for (int i = 1; i < tokc; ++i) {
                        pargv[i] = strdup(tok[i]);
                    }
                    pargv[tokc] = NULL;

                    if (start_stream_thread(pargv) != 0) {
                        free_argv_array(pargv);
                        emit_err("failed to start process");
                    } else {
                        char ok_msg[MAXLINE];
                        snprintf(ok_msg, sizeof(ok_msg), "%s started", tok[0]);
                        emit_ok(ok_msg);
                    }
                }
            } else {
                char err_msg[MAXLINE];
                snprintf(err_msg, sizeof(err_msg), "%s: not executable", tok[0]);
                emit_err(err_msg);
            }
        }
    } else {
        /* Command was not found as a built-in or in the tools/ directory. */
        char err_msg[MAXLINE];
        snprintf(err_msg, sizeof(err_msg), "%s: command not found", tok[0]);
        emit_err(err_msg);
    }
}

    free_tokens(tok, tokc);
}/* main loop */

    pthread_mutex_lock(&stream_lock);
    if (stream_running && stream_child_pid > 0) {
        pid_t pg = getpgid(stream_child_pid);
        if (pg > 0) kill(-pg, SIGTERM);
        else kill(stream_child_pid, SIGTERM);
        waitpid(stream_child_pid, NULL, 0);
    }
    pthread_mutex_unlock(&stream_lock);

    free_env_vars();

    return 0;
}

