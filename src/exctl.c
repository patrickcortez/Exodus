/*
 * exctl.c - Exodus Guardian Control Utility
 *
 * Provides a 'systemctl' like interface for managing independent
 * node guardians started via XDG Autostart.
 *
 * Compile Command:
 * gcc -Wall -Wextra -O2 exctl.c ctz-json.a -o exctl
 *
 * To build a fully self-contained static binary (optional):
 * gcc -Wall -Wextra -O2 exctl.c ctz-json.a -static -o exctl
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <libgen.h> // For dirname() and basename()
#include <errno.h>
#include <ctype.h> // For isdigit

#include "ctz-json.h"
#include "exodus-common.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// --- Color Definitions ---
#define C_GREEN "\033[0;32m"
#define C_RED "\033[0;31m"
#define C_YELLOW "\033[0;33m"
#define C_BOLD "\033[1m"
#define C_RESET "\033[0m"

// --- Struct for holding process stats ---
typedef struct {
    char state;
    int tasks;
    long vmrss_kb;
    unsigned long utime;
    unsigned long stime;
} process_stats;


// --- Helper: get_executable_dir ---
int get_executable_dir(char* buffer, size_t size) {
    char path_buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path_buf, sizeof(path_buf) - 1);
    if (len == -1) {
        if (getcwd(buffer, size) != NULL) return 0;
        perror("readlink/getcwd failed");
        return -1;
    }
    path_buf[len] = '\0';
    char* dir = dirname(path_buf);
    if (dir == NULL) {
        perror("dirname failed");
        return -1;
    }
    strncpy(buffer, dir, size - 1);
    buffer[size - 1] = '\0';
    return 0;
}

// --- Helper: get_config_path ---
static int get_config_path(char* buffer, size_t size) {
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "Error: Could not determine executable directory.\n");
        return -1;
    }
    snprintf(buffer, size, "%s/nodewatch.json", exe_dir);
    return 0;
}

// --- Helper: get_guardian_path ---
static void get_guardian_path(const char* node_path, const char* node_name, char* buf, size_t size) {
    snprintf(buf, size, "%s/.log/%s-guardian", node_path, node_name);
}

// --- NEW Helper: get_home_from_uid ---
// Manually parses /etc/passwd to find home dir for a UID.
// This avoids getpwuid() which has linker issues.
static int get_home_from_uid(uid_t uid, char* home_buf, size_t home_size) {
    FILE* f = fopen("/etc/passwd", "r");
    if (!f) {
        perror("[exctl] Error: Could not open /etc/passwd");
        return -1;
    }
    char line[1024];
    char* saveptr;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char* line_copy = strdup(line);
        if (line_copy == NULL) continue;

        strtok_r(line_copy, ":", &saveptr); // name
        strtok_r(NULL, ":", &saveptr);      // pass
        char* uid_str = strtok_r(NULL, ":", &saveptr);

        if (uid_str && atoi(uid_str) == (int)uid) {
            strtok_r(NULL, ":", &saveptr); // gid
            strtok_r(NULL, ":", &saveptr); // gecos
            char* home_dir = strtok_r(NULL, ":", &saveptr);
            if (home_dir) {
                strncpy(home_buf, home_dir, home_size - 1);
                home_buf[home_size - 1] = '\0';
                found = 1;
            }
            free(line_copy);
            break;
        }
        free(line_copy);
    }
    fclose(f);
    return found ? 0 : -1;
}

// --- NEW Helper: get_user_uid_from_path ---
// Gets the owner UID of the node path itself
static int get_user_uid_from_path(const char* path, uid_t* user_id) {
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("[exctl] stat failed on node path");
        return -1;
    }
    *user_id = st.st_uid;
    return 0;
}

// --- REPLACED Helper: get_desktop_file_path ---
// Now uses the node's path to find the correct user's home
static int get_desktop_file_path(const char* node_name, const char* node_path, char* buf, size_t size) {
    uid_t uid;
    if (get_user_uid_from_path(node_path, &uid) != 0) {
        fprintf(stderr, "Error: Could not stat node path: %s\n", node_path);
        return -1;
    }
    
    char home_dir[PATH_MAX];
    if (get_home_from_uid(uid, home_dir, sizeof(home_dir)) != 0) {
        fprintf(stderr, "Error: Could not find home directory for UID %d\n", (int)uid);
        return -1;
    }
    
    snprintf(buf, size, "%s/.config/autostart/exodus-guardian-%s.desktop", home_dir, node_name);
    return 0;
}

// --- REPLACED Helper: find_guardian_pid ---
static pid_t find_guardian_pid(const char* guardian_exec_path) {
    char cmd[PATH_MAX + 64];
    
    // --- FIX: Use basename() and pgrep -x ---
    // This fixes the "pgrep finds itself" bug.
    char* path_copy = strdup(guardian_exec_path);
    if (!path_copy) return 0;
    
    char* exec_name = basename(path_copy);
    if (!exec_name) {
        free(path_copy);
        return 0;
    }
    
    // Use pgrep -x for an EXACT process name match (e.g., "Site2-guardian")
    snprintf(cmd, sizeof(cmd), "pgrep -x \"%s\"", exec_name);
    free(path_copy);
    // --- END FIX ---

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return 0;
    }
    
    pid_t pid = 0;
    int n = fscanf(pipe, "%d", &pid);
    pclose(pipe);
    
    if (n == 1 && pid > 0) {
        return pid;
    }
    return 0;
}

// --- Helper: get_process_start_time ---
static time_t get_process_start_time(pid_t pid) {
    char proc_path[256];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    struct stat st;
    if (stat(proc_path, &st) == 0) return st.st_mtime;
    return 0;
}

// --- Helper: check_is_auto ---
static int check_is_auto(const char* node_path, const char* node_name) {
    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/.log/%s.conf", node_path, node_name);
    FILE* f = fopen(conf_path, "r");
    if (!f) return 0;
    char line[1024];
    int is_auto = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "auto=", 5) == 0) {
            if (strcmp(line, "auto=1\n") == 0 || strcmp(line, "auto=1") == 0) {
                is_auto = 1;
            }
            break;
        }
    }
    fclose(f);
    return is_auto;
}

// --- Helper: format_bytes ---
static void format_bytes(long kb, char* buf, size_t size) {
    if (kb < 1024) snprintf(buf, size, "%ld.0K", kb);
    else if (kb < 1024 * 1024) snprintf(buf, size, "%.1fM", (double)kb / 1024.0);
    else snprintf(buf, size, "%.1fG", (double)kb / (1024.0 * 1024.0));
}

// --- Helper: format_cpu_time ---
static void format_cpu_time(unsigned long jiffies, char* buf, size_t size) {
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    unsigned long total_ms = (jiffies * 1000) / clk_tck;
    if (total_ms < 1000) snprintf(buf, size, "%lums", total_ms);
    else snprintf(buf, size, "%.2fs", (double)total_ms / 1000.0);
}

// --- Helper: get_process_stats ---
static int get_process_stats(pid_t pid, process_stats* stats) {
    char path_buf[256];
    FILE* f;
    snprintf(path_buf, sizeof(path_buf), "/proc/%d/stat", pid);
    f = fopen(path_buf, "r");
    if (!f) return -1;
    char stat_line[1024];
    if (fgets(stat_line, sizeof(stat_line), f) == NULL) {
        fclose(f); return -1;
    }
    fclose(f);
    char* s = strrchr(stat_line, ')');
    if (!s) return -1;
    s += 2;
    int n = sscanf(s, "%c %*d %*d %*d %*d %*d %*d %*d %*d %*d %lu %lu",
           &stats->state, &stats->utime, &stats->stime);
    if (n != 3) stats->state = '?';
    
    snprintf(path_buf, sizeof(path_buf), "/proc/%d/status", pid);
    f = fopen(path_buf, "r");
    if (!f) return -1;
    char line_buf[256];
    stats->tasks = 0; stats->vmrss_kb = 0;
    while(fgets(line_buf, sizeof(line_buf), f)) {
        if (strncmp(line_buf, "Threads:", 8) == 0) sscanf(line_buf, "Threads: %d", &stats->tasks);
        else if (strncmp(line_buf, "VmRSS:", 6) == 0) sscanf(line_buf, "VmRSS: %ld kB", &stats->vmrss_kb);
        if (stats->tasks > 0 && stats->vmrss_kb > 0) break;
    }
    fclose(f);
    return 0;
}


// --- Core Function: print_status_for_node ---
void print_status_for_node(const char* node_name, const char* node_path) {
    char guardian_exec_path[PATH_MAX];
    char desktop_file_path[PATH_MAX];
    
    get_guardian_path(node_path, node_name, guardian_exec_path, sizeof(guardian_exec_path));

    printf("%s●%s exodus-guardian-%s.desktop - Exodus Self-Surveillance Guardian for %s\n",
           C_BOLD, C_RESET, node_name, node_name);

    // --- MODIFIED CALL: Pass node_path ---
    if (get_desktop_file_path(node_name, node_path, desktop_file_path, sizeof(desktop_file_path)) == 0) {
        if (access(desktop_file_path, F_OK) == 0) {
            printf("      Loaded: loaded (%s; enabled; preset: enabled)\n", desktop_file_path);
        } else {
            printf("      Loaded: %snot-found%s (File not found: %s)\n", C_RED, C_RESET, desktop_file_path);
        }
    } else {
        printf("      Loaded: %serror%s (Could not determine home directory to find .desktop file)\n", C_RED, C_RESET);
    }

    // Active Line
    pid_t pid = find_guardian_pid(guardian_exec_path);
    if (pid > 0) {
        process_stats stats;
        if (get_process_stats(pid, &stats) != 0) {
            printf("      Active: %sinactive (dead)%s (Process vanished during check)\n", C_RED, C_RESET);
            printf("\n");
            return;
        }

        time_t start_time = get_process_start_time(pid);
        char time_buf[128] = {0};
        if (start_time > 0) strftime(time_buf, sizeof(time_buf), "%a %Y-%m-%d %H:%M:%S %Z", localtime(&start_time));
        else strcpy(time_buf, "unknown time");
        
        time_t now = time(NULL);
        double diff = difftime(now, start_time);
        
        const char* state_str;
        const char* state_color = C_GREEN;
        switch(stats.state) {
            case 'R': state_str = "active (running)"; break;
            case 'S': state_str = "active (sleeping)"; break;
            case 'D': state_str = "active (disk sleep)"; state_color = C_YELLOW; break;
            case 'T': state_str = "inactive (stopped)"; state_color = C_YELLOW; break;
            case 'Z': state_str = "inactive (zombie)"; state_color = C_RED; break;
            default:  state_str = "active (unknown)"; state_color = C_YELLOW; break;
        }
        
        printf("      Active: %s%s%s since %s; %.0fs ago\n",
               state_color, state_str, C_RESET, time_buf, diff);
        printf("    Main PID: %d (%s-guardian)\n", pid, node_name);
        
        char mem_buf[32], cpu_buf[32];
        format_bytes(stats.vmrss_kb, mem_buf, sizeof(mem_buf));
        format_cpu_time(stats.utime + stats.stime, cpu_buf, sizeof(cpu_buf));
        
        printf("       Tasks: %d (limit: 16358)\n", stats.tasks);
        printf("      Memory: %s\n", mem_buf);
        printf("         CPU: %s\n", cpu_buf);

    } else {
        printf("      Active: %sinactive (dead)%s\n", C_RED, C_RESET);
    }
    printf("\n");
}

// --- Command: run_status ---
void run_status_cmd(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: exctl status <node-name> | -a\n");
        return;
    }
    char config_path[PATH_MAX];
    if (get_config_path(config_path, sizeof(config_path)) != 0) return;
    FILE* f = fopen(config_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Could not open config file: %s\n", config_path);
        return;
    }
    fseek(f, 0, SEEK_END); long length = ftell(f); fseek(f, 0, SEEK_SET);
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(f); return; }
    fread(buffer, 1, length, f); buffer[length] = '\0'; fclose(f);
    char error_buf[256];
    ctz_json_value* root = ctz_json_parse(buffer, error_buf, sizeof(error_buf));
    free(buffer);
    if (!root) {
        fprintf(stderr, "Error parsing %s: %s\n", config_path, error_buf);
        return;
    }
    if (strcmp(argv[2], "-a") == 0) {
        int found_auto = 0;
        for (size_t i = 0; i < ctz_json_get_object_size(root); i++) {
            const char* node_name = ctz_json_get_object_key(root, i);
            ctz_json_value* node_obj = ctz_json_get_object_value(root, i);
            ctz_json_value* path_val = ctz_json_find_object_value(node_obj, "path");
            if (path_val && ctz_json_get_type(path_val) == CTZ_JSON_STRING) {
                const char* node_path = ctz_json_get_string(path_val);
                if (check_is_auto(node_path, node_name)) {
                    print_status_for_node(node_name, node_path);
                    found_auto = 1;
                }
            }
        }
        if (!found_auto) printf("No independent ('auto=1') nodes found.\n");
    } else {
        const char* node_name = argv[2];
        ctz_json_value* node_obj = ctz_json_find_object_value(root, node_name);
        if (!node_obj) {
            fprintf(stderr, "Error: Node '%s' not found in %s\n", node_name, config_path);
        } else {
            ctz_json_value* path_val = ctz_json_find_object_value(node_obj, "path");
            if (path_val && ctz_json_get_type(path_val) == CTZ_JSON_STRING) {
                print_status_for_node(node_name, ctz_json_get_string(path_val));
            } else {
                fprintf(stderr, "Error: Node '%s' has no 'path' in config.\n", node_name);
            }
        }
    }
    ctz_json_free(root);
}

// --- JSON Parsing Helper ---
static const char* get_node_path_from_config(const char* node_name, const char* config_path) {
    FILE* f = fopen(config_path, "rb");
    if (!f) { fprintf(stderr, "Error: Could not open config file: %s\n", config_path); return NULL; }
    fseek(f, 0, SEEK_END); long length = ftell(f); fseek(f, 0, SEEK_SET);
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(f); return NULL; }
    fread(buffer, 1, length, f); buffer[length] = '\0'; fclose(f);
    
    static char node_path_buffer[PATH_MAX]; // Static buffer for return value
    const char* result = NULL;
    
    ctz_json_value* root = ctz_json_parse(buffer, NULL, 0);
    if (root) {
        ctz_json_value* node_obj = ctz_json_find_object_value(root, node_name);
        if (node_obj) {
            ctz_json_value* path_val = ctz_json_find_object_value(node_obj, "path");
            if (path_val && ctz_json_get_type(path_val) == CTZ_JSON_STRING) {
                strncpy(node_path_buffer, ctz_json_get_string(path_val), sizeof(node_path_buffer) - 1);
                result = node_path_buffer;
            }
        }
        ctz_json_free(root);
    }
    free(buffer);
    if (!result) fprintf(stderr, "Error: Node '%s' or its path not found in config.\n", node_name);
    return result;
}

// --- Command: run_start ---
void run_start_cmd(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: exctl start <node-name>\n");
        return;
    }
    const char* node_name = argv[2];
    char config_path[PATH_MAX];
    if (get_config_path(config_path, sizeof(config_path)) != 0) return;

    const char* node_path = get_node_path_from_config(node_name, config_path);
    if (!node_path) return;

    char guardian_exec_path[PATH_MAX];
    get_guardian_path(node_path, node_name, guardian_exec_path, sizeof(guardian_exec_path));

    if (find_guardian_pid(guardian_exec_path) > 0) {
        printf("Guardian for '%s' is already running.\n", node_name);
        return;
    }
    if (access(guardian_exec_path, X_OK) != 0) {
        fprintf(stderr, "Error: Guardian executable not found or not executable:\n%s\n", guardian_exec_path);
        fprintf(stderr, "Run 'exodus node-conf %s --auto 1' to create it.\n", node_name);
        return;
    }
    
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0) exit(1);
        close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
        
        // --- FIX: Drop privileges before exec ---
        uid_t uid;
        if (get_user_uid_from_path(node_path, &uid) == 0) {
            if (setuid(uid) != 0) {
                 // Log to syslog maybe? can't print to stderr
                 exit(1); // Fail if we can't drop privileges
            }
        }
        // --- END FIX ---
        
        execl(guardian_exec_path, guardian_exec_path, (char*)NULL);
        exit(1);
    } else if (pid < 0) {
        perror("fork failed");
    } else {
        printf("Started guardian for '%s'.\n", node_name);
        sleep(1);
        print_status_for_node(node_name, node_path);
    }
}

// --- Command: run_stop ---
void run_stop_cmd(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: exctl stop <node-name>\n");
        return;
    }
    const char* node_name = argv[2];
    char config_path[PATH_MAX];
    if (get_config_path(config_path, sizeof(config_path)) != 0) return;

    const char* node_path = get_node_path_from_config(node_name, config_path);
    if (!node_path) return;

    char guardian_exec_path[PATH_MAX];
    get_guardian_path(node_path, node_name, guardian_exec_path, sizeof(guardian_exec_path));

    if (find_guardian_pid(guardian_exec_path) == 0) {
        printf("Guardian for '%s' is already stopped.\n", node_name);
        return;
    }
    

    char* path_copy = strdup(guardian_exec_path);
    if (!path_copy) return;
    char* exec_name = basename(path_copy);
    
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "pkill -x \"%s\"", exec_name);
    free(path_copy);

    if (system(cmd) == 0) {
        printf("Sent stop signal to guardian for '%s'.\n", node_name);
    } else {
        // This can happen if pgrep found it but pkill was too slow
        printf("Guardian for '%s' was running but 'pkill' failed. It may have stopped.\n", node_name);
    }
    
    sleep(1);
    print_status_for_node(node_name, node_path);
}

// --- Main ---
void print_usage() {
    fprintf(stderr, "Usage: exctl <command> [args...]\n\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  status <node-name>    Check the status of a specific guardian.\n");
    fprintf(stderr, "  status -a             Check the status of all 'auto' guardians.\n");
    fprintf(stderr, "  start <node-name>     Manually start a guardian process.\n");
    fprintf(stderr, "  stop <node-name>      Manually stop a guardian process.\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    if (strcmp(argv[1], "status") == 0) run_status_cmd(argc, argv);
    else if (strcmp(argv[1], "start") == 0) run_start_cmd(argc, argv);
    else if (strcmp(argv[1], "stop") == 0) run_stop_cmd(argc, argv);
    else {
        print_usage();
        return 1;
    }
    return 0;
}