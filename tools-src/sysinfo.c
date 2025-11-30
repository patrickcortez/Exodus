/* tools/sysinfo.c
 *
 * A tool to display key system information by reading from /proc.
 *
 * Build:
 * gcc -o sysinfo sysinfo.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#define LINE_BUFFER_SIZE 256

// Helper function to read a specific field from a /proc file
void get_proc_field(const char *path, const char *field_name, char *output, size_t out_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        strncpy(output, "N/A", out_len);
        return;
    }

    char line[LINE_BUFFER_SIZE];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, field_name, strlen(field_name)) == 0) {
            char *value = strchr(line, ':');
            if (value && *(++value)) {
                // Skip leading whitespace
                while (*value == ' ' || *value == '\t') value++;
                // Trim trailing newline
                char *end = strchr(value, '\n');
                if (end) *end = '\0';
                strncpy(output, value, out_len);
                fclose(fp);
                return;
            }
        }
    }

    strncpy(output, "N/A", out_len);
    fclose(fp);
}

// Function to format uptime from seconds into a human-readable string
void format_uptime(long uptime_seconds, char *output, size_t out_len) {
    long days = uptime_seconds / (24 * 3600);
    uptime_seconds %= (24 * 3600);
    long hours = uptime_seconds / 3600;
    uptime_seconds %= 3600;
    long mins = uptime_seconds / 60;

    snprintf(output, out_len, "%ldd %ldh %ldm", days, hours, mins);
}

int main() {
    char cpu_model[LINE_BUFFER_SIZE];
    char mem_total_str[LINE_BUFFER_SIZE];
    char mem_free_str[LINE_BUFFER_SIZE];
    char uptime_str[LINE_BUFFER_SIZE];

    // 1. Get CPU Model Name
    get_proc_field("/proc/cpuinfo", "model name", cpu_model, sizeof(cpu_model));

    // 2. Get Memory Information
    get_proc_field("/proc/meminfo", "MemTotal", mem_total_str, sizeof(mem_total_str));
    get_proc_field("/proc/meminfo", "MemAvailable", mem_free_str, sizeof(mem_free_str)); // MemAvailable is often more useful than MemFree

    long mem_total_kb = atol(mem_total_str);
    long mem_free_kb = atol(mem_free_str);

    // 3. Get System Uptime
    struct sysinfo s_info;
    if (sysinfo(&s_info) == 0) {
        format_uptime(s_info.uptime, uptime_str, sizeof(uptime_str));
    } else {
        strncpy(uptime_str, "N/A", sizeof(uptime_str));
    }

    // --- Print the formatted output ---
    printf("┌─────────────────── System Information ───────────────────┐\n");
    printf("│ %-12s: %-49s │\n", "CPU", cpu_model);
    printf("│ %-12s: %ld MB / %ld MB Free                       │\n", "Memory", mem_free_kb / 1024, mem_total_kb / 1024);
    printf("│ %-12s: %-49s │\n", "Uptime", uptime_str);
    printf("└──────────────────────────────────────────────────────────┘\n");

    return 0;
}