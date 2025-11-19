/*
 * xpdate.c - Exodus Update Utility (HTTPS Enabled)
 *
 * A self-contained update tool that:
 * 1. Manually parses the local .git directory to find the current commit hash.
 * 2. Connects to a remote HTTPS server (GitHub) using OpenSSL.
 * 3. Downloads and unpacks updates without using system() or exec().
 *
 * FEATURES:
 * - Uses OpenSSL for secure HTTPS connections.
 * - Parses .git manually.
 * - Custom Tar extractor (currently a stub).
 *
 * COMPILE:
 * gcc -Wall -Wextra -O2 Xpdate.c -o Xpdate -lssl -lcrypto
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <libgen.h> // For dirname
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CONFIG_DIR_REL ".config/exodus"
#define CONFIG_FILE_NAME "xpdate.conf"
#define HTTP_PORT 80
#define HTTPS_PORT 443

// Global storage for repository details
char g_repo_owner[128] = {0};
char g_repo_name[128] = {0};

// --- Tar Header Structure ---
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} TarHeader;

// --- Utility Functions ---

void trim(char* s) {
    char* p = s;
    int l = strlen(p);
    while(l > 0 && isspace(p[l-1])) p[--l] = 0;
    while(*p && isspace(*p)) p++;
    memmove(s, p, l + 1);
}

int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

void get_config_path(char* buffer, size_t size) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buffer, size, "%s/%s/%s", home, CONFIG_DIR_REL, CONFIG_FILE_NAME);
}

// --- Git File Parsing ---

int get_local_git_hash(const char* repo_path, char* hash_buf, size_t size) {
    char head_path[PATH_MAX];
    snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", repo_path);
    
    FILE* f = fopen(head_path, "r");
    if (!f) return -1;
    
    char ref[1024];
    if (!fgets(ref, sizeof(ref), f)) { fclose(f); return -1; }
    fclose(f);
    trim(ref);
    
    if (strncmp(ref, "ref: ", 5) == 0) {
        char ref_path[PATH_MAX];
        snprintf(ref_path, sizeof(ref_path), "%s/.git/%s", repo_path, ref + 5);
        
        f = fopen(ref_path, "r");
        if (!f) return -2; 
        if (!fgets(hash_buf, size, f)) { fclose(f); return -1; }
        fclose(f);
        trim(hash_buf);
    } else {
        strncpy(hash_buf, ref, size - 1);
    }
    return 0;
}

// --- Network Client (HTTPS via OpenSSL) ---

typedef struct {
    int socket;
    SSL* ssl;
    SSL_CTX* ctx;
    int is_https;
} Connection;

void init_openssl() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

void cleanup_connection(Connection* conn) {
    if (conn->is_https && conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->socket != -1) close(conn->socket);
    if (conn->is_https && conn->ctx) SSL_CTX_free(conn->ctx);
}

int parse_url(const char* url, char* host, int* port, char* path, int* is_https) {
    const char* ptr = url;
    if (strncmp(ptr, "http://", 7) == 0) {
        ptr += 7;
        *port = 80;
        *is_https = 0;
    } else if (strncmp(ptr, "https://", 8) == 0) {
        ptr += 8;
        *port = 443;
        *is_https = 1;
    } else {
        *port = 80;
        *is_https = 0;
    }
    
    const char* p_start = strchr(ptr, '/');
    size_t host_len = p_start ? (size_t)(p_start - ptr) : strlen(ptr);
    strncpy(host, ptr, host_len);
    host[host_len] = '\0';
    
    char* port_colon = strchr(host, ':');
    if (port_colon) {
        *port_colon = '\0';
        *port = atoi(port_colon + 1);
    }
    
    if (p_start) strcpy(path, p_start);
    else strcpy(path, "/");
    
    return 0;
}

void transform_github_url(const char* input_url, char* output_url) {
    // Purpose: Parse the user/repo and return the GitHub API commit URL.
    if (strstr(input_url, "github.com") && !strstr(input_url, "api.github.com")) {
        char clean_url[256];
        strcpy(clean_url, input_url);
        char* git_ext = strstr(clean_url, ".git");
        if (git_ext) *git_ext = '\0';
        
        char* gh_start = strstr(clean_url, "github.com/");
        if (gh_start) {
            gh_start += 11;
            
            char* slash = strchr(gh_start, '/');
            if (slash) {
                // Parse owner and repo name
                *slash = '\0';
                strncpy(g_repo_owner, gh_start, sizeof(g_repo_owner) - 1);
                *slash = '/'; // Restore original character
                strncpy(g_repo_name, slash + 1, sizeof(g_repo_name) - 1);
                
                // Target the API endpoint for the main branch commit
                sprintf(output_url, "https://api.github.com/repos/%s/%s/commits/main", g_repo_owner, g_repo_name);
                return;
            }
        }
    }
    strcpy(output_url, input_url);
}

void get_download_url(const char* sha, char* output_url, size_t size) {
    // Generates the URL for downloading the full repository tarball/zip
    if (g_repo_owner[0] != 0 && g_repo_name[0] != 0) {
        snprintf(output_url, size, "https://api.github.com/repos/%s/%s/tarball/%s", g_repo_owner, g_repo_name, sha);
    } else {
        // Fallback or use original URL if parsing failed
        snprintf(output_url, size, "https://api.github.com/repos/unknown/unknown/tarball/%s", sha);
    }
}


int connect_to_host(const char* host, int port, int is_https, Connection* conn) {
    struct hostent* server = gethostbyname(host);
    if (!server) return -1;
    
    conn->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->socket < 0) return -1;
    
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    
    if (connect(conn->socket, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(conn->socket);
        return -1;
    }

    conn->is_https = is_https;
    if (is_https) {
        conn->ctx = SSL_CTX_new(TLS_client_method());
        if (!conn->ctx) return -1;
        
        SSL_CTX_set_default_verify_paths(conn->ctx);
        
        conn->ssl = SSL_new(conn->ctx);
        SSL_set_fd(conn->ssl, conn->socket);
        SSL_set_tlsext_host_name(conn->ssl, host); 

        if (SSL_connect(conn->ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            return -1;
        }
    }
    return 0;
}

int conn_write(Connection* conn, const char* buf, int len) {
    if (conn->is_https) return SSL_write(conn->ssl, buf, len);
    return write(conn->socket, buf, len);
}

int conn_read(Connection* conn, char* buf, int len) {
    if (conn->is_https) return SSL_read(conn->ssl, buf, len);
    return read(conn->socket, buf, len);
}

// Stub for TAR extraction - Must be implemented fully to work.
int unpack_tar_stream(Connection* conn, const char* dest_path) {
    printf("[INFO] Starting download to be unpacked in: %s\n", dest_path);
    printf("[STUB] *** Actual TAR extraction logic is not implemented ***\n");
    printf("[STUB] Skipping download and simulating success.\n");
    // In a real implementation, this would:
    // 1. Read chunks from conn_read(conn, ...)
    // 2. Decompress GZIP (if compressed)
    // 3. Parse TarHeader for file name and size.
    // 4. Create directories (mkdir_p) and files (write) at dest_path.
    // 5. Skip padding/blocks as necessary.
    
    // For now, we drain the socket to prevent future errors and simulate success.
    char drain_buf[4096];
    int n;
    while((n = conn_read(conn, drain_buf, sizeof(drain_buf))) > 0);

    return 0; 
}


int http_get_file(const char* url, const char* dest_path, char* version_buf, size_t v_size) {
    char host[256], path[1024];
    int port, is_https;
    
    char current_url[2048];
    strncpy(current_url, url, sizeof(current_url));
    
    int redirects = 0;
    while (redirects < 5) {
        if (parse_url(current_url, host, &port, path, &is_https) != 0) return -1;
        
        Connection conn = {0};
        if (connect_to_host(host, port, is_https, &conn) != 0) return -1;
        
        // Add specialized Accept header for GitHub API (SHA) or download
        char request[2048];
        const char* accept_header = "";
        
        if (strstr(host, "api.github.com") && dest_path == NULL) {
            // Request SHA format for version check
            accept_header = "Accept: application/vnd.github.v3.sha\r\n";
        } else if (strstr(host, "api.github.com") && dest_path != NULL) {
            // Request tarball for download
            accept_header = "Accept: application/vnd.github.v3.raw\r\n";
        }

        snprintf(request, sizeof(request), 
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: Xpdate/1.0\r\n"
            "%s" 
            "Connection: close\r\n\r\n", 
            path, host, accept_header);
        
        conn_write(&conn, request, strlen(request));
        
        char buffer[4096];
        int total_read = 0;
        int headers_end_pos = -1;

        // Read headers fully
        while (total_read < (int)sizeof(buffer) - 1) {
            int n = conn_read(&conn, buffer + total_read, sizeof(buffer) - 1 - total_read);
            if (n <= 0) break;
            total_read += n;
            buffer[total_read] = 0;

            char *end_ptr = strstr(buffer, "\r\n\r\n");
            if (end_ptr) {
                headers_end_pos = end_ptr - buffer;
                break;
            }
        }

        if (headers_end_pos == -1) {
            printf("[ERROR] Incomplete headers received.\n");
            cleanup_connection(&conn);
            return -1;
        }
        
        // Check for Redirects
        if (strncmp(buffer, "HTTP/1.1 30", 11) == 0) {
            char* loc = strstr(buffer, "Location: ");
            if (loc) {
                loc += 10;
                char* end = strstr(loc, "\r\n");
                if (end) *end = 0;
                strncpy(current_url, loc, sizeof(current_url));
                redirects++;
                cleanup_connection(&conn);
                continue;
            }
        }

        if (strstr(buffer, " 200 ") == NULL) {
            // Extract status line for cleaner output
            char *line_end = strstr(buffer, "\r\n");
            if (line_end) *line_end = 0;
            // The caller (main) will print the failure message.
            cleanup_connection(&conn);
            return -1;
        }
        
        char* body = buffer + headers_end_pos + 4;
        
        // --- CONTENT PROCESSING ---
        if (version_buf) {
            // Extract SHA (for version check)
            strncpy(version_buf, body, v_size - 1);
            trim(version_buf);
            cleanup_connection(&conn);
            return 0;
        }
        
        if (dest_path) {
            // Download/Unpack (for update execution)
            
            // First, process the partial body already read (buffer)
            int body_len = total_read - (headers_end_pos + 4);
            // This would normally be fed into the extraction logic first, 
            // then the rest streamed from conn.
            
            // Since unpack_tar_stream is a stub, we pass the connection directly
            // and let it handle the streaming reads.
            int result = unpack_tar_stream(&conn, dest_path);
            
            cleanup_connection(&conn);
            return result;
        }
        
        cleanup_connection(&conn);
        return 0;
    }
    return -1;
}

// --- Main ---

int main() {
    init_openssl();
    
    char conf_path[PATH_MAX];
    get_config_path(conf_path, sizeof(conf_path));
    
    FILE* f = fopen(conf_path, "r");
    if (!f) {
        fprintf(stderr, "[ERROR] Configuration file missing: %s\n", conf_path);
        fprintf(stderr, "[INFO] Please configure manually or run setup.\n");
        return 1;
    }

    char repo_path[PATH_MAX] = {0};
    char base_url[256] = {0};
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (strncmp(line, "REPO=", 5) == 0) strcpy(repo_path, line + 5);
        if (strncmp(line, "URL=", 4) == 0) strcpy(base_url, line + 4);
    }
    fclose(f);

    if (repo_path[0] == 0 || base_url[0] == 0) {
        fprintf(stderr, "[ERROR] Configuration incomplete. Check REPO= and URL= in config.\n");
        return 1;
    }

    // 1. Local Hash
    char local_hash[128] = {0};
    if (get_local_git_hash(repo_path, local_hash, sizeof(local_hash)) != 0) {
        fprintf(stderr, "[ERROR] Local repository access failed. Is '%s' a git clone?\n", repo_path);
        return 1;
    }
    printf("Exodus Update Utility (xpdate)\n");
    printf("----------------------------------------\n");
    printf("[STATUS] Current Local Revision: %s\n", local_hash);

    // 2. Remote Hash URL Generation (Uses GitHub API)
    char api_url[1024];
    transform_github_url(base_url, api_url);

    printf("[INFO] Checking for Remote Revision...\n");
    char remote_hash[128] = {0};
    
    // First attempt (usually 'main')
    if (http_get_file(api_url, NULL, remote_hash, sizeof(remote_hash)) != 0) {
        // Fallback logic: If main failed, try master.
        if (strstr(api_url, "/main")) {
            printf("[INFO] Branch 'main' not found. Retrying with 'master'...\n");
            char* pos = strstr(api_url, "/main");
            if (pos) memcpy(pos, "/master", 7); // Replace /main with /master
            
            if (http_get_file(api_url, NULL, remote_hash, sizeof(remote_hash)) != 0) {
                 fprintf(stderr, "[ERROR] Failed to retrieve remote revision from both 'main' and 'master'.\n");
                 return 1;
            }
        } else {
            fprintf(stderr, "[ERROR] Failed to retrieve remote revision.\n");
            return 1;
        }
    }
    
    printf("[STATUS] Latest Remote Revision: %s\n", remote_hash);
    printf("----------------------------------------\n");
    
    if (strcmp(local_hash, remote_hash) == 0) {
        printf("[SUCCESS] Your local repository is up to date.\n");
    } else {
        printf("[UPDATE] New version found. Commencing download.\n");

        // 3. Download and Update Logic
        char download_url[1024];
        get_download_url(remote_hash, download_url, sizeof(download_url));
        
        char repo_path_copy[PATH_MAX];
        strncpy(repo_path_copy, repo_path, sizeof(repo_path_copy));
        
        // Get the parent directory (dirname modifies its argument)
        char* parent_dir = dirname(repo_path_copy); 
        
        printf("[INFO] Downloading from: %s\n", download_url);
        printf("[INFO] Extracting to parent directory: %s\n", parent_dir);

        // Perform the download and extraction
        if (http_get_file(download_url, parent_dir, NULL, 0) != 0) {
            fprintf(stderr, "[ERROR] Download or extraction failed.\n");
            return 1;
        }

        printf("[SUCCESS] Update complete. New files extracted to %s/\n", parent_dir);
        printf("[WARNING] Note: The extraction logic in this utility is currently a stub.\n");
        printf("[WARNING] Please ensure the 'unpack_tar_stream' function is fully implemented.\n");
    }

    return 0;
}