#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>

#define MAX_PATH 4096
#define LINE_BUF 256

// Function to check if a string is a number (for process IDs)
int is_number(const char *s) {
    if (s == NULL || *s == '\0') return 0;
    for (; *s; s++) {
        if (!isdigit(*s)) return 0;
    }
    return 1;
}

// Gets the process name for a given PID
void get_proc_name(const char* pid, char* name_buf, size_t buf_size) {
    char comm_path[MAX_PATH];
    snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", pid);
    FILE* f = fopen(comm_path, "r");
    if (f) {
        fscanf(f, "%255s", name_buf);
        fclose(f);
    } else {
        strncpy(name_buf, "unknown", buf_size);
    }
}

// Check 1: Scan for processes running from suspicious locations
int check_suspicious_processes() {
    printf("### Scanning for processes in suspicious locations...\n");
    int found_count = 0;
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        perror("Failed to open /proc");
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        if (entry->d_type == DT_DIR && is_number(entry->d_name)) {
            char exe_link[MAX_PATH];
            char real_path[MAX_PATH] = {0};

            snprintf(exe_link, sizeof(exe_link), "/proc/%s/exe", entry->d_name);
            ssize_t len = readlink(exe_link, real_path, sizeof(real_path) - 1);
            
            if (len != -1) {
                real_path[len] = '\0';
                // Define what we consider a "suspicious" path
                if (strncmp(real_path, "/tmp/", 5) == 0 || strncmp(real_path, "/var/tmp/", 9) == 0) {
                    char proc_name[256] = {0};
                    get_proc_name(entry->d_name, proc_name, sizeof(proc_name));
                    printf("  [!] Suspicious process found:\n");
                    printf("      > PID: %s\n", entry->d_name);
                    printf("      > Name: %s\n", proc_name);
                    printf("      > Path: %s\n", real_path);
                    found_count++;
                }
            }
        }
    }
    closedir(proc_dir);

    if (found_count == 0) {
        printf("  > No processes found in /tmp/ or /var/tmp/.\n");
    }
    return found_count;
}


// Check 2: Scan for unexpected listening TCP ports
int check_listening_ports() {
    printf("\n### Scanning for listening TCP ports...\n");
    int found_count = 0;
    FILE *tcp_file = fopen("/proc/net/tcp", "r");
    if (!tcp_file) {
        perror("Failed to open /proc/net/tcp");
        return 0;
    }

    char line[LINE_BUF];
    fgets(line, sizeof(line), tcp_file); // Skip header line

    while (fgets(line, sizeof(line), tcp_file)) {
        unsigned long local_addr, rem_addr;
        int local_port, state, uid, inode;
        
        // Parse the line from /proc/net/tcp
        // We only care about the local address, state, and inode
        sscanf(line, "%*d: %8lX:%4X %*8lX:%*X %2X %*lX:%*lX %*d:%*lX %*lX %d %*d %d",
               &local_addr, &local_port, &state, &uid, &inode);

        // State '0A' means LISTEN
        if (state == 0x0A) {
             printf("  [!] Found listening port: %d (UID: %d)\n", local_port, uid);
             found_count++;
        }
    }
    fclose(tcp_file);

    if (found_count == 0) {
        printf("  > No active TCP listeners found.\n");
    }
    return found_count;
}

int main() {
    printf("--- Starting System Anomaly Scan ---\n\n");
    
    int suspicious_procs = check_suspicious_processes();
    int listening_ports = check_listening_ports();

    printf("\n--- Scan Complete ---\n");
    if (suspicious_procs > 0 || listening_ports > 0) {
        printf("Result: YES, suspicious activity was detected. Please review the report above.\n");
    } else {
        printf("Result: NO, the system appears to be safe based on these checks.\n");
    }

    return 0;
}