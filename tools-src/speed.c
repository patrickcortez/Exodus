/*
 * speed.c
 * An advanced, self-contained internet speed test tool in C.
 *
 * This "v6" version is updated to use 1GB (or 1000MB) test files.
 * This is necessary because 100MB files are too small for modern
 * high-speed connections, causing the test to finish too quickly
 * (in < 1 second), which breaks the measurement.
 *
 * COMPILE:
 * gcc -o speed speed.c -O2 -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h> // For pow() and sqrt()

// --- Configuration ---
#define BUFFER_SIZE 8192
#define MAX_SPEED_SAMPLES 2048 // Increased samples for longer test
#define TRIM_PERCENTAGE 0.1
#define LATENCY_TEST_COUNT 5

// --- Server Definitions ---
typedef struct {
    const char* name;
    const char* host;
    const char* file_path;  // <-- NOW CONTAINS 1GB/1000MB FILES
    const char* fallback_ip;
    double latency_avg;
    double latency_min;
    double latency_max;
    double latency_jitter;
    double packet_loss;
} TestServer;

// UPDATED to use 1GB files for a more accurate, longer test.
TestServer g_servers[] = {
    // Asia
    {"Leaseweb (SG)", "mirror.sg.leaseweb.net", "/1000mb.bin", "103.19.131.130", -1.0},
    {"Leaseweb (HK)", "mirror.hk.leaseweb.net", "/1000mb.bin", "103.19.133.130", -1.0},
    {"IIJ (JP)", "ftp.iij.ad.jp", "/pub/speedtest/1GB.dat", "202.232.1.240", -1.0},
    // Australia
    {"Leaseweb (AU)", "mirror.syd1.au.leaseweb.net", "/1000mb.bin", "103.19.135.130", -1.0},
    // Europe
    {"Hetzner (DE)", "speed.hetzner.de", "/1GB.bin", "188.40.24.2", -1.0},
    {"Leaseweb (NL)", "mirror.nl.leaseweb.net", "/1000mb.bin", "5.79.100.34", -1.0},
    {"OVH (FR)", "proof.ovh.net", "/files/1G.dat", "188.165.12.106", -1.0},
    {"Tele2 (SE)", "speedtest.tele2.net", "/1GB.zip", "130.244.1.30", -1.0}, // Replaced ThinkBroadband
    // USA
    {"Leaseweb (US-E)", "mirror.wdc1.us.leaseweb.net", "/1000mb.bin", "104.156.90.130", -1.0},
    {"Leaseweb (US-W)", "mirror.sfo12.us.leaseweb.net", "/1000mb.bin", "104.245.32.190", -1.0}
};
int g_num_servers = sizeof(g_servers) / sizeof(g_servers[0]);

// --- Utility Functions ---

long long get_time_millis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

int compare_doubles(const void* a, const void* b) {
    if (*(double*)a > *(double*)b) return 1;
    if (*(double*)a < *(double*)b) return -1;
    return 0;
}

int compare_servers(const void* a, const void* b) {
    TestServer* s1 = (TestServer*)a;
    TestServer* s2 = (TestServer*)b;
    if (s1->latency_avg < 0) return 1;
    if (s2->latency_avg < 0) return -1;
    if (s1->latency_avg > s2->latency_avg) return 1;
    if (s1->latency_avg < s2->latency_avg) return -1;
    return 0;
}

int simple_json_parse(const char* json, const char* key, char* out_buf, size_t out_len) {
    char key_buf[256];
    snprintf(key_buf, sizeof(key_buf), "\"%s\": \"", key);
    
    const char* key_start = strstr(json, key_buf);
    if (!key_start) return 0;
    
    key_start += strlen(key_buf);
    
    const char* val_end = strchr(key_start, '"');
    if (!val_end) return 0;
    
    size_t val_len = val_end - key_start;
    if (val_len >= out_len) {
        val_len = out_len - 1;
    }
    
    memcpy(out_buf, key_start, val_len);
    out_buf[val_len] = '\0';
    return 1;
}

// --- Network & Logic Functions ---

int get_user_info(char* out_ip, size_t ip_len, char* out_isp, size_t isp_len, char* out_city, size_t city_len) {
    int sock_fd;
    struct sockaddr_in serv_addr;
    struct hostent* host_entry;
    char buffer[4096];
    
    host_entry = gethostbyname("ipinfo.io");
    if (host_entry == NULL) return 0;

    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 0;

    memcpy(&serv_addr.sin_addr.s_addr, host_entry->h_addr, host_entry->h_length);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(80);

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock_fd);
        return 0;
    }
    
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    const char* request = "GET /json HTTP/1.1\r\nHost: ipinfo.io\r\nUser-Agent: C-SpeedTest\r\nConnection: close\r\n\r\n";
    if (send(sock_fd, request, strlen(request), 0) < 0) {
        close(sock_fd);
        return 0;
    }

    int total_read = 0;
    int n = 0;
    while ((n = read(sock_fd, buffer + total_read, sizeof(buffer) - total_read - 1)) > 0) {
        total_read += n;
    }
    close(sock_fd);
    buffer[total_read] = '\0';
    
    const char* json_body = strstr(buffer, "\r\n\r\n");
    if (!json_body) return 0;
    json_body += 4;
    
    int ip_ok = simple_json_parse(json_body, "ip", out_ip, ip_len);
    int isp_ok = simple_json_parse(json_body, "org", out_isp, isp_len);
    int city_ok = simple_json_parse(json_body, "city", out_city, city_len);

    return (ip_ok && isp_ok && city_ok);
}

void test_server_latency(TestServer* server) {
    int sock_fd;
    struct sockaddr_in serv_addr;
    struct hostent* host_entry;

    printf("  Testing: %-25s ", server->name);
    fflush(stdout);

    host_entry = gethostbyname(server->host);
    if (host_entry == NULL) {
        if (inet_pton(AF_INET, server->fallback_ip, &serv_addr.sin_addr) <= 0) {
            printf("FAIL (DNS & Fallback IP)\n");
            server->latency_avg = -1.0;
            return;
        }
    } else {
        memcpy(&serv_addr.sin_addr.s_addr, host_entry->h_addr, host_entry->h_length);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(80);

    double latencies[LATENCY_TEST_COUNT];
    int success_count = 0;
    double sum = 0.0;
    
    server->latency_min = 1000000.0;
    server->latency_max = 0.0;

    for (int i = 0; i < LATENCY_TEST_COUNT; i++) {
        if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            continue;
        }
        
        struct timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

        long long start_time = get_time_millis();
        if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock_fd);
            continue;
        }
        long long end_time = get_time_millis();
        close(sock_fd);

        double latency = (double)(end_time - start_time);
        latencies[success_count] = latency;
        success_count++;
        sum += latency;
        
        if (latency < server->latency_min) server->latency_min = latency;
        if (latency > server->latency_max) server->latency_max = latency;
        
        usleep(50000);
    }

    server->packet_loss = 1.0 - ((double)success_count / LATENCY_TEST_COUNT);
    if (success_count == 0) {
        printf("FAIL (Connect) (%.0f%% loss)\n", server->packet_loss * 100.0);
        server->latency_avg = -1.0;
        return;
    }

    server->latency_avg = sum / success_count;

    double sum_sq_diff = 0.0;
    for (int i = 0; i < success_count; i++) {
        sum_sq_diff += pow(latencies[i] - server->latency_avg, 2);
    }
    server->latency_jitter = sqrt(sum_sq_diff / success_count);

    printf("OK (avg: %.2fms, jitter: %.2fms, loss: %.0f%%)\n",
           server->latency_avg, server->latency_jitter, server->packet_loss * 100.0);
}

int connect_for_download(TestServer* server) {
    int sock_fd;
    struct sockaddr_in serv_addr;
    struct hostent* host_entry;

    host_entry = gethostbyname(server->host);
    if (host_entry == NULL) {
        if (inet_pton(AF_INET, server->fallback_ip, &serv_addr.sin_addr) <= 0) {
            fprintf(stderr, "Error: Host DNS and fallback IP failed for %s.\n", server->host);
            return -1;
        }
    } else {
        memcpy(&serv_addr.sin_addr.s_addr, host_entry->h_addr, host_entry->h_length);
    }

    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error: Socket creation failed");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(80);

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error: Connection failed");
        close(sock_fd);
        return -1;
    }
    
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    
    return sock_fd;
}

double calculate_trimmed_mean(double* samples, int count) {
    if (count == 0) return 0.0;
    qsort(samples, count, sizeof(double), compare_doubles);
    int trim_count = (int)(count * TRIM_PERCENTAGE);
    double sum = 0.0;
    int samples_to_use = 0;
    for (int i = trim_count; i < count - trim_count; i++) {
        sum += samples[i];
        samples_to_use++;
    }
    if (samples_to_use == 0) return (count > 0 ? samples[0] : 0.0);
    return sum / samples_to_use;
}

int perform_download_test(int sock_fd, TestServer* server) {
    char request[1024];
    char buffer[BUFFER_SIZE];
    double speed_samples[MAX_SPEED_SAMPLES];
    int sample_count = 0;
    
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: C-SpeedTest/1.0\r\n"
             "Connection: close\r\n\r\n",
             server->file_path, server->host);

    if (send(sock_fd, request, strlen(request), 0) < 0) {
        perror("Error: Failed to send request");
        return 0;
    }

    char* header_end = NULL;
    int header_len = 0;
    int total_header_bytes = 0;
    while ((header_len = read(sock_fd, buffer, BUFFER_SIZE - 1)) > 0) {
        total_header_bytes += header_len;
        buffer[header_len] = '\0';
        header_end = strstr(buffer, "\r\n\r\n");
        if (header_end) {
            header_end += 4;
            break;
        }
        if (total_header_bytes > 8192) {
             fprintf(stderr, "Error: Failed to find HTTP headers.\n");
             return 0;
        }
    }
    if (header_len <= 0) {
        perror("Error: Failed to read from socket (during headers)");
        return 0;
    }

    long long total_bytes = 0;
    int bytes_read = 0;
    total_bytes = header_len - (int)(header_end - buffer);

    long long start_time = get_time_millis();
    long long last_report_time = start_time;
    long long bytes_at_last_report = 0;

    while ((bytes_read = read(sock_fd, buffer, BUFFER_SIZE)) > 0) {
        total_bytes += bytes_read;
        long long now = get_time_millis();
        
        double time_since_last = (now - last_report_time) / 1000.0;
        if (time_since_last > 0.5) {
            long long bytes_diff = total_bytes - bytes_at_last_report;
            double mbps = (bytes_diff * 8) / (1000000.0 * time_since_last);

            if (sample_count < MAX_SPEED_SAMPLES) {
                speed_samples[sample_count++] = mbps;
            }

            printf("\rDownload: %6.2f Mbps  (%.1f MB)", mbps, (double)total_bytes / 1048576.0);
            fflush(stdout);

            last_report_time = now;
            bytes_at_last_report = total_bytes;
        }
    }
    
    if (bytes_read < 0) {
        perror("\nError: Socket read error during download");
        return 0;
    }
    
    long long end_time = get_time_millis();
    double total_duration_s = (end_time - start_time) / 1000.0;
    
    if (total_duration_s < 1.0) { // This check is now safe, as 1GB will take > 1s
        fprintf(stderr, "\nDownload was too fast or too small to measure.\n");
        return 0;
    }

    double final_mbps_trimmed = calculate_trimmed_mean(speed_samples, sample_count);
    
    printf("\rDownload: %6.2f Mbps  (%.1f MB data used)\n", final_mbps_trimmed, (double)total_bytes / 1048576.0);
    
    printf("\n");
    
    return 1;
}

// --- Main Function ---
int main() {
    char user_ip[64] = {0};
    char user_isp[128] = {0};
    char user_city[128] = {0};
    
    printf("--- Internet Speed Test ---\n");
    
    printf("Fetching your location and ISP...\n");
    if (get_user_info(user_ip, sizeof(user_ip), user_isp, sizeof(user_isp), user_city, sizeof(user_city))) {
        printf("  ISP:    %s\n", user_isp);
        printf("  City:   %s\n", user_city);
        printf("  IP:     %s\n", user_ip);
    } else {
        printf("  Could not determine your ISP and location.\n");
    }

    printf("\nFinding best server by latency (%d attempts each):\n", LATENCY_TEST_COUNT);
    for (int i = 0; i < g_num_servers; i++) {
        test_server_latency(&g_servers[i]);
    }

    qsort(g_servers, g_num_servers, sizeof(TestServer), compare_servers);
    
    TestServer* best_server = &g_servers[0];
    if (best_server->latency_avg < 0) {
        fprintf(stderr, "\nError: No reachable test servers found. Check network.\n");
        return 1;
    }

    printf("\n--- Test Configuration ---\n");
    printf("Server:         %s\n", best_server->name);
    printf("Idle Latency:   %.2f ms (jitter: %.2fms, low: %.2fms, high: %.2fms)\n",
           best_server->latency_avg, best_server->latency_jitter,
           best_server->latency_min, best_server->latency_max);
    printf("Packet Loss:    %.0f%%\n", best_server->packet_loss * 100.0);


    int download_success = 0;
    for (int i = 0; i < g_num_servers; i++) {
        if (g_servers[i].latency_avg < 0) {
            continue;
        }
        
        printf("\n--- Starting Download Test (Server %d/%d) ---\n", i + 1, g_num_servers);
        printf("Using: %s (%s) - Downloading 1GB file...\n", g_servers[i].name, g_servers[i].host);

        int sock_fd = connect_for_download(&g_servers[i]);
        if (sock_fd < 0) {
            fprintf(stderr, "Failed to connect to %s. Trying next server...\n", g_servers[i].name);
            continue;
        }
        
        if (perform_download_test(sock_fd, &g_servers[i])) {
            download_success = 1;
            close(sock_fd);
            break;
        }
        
        close(sock_fd);
        fprintf(stderr, "\nDownload from %s failed. Trying next server...\n", g_servers[i].name);
    }
    
    if (!download_success) {
        fprintf(stderr, "\nError: All test servers failed.\n");
        return 1;
    }
    
    printf("\n--- Test Complete ---\n");
    printf("(Upload test is not available with public file mirrors.)\n");

    return 0;
}