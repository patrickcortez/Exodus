/*
 * exodus_snapshot_delta.c - Anchor-Weave object storage model
 *
 * COMPILE:
 * gcc -Wall -Wextra -O2 exodus-anchor-weave.c cortez_ipc.o ctz-json.a -o exodus_snapshot -lz -lm
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <ftw.h>
#include <stddef.h>
#include <fcntl.h>
#include <time.h>
#include <utime.h>
#include <sys/time.h>
#include <zlib.h>
#include <math.h>
#include <stdint.h>
#include <sys/mman.h>

#include "cortez_ipc.h"
#include "ctz-json.h"

// --- Forward Declarations ---
static char* read_object(const char* hash, size_t* uncompressed_size);
static int find_file_in_tree(const char* current_tree_hash, const char* path_to_find, 
                             char* blob_hash_out, mode_t* mode_out, 
                             double* entropy_out, char* type_out);
static void free_lcs_matrix(int** matrix, int rows);


#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define HASH_LEN 64
#define HASH_STR_LEN (HASH_LEN + 1)

// ANSI Color Codes
#define C_GREEN "\033[0;32m"
#define C_RED "\033[0;31m"
#define C_CYAN "\033[0;36m"
#define C_YELLOW "\033[0;33m"
#define C_RESET "\033[0m"

// The fixed block size (like rsync/git). 4KB is a good balance.
#define DELTA_BLOCK_SIZE 4096 

// A prime for the Adler-32 rolling hash
#define ADLER_MOD 65521

// The number of buckets in our hash map. A prime number is good.
#define HASH_MAP_BUCKETS 16381

#define IN_MEMORY_FILE_LIMIT (512 * 1024 * 1024)

#define ZLIB_CHUNK_SIZE 16384

#define DECONSTRUCT_THRESHOLD (5LL * 1024 * 1024 * 1024)

// --- Structs for LINE-BASED DIFF (for 'diff' command) ---

// Represents one line in a text file
typedef struct TextLine {
    char* line;
    size_t len;
    struct TextLine* next;
} TextLine;

// Represents a single diff operation
typedef enum { DIFF_ADD, DIFF_DEL, DIFF_SAME } DiffOpType;
typedef struct DiffOp {
    DiffOpType type;
    char* content;
    struct DiffOp* next;
} DiffOp;

// --- Structs for BYTE-LEVEL DELTA (for Storage) ---

/**
 * @brief A dynamic buffer for building the binary delta script.
 */
typedef struct DeltaScript {
    char* buffer;
    size_t size;
    size_t capacity;
} DeltaScript;

// --- SHA-256 (Embedded, UNCHANGED) ---
#define SHA256_BLOCK_SIZE 32
typedef struct {
	uint8_t  data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
} SHA256_CTX;

// Ensure structs are packed to match on-disk format
#pragma pack(push, 1)

/**
 * @brief Standard EBOF v4 Header
 */
typedef struct EBOFv4Header {
    uint32_t magic;         // 0xE7B0B0E8
    uint16_t version;       // 0x0400 (v4)
    uint16_t type;          // 0x0010 for BBLK, 0x0011 for MOBJ
    uint64_t payload_size;  // Size of the data *after* this header
} EBOFv4Header;

// Magic numbers for our new object types
#define EBOF_MAGIC 0xE7B0B0E8
#define EBOF_VERSION 0x0400
#define EBOF_TYPE_BBLK 0x0010
#define EBOF_TYPE_MOBJ 0x0011

/**
 * @brief Header for a Binary Block (.bblk) file
 * This struct follows *immediately* after the EBOFv4Header.
 */
typedef struct BinaryBlockHeader {
    uint8_t  parent_block_hash[32]; // Optional, raw 32-byte SHA-256
    float    entropy_score;         //
    uint64_t original_offset;       //
    uint64_t original_length;       //
    uint32_t crc32_checksum;        //
} BinaryBlockHeader;

/**
 * @brief Header for a Manifest Object (.mobj) file
 * This struct follows *immediately* after the EBOFv4Header.
 */
typedef struct ManifestObjectHeader {
    uint16_t file_path_len;     //
    // file_path data follows this header
    uint32_t file_mode;         //
    uint64_t total_size;        //
    uint32_t block_count;       //
    // block_entries data follows this
    float    entropy_mean;      //
    uint8_t  file_signature[64]; // Ed25519 signature (stubbed for now)
} ManifestObjectHeader;

/**
 * @brief On-disk entry for a single block in a manifest
 */
typedef struct ManifestBlockEntry {
    uint8_t  block_hash[32];    // Raw 32-byte SHA-256
    uint64_t offset;            //
    uint64_t length;            //
} ManifestBlockEntry;

#pragma pack(pop)
/**
 * @brief In-memory representation of a manifest for easier use.
 * This is what read_mobj_object() will return.
 */
typedef struct ManifestData {
    char* file_path;
    uint32_t file_mode;
    uint64_t total_size;
    uint32_t block_count;
    ManifestBlockEntry* blocks; // Array of block_count entries
    float entropy_mean;
    uint8_t file_signature[64];
} ManifestData;

static ManifestData* read_mobj_object(const char* hash_hex);

#define DBL_INT_ADD(a,b,c) if (a > 0xffffffff - (c)) ++(b); a += c;
#define ROTLEFT(a,b)  (((a) << (b)) | ((a) >> (32-(b))))
#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))
// --- NEW: SBDS Deconstruction (CDC) Defines ---
#define CDC_WINDOW_SIZE 4096      // The rolling hash window size
#define CDC_MIN_BLOCK   2048      // 2KB
#define CDC_MAX_BLOCK   (64 * 1024) // 64KB
#define CDC_TARGET_BITS 13        // Avg block size = 2^13 = 8KB
#define CDC_MASK        ((1 << CDC_TARGET_BITS) - 1)
#define CDC_TARGET      CDC_MASK
static const uint32_t k[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
	uint32_t a,b,c,d,e,f,g,h,i,j,t1,t2,m[64];
	for (i=0,j=0; i < 16; ++i, j += 4)
		m[i] = (data[j] << 24) | (data[j+1] << 16) | (data[j+2] << 8) | (data[j+3]);
	for ( ; i < 64; ++i)
		m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
	for (i = 0; i < 64; ++i) {
		t1 = h + EP1(e) + CH(e,f,g) + k[i] + m[i];
		t2 = EP0(a) + MAJ(a,b,c);
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}
static void sha256_init(SHA256_CTX *ctx) {
	ctx->datalen = 0;
	ctx->bitlen = 0;
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) {
	uint32_t i;
	for (i=0; i < len; ++i) {
		ctx->data[ctx->datalen] = data[i];
		ctx->datalen++;
		if (ctx->datalen == 64) {
			sha256_transform(ctx,ctx->data);
			DBL_INT_ADD(ctx->bitlen,ctx->bitlen,512);
			ctx->datalen = 0;
		}
	}
}
static void sha256_final(SHA256_CTX *ctx, uint8_t hash[]) {
	uint32_t i;
	i = ctx->datalen;
	if (ctx->datalen < 56) {
		ctx->data[i++] = 0x80;
		while (i < 56) ctx->data[i++] = 0x00;
	}
	else {
		ctx->data[i++] = 0x80;
		while (i < 64) ctx->data[i++] = 0x00;
		sha256_transform(ctx,ctx->data);
		memset(ctx->data,0,56);
	}
	DBL_INT_ADD(ctx->bitlen,ctx->bitlen,ctx->datalen * 8);
	ctx->data[63] = ctx->bitlen;
	ctx->data[62] = ctx->bitlen >> 8;
	ctx->data[61] = ctx->bitlen >> 16;
	ctx->data[60] = ctx->bitlen >> 24;
	ctx->data[59] = ctx->bitlen >> 32;
	ctx->data[58] = ctx->bitlen >> 40;
	ctx->data[57] = ctx->bitlen >> 48;
	ctx->data[56] = ctx->bitlen >> 56;
	sha256_transform(ctx,ctx->data);
	for (i=0; i < 4; ++i) {
		hash[i]    = (ctx->state[0] >> (24-i*8)) & 0xff;
		hash[i+4]  = (ctx->state[1] >> (24-i*8)) & 0xff;
		hash[i+8]  = (ctx->state[2] >> (24-i*8)) & 0xff;
		hash[i+12] = (ctx->state[3] >> (24-i*8)) & 0xff;
		hash[i+16] = (ctx->state[4] >> (24-i*8)) & 0xff;
		hash[i+20] = (ctx->state[5] >> (24-i*8)) & 0xff;
		hash[i+24] = (ctx->state[6] >> (24-i*8)) & 0xff;
		hash[i+28] = (ctx->state[7] >> (24-i*8)) & 0xff;
	}
}
// --- END SHA-256 ---

// For building tree objects in memory
typedef struct TreeEntry {
    char name[NAME_MAX + 1];
    mode_t mode;
    char type; // 'B' (blob), 'T' (tree), 'L' (link), 'M' (Manifest)
    char hash[HASH_STR_LEN];
    double entropy;
    char author[128];
    struct TreeEntry* next;
} TreeEntry;

typedef struct IgnoreEntry {
    char pattern[PATH_MAX];
    size_t pattern_len;
    struct IgnoreEntry* next;
} IgnoreEntry;


// --- Globals ---
static char g_node_root_path[PATH_MAX] = {0};
static char g_objects_dir[PATH_MAX] = {0};
static char g_current_subsection[NAME_MAX + 1] = {0}; // NEW
static char g_bblk_objects_dir[PATH_MAX] = {0}; // NEW: For .bblk files
static char g_mobj_objects_dir[PATH_MAX] = {0}; // NEW: For .mobj files
static char g_unlink_root_path[PATH_MAX] = {0}; // <-- THIS WAS MISSING
static IgnoreEntry* g_ignore_list_head = NULL;

typedef struct DeltaOp {
    char op;            // 'C' (Copy) or 'I' (Insert)
    size_t offset;      // For 'C': offset in old_content
    size_t len;         // For 'C': length to copy, For 'I': length to insert
    char* data;         // For 'I': data to insert (owned by this struct)
    struct DeltaOp* next;
} DeltaOp;

static void free_delta_op_list(DeltaOp* head) {
    while (head) {
        DeltaOp* next = head->next;
        if (head->data) {
            free(head->data);
        }
        free(head);
        head = next;
    }
}


// --- Utility Functions ---

void log_msg(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[Snapshot] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

/**
 * @brief Converts raw bytes to a hex string.
 */
static void hex_encode(const uint8_t* raw, size_t raw_len, char* hex_out) {
    for (size_t i = 0; i < raw_len; i++) {
        sprintf(hex_out + (i * 2), "%02x", raw[i]);
    }
    hex_out[raw_len * 2] = '\0';
}

/**
 * @brief In-memory dynamic list of block entries for a manifest.
 */
typedef struct ManifestBlockList {
    ManifestBlockEntry* blocks;
    size_t count;
    size_t capacity;
} ManifestBlockList;

/**
 * @brief Appends a new block entry to a manifest list, resizing if needed.
 */
static int append_manifest_block(ManifestBlockList* list, const uint8_t* hash_raw, uint64_t offset, uint64_t length) {
    if (list->count + 1 > list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 16 : list->capacity * 2;
        ManifestBlockEntry* new_blocks = realloc(list->blocks, new_cap * sizeof(ManifestBlockEntry));
        if (!new_blocks) {
            log_msg("Failed to allocate memory for manifest block list");
            return -1;
        }
        list->blocks = new_blocks;
        list->capacity = new_cap;
    }
    
    ManifestBlockEntry* entry = &list->blocks[list->count];
    memcpy(entry->block_hash, hash_raw, 32);
    entry->offset = offset;
    entry->length = length;
    
    list->count++;
    return 0;
}

/**
 * @brief Frees the in-memory ManifestData struct.
 */
static void free_manifest_data(ManifestData* manifest) {
    if (!manifest) return;
    free(manifest->file_path);
    free(manifest->blocks);
    free(manifest);
}

void log_msg_diff(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static const char* find_user_for_file(ctz_json_value* history_json, const char* relative_path) {
    static const char* default_user = "unknown";
    if (!history_json || ctz_json_get_type(history_json) != CTZ_JSON_ARRAY) {
        return default_user;
    }
    size_t event_count = ctz_json_get_array_size(history_json);
    // Iterate in reverse (newest events first)
    for (size_t i = event_count; i > 0; i--) {
        ctz_json_value* item = ctz_json_get_array_element(history_json, i - 1);
        if (ctz_json_get_type(item) != CTZ_JSON_OBJECT) continue;
        ctz_json_value* name_val = ctz_json_find_object_value(item, "name");
        if (name_val && ctz_json_get_type(name_val) == CTZ_JSON_STRING &&
            strcmp(ctz_json_get_string(name_val), relative_path) == 0) {
            ctz_json_value* user_val = ctz_json_find_object_value(item, "user");
            if (user_val && ctz_json_get_type(user_val) == CTZ_JSON_STRING) {
                return ctz_json_get_string(user_val);
            }
        }
    }
    return default_user;
}

static void get_username_from_uid_embedded(uid_t uid, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "uid_%d", (int)uid); // Default
    FILE* f = fopen("/etc/passwd", "r");
    if (!f) return;
    char line[1024];
    char* saveptr;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char* line_copy = strdup(line);
        if (line_copy == NULL) continue;
        char* name = strtok_r(line_copy, ":", &saveptr);
        if (name == NULL) { free(line_copy); continue; }
        char* pass = strtok_r(NULL, ":", &saveptr);
        if (pass == NULL) { free(line_copy); continue; }
        char* uid_str = strtok_r(NULL, ":", &saveptr);
        if (uid_str == NULL) { free(line_copy); continue; }
        if (atoi(uid_str) == (int)uid) {
            strncpy(buf, name, buf_size - 1);
            buf[buf_size - 1] = '\0';
            free(line_copy);
            fclose(f);
            return;
        }
        free(line_copy);
    }
    fclose(f);
}

static int get_buffer_hash(const char* buffer, size_t size, char* hash_buf) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t*)buffer, size);
    uint8_t hash_raw[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, hash_raw);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        sprintf(hash_buf + (i * 2), "%02x", hash_raw[i]);
    }
    hash_buf[HASH_LEN] = '\0';
    return 0;
}

static int write_string_to_file(const char* fpath, const char* str) {
    char* dir_copy = strdup(fpath);
    if (!dir_copy) return -1;
    char mkdir_cmd[PATH_MAX + 10];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", dirname(dir_copy));
    (void)system(mkdir_cmd);
    free(dir_copy);
    FILE* f = fopen(fpath, "w");
    if (!f) {
        log_msg("Failed to open file '%s' for writing: %s", fpath, strerror(errno));
        return -1;
    }
    int result = fprintf(f, "%s", str);
    fclose(f);
    return (result < 0) ? -1 : 0;
}

static int read_string_from_file(const char* fpath, char* buf, size_t buf_size) {
    FILE* f = fopen(fpath, "r");
    if (!f) return -1;
    if (fgets(buf, buf_size, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static void get_object_path(const char* hash, char* path_buf) {
    int len = snprintf(path_buf, PATH_MAX, "%s/%.2s", g_objects_dir, hash);
    if (len < 0 || (size_t)len >= PATH_MAX) {
        path_buf[0] = '\0';
        return;
    }
    snprintf(path_buf + len, PATH_MAX - len, "/%s", hash + 2);
}

// --- NEW: SBDS Path Helpers ---

/**
 * @brief Gets the path for a Binary Block (.bblk) object.
 * e.g., .log/objects/b/ab/abcdef123...
 */
static void get_bblk_object_path(const char* hash_hex, char* path_buf) {
    int len = snprintf(path_buf, PATH_MAX, "%s/%.2s", g_bblk_objects_dir, hash_hex);
    if (len < 0 || (size_t)len >= PATH_MAX) {
        path_buf[0] = '\0';
        return;
    }
    // Create the directory if it doesn't exist.
    // This is safe to call multiple times.
    char obj_dir[PATH_MAX];
    strncpy(obj_dir, path_buf, PATH_MAX);
    if (mkdir(obj_dir, 0755) != 0 && errno != EEXIST) {
        // We can't log here, but the write will fail later.
    }
    snprintf(path_buf + len, PATH_MAX - len, "/%s.bblk", hash_hex + 2);
}

/**
 * @brief Gets the path for a Manifest (.mobj) object.
 * e.g., .log/objects/m/a1/a12345678...
 */
static void get_mobj_object_path(const char* hash_hex, char* path_buf) {
    int len = snprintf(path_buf, PATH_MAX, "%s/%.2s", g_mobj_objects_dir, hash_hex);
    if (len < 0 || (size_t)len >= PATH_MAX) {
        path_buf[0] = '\0';
        return;
    }
    // Create the directory if it doesn't exist.
    char obj_dir[PATH_MAX];
    strncpy(obj_dir, path_buf, PATH_MAX);
    if (mkdir(obj_dir, 0755) != 0 && errno != EEXIST) {
        // We can't log here, but the write will fail later.
    }
    snprintf(path_buf + len, PATH_MAX - len, "/%s.mobj", hash_hex + 2);
}

// --- END SBDS Path Helpers ---

static double calculate_entropy(const char* content, size_t size) {
    if (size == 0) return 0.0;
    long counts[256] = {0};
    for (size_t i = 0; i < size; i++) {
        counts[(unsigned char)content[i]]++;
    }
    double total_entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0) {
            double probability = (double)counts[i] / (double)size;
            total_entropy += probability * log2(probability);
        }
    }
    return -total_entropy;
}

// Check for null bytes in the first 4KB to guess if it's binary
// This is ONLY used for the 'diff' command, not for storage.
static int is_binary_file_for_diff(const char* content, size_t size) {
    size_t check_len = (size > 4096) ? 4096 : size;
    if (memchr(content, '\0', check_len) != NULL) {
        return 1; // Found a null byte, treat as binary
    }
    return 0; // Likely text
}

static void free_ignore_list() {
    while (g_ignore_list_head) {
        IgnoreEntry* next = g_ignore_list_head->next;
        free(g_ignore_list_head);
        g_ignore_list_head = next;
    }
}

static void load_ignore_list(const char* node_path) {
    char retain_file_path[PATH_MAX];
    snprintf(retain_file_path, sizeof(retain_file_path), "%s/.retain", node_path);
    FILE* f = fopen(retain_file_path, "r");
    if (!f) return;
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '/') { line[len - 1] = 0; len--; }
        if (len == 0 || line[0] == '#') continue;
        IgnoreEntry* new_entry = malloc(sizeof(IgnoreEntry));
        if (!new_entry) continue;
        strncpy(new_entry->pattern, line, sizeof(new_entry->pattern) - 1);
        new_entry->pattern_len = len;
        new_entry->next = g_ignore_list_head;
        g_ignore_list_head = new_entry;
    }
    fclose(f);
}

static int is_path_ignored(const char* relative_path) {
    if (strcmp(relative_path, ".log") == 0) return 1;
    if (strcmp(relative_path, ".retain") == 0) return 1;
    for (IgnoreEntry* current = g_ignore_list_head; current; current = current->next) {
        if (strncmp(relative_path, current->pattern, current->pattern_len) == 0) {
            char next_char = relative_path[current->pattern_len];
            if (next_char == '\0' || next_char == '/') return 1;
        }
    }
    return 0;
}

static int get_file_hash_stream(const char* fpath, char* hash_buf, size_t* file_size_out) {
    FILE* f = fopen(fpath, "rb");
    if (!f) return -1;
    
    SHA256_CTX ctx;
    sha256_init(&ctx);
    
    unsigned char buffer[ZLIB_CHUNK_SIZE]; // Use ZLIB_CHUNK_SIZE
    size_t bytes_read;
    size_t total_size = 0;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        sha256_update(&ctx, buffer, bytes_read);
        total_size += bytes_read;
    }
    fclose(f);
    
    if (file_size_out) *file_size_out = total_size;
    
    uint8_t hash_raw[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, hash_raw);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        sprintf(hash_buf + (i * 2), "%02x", hash_raw[i]);
    }
    hash_buf[HASH_LEN] = '\0';
    return 0;
}

/**
 * @brief Calculates entropy of a file by streaming it from disk.
 */
static double calculate_entropy_stream(const char* fpath, size_t size) {
    if (size == 0) return 0.0;
    
    FILE* f = fopen(fpath, "rb");
    if (!f) return 0.0;
    
    long counts[256] = {0};
    unsigned char buffer[ZLIB_CHUNK_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        for (size_t i = 0; i < bytes_read; i++) {
            counts[buffer[i]]++;
        }
    }
    fclose(f);
    
    double total_entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0) {
            double probability = (double)counts[i] / (double)size;
            total_entropy += probability * log2(probability);
        }
    }
    return -total_entropy;
}

/**
 * @brief Compress and write a FULL BLOB object by streaming from a file.
 * Uses zlib's streaming deflate API to keep memory usage low.
 * Format: "BLOB\0"[data]
 */
static int write_blob_object_stream(const char* hash, const char* fpath) {
    char obj_path[PATH_MAX];
    get_object_path(hash, obj_path);
    struct stat st;
    if (stat(obj_path, &st) == 0) return 0; // Already exists

    char obj_dir[PATH_MAX];
    strncpy(obj_dir, obj_path, sizeof(obj_dir));
    *strrchr(obj_dir, '/') = '\0';
    if (mkdir(obj_dir, 0755) != 0 && errno != EEXIST) return -1;

    FILE* fin = fopen(fpath, "rb");
    if (!fin) return -1;
    
    FILE* fout = fopen(obj_path, "wb");
    if (!fout) { fclose(fin); return -1; }

    int ret, flush;
    unsigned have;
    z_stream strm;
    unsigned char in[ZLIB_CHUNK_SIZE];
    unsigned char out[ZLIB_CHUNK_SIZE];

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK) { fclose(fin); fclose(fout); return ret; }

    // 1. Compress and write header first
    const char* header = "BLOB\0";
    int header_len = 5;
    strm.avail_in = header_len;
    strm.next_in = (Bytef*)header;
    
    // Run deflate() on header
    do {
        strm.avail_out = ZLIB_CHUNK_SIZE;
        strm.next_out = out;
        ret = deflate(&strm, Z_NO_FLUSH); // Process header
        if (ret == Z_STREAM_ERROR) { (void)deflateEnd(&strm); fclose(fin); fclose(fout); return ret; }
        have = ZLIB_CHUNK_SIZE - strm.avail_out;
        if (fwrite(out, 1, have, fout) != have || ferror(fout)) {
            (void)deflateEnd(&strm); fclose(fin); fclose(fout); return Z_ERRNO;
        }
    } while (strm.avail_out == 0);
    
    // 2. Compress and write file content
    do {
        strm.avail_in = fread(in, 1, ZLIB_CHUNK_SIZE, fin);
        if (ferror(fin)) {
            (void)deflateEnd(&strm); fclose(fin); fclose(fout); return Z_ERRNO;
        }
        flush = feof(fin) ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = in;

        do {
            strm.avail_out = ZLIB_CHUNK_SIZE;
            strm.next_out = out;
            ret = deflate(&strm, flush);
            if (ret == Z_STREAM_ERROR) { (void)deflateEnd(&strm); fclose(fin); fclose(fout); return ret; }
            have = ZLIB_CHUNK_SIZE - strm.avail_out;
            if (fwrite(out, 1, have, fout) != have || ferror(fout)) {
                (void)deflateEnd(&strm); fclose(fin); fclose(fout); return Z_ERRNO;
            }
        } while (strm.avail_out == 0);

    } while (flush != Z_FINISH);

    (void)deflateEnd(&strm);
    fclose(fin);
    fclose(fout);
    return Z_OK;
}

// --- NEW: Anchor-Weave Path Helpers ---

static void get_subsection_versions_file(const char* node_path, const char* subsection_name, char* path_buf, size_t buf_size) {
    if (strcmp(subsection_name, "master") == 0) {
        snprintf(path_buf, buf_size, "%s/.log/TRUNK.versions.json", node_path);
    } else {
        snprintf(path_buf, buf_size, "%s/.log/subsections/%s.versions.json", node_path, subsection_name);
    }
}

/**
 * @brief Gets path to .log/TRUNK_HEAD (the master subsection)
 */
static void get_trunk_head_file(const char* node_path, char* path_buf, size_t buf_size) {
    snprintf(path_buf, buf_size, "%s/.log/TRUNK_HEAD", node_path);
}

/**
 * @brief Gets path to a subsection's HEAD file
 * e.g., .log/subsections/dev.subsec
 */
static void get_subsection_head_file(const char* node_path, const char* subsection_name, char* path_buf, size_t buf_size) {
    snprintf(path_buf, buf_size, "%s/.log/subsections/%s.subsec", node_path, subsection_name);
}

/**
 * @brief Gets path to the subsections directory
 */
static void get_subsections_dir(const char* node_path, char* path_buf, size_t buf_size) {
    snprintf(path_buf, buf_size, "%s/.log/subsections", node_path);
}

/**
 * @brief Gets the path to the *active* HEAD file (Trunk or Subsection)
 */
static void get_active_head_file(const char* node_path, char* path_buf, size_t buf_size) {
    if (strcmp(g_current_subsection, "master") == 0) {
        get_trunk_head_file(node_path, path_buf, buf_size);
    } else {
        get_subsection_head_file(node_path, g_current_subsection, path_buf, buf_size);
    }
}

// --- END NEW Path Helpers ---


static void free_tree_list(TreeEntry* head) {
    while (head) {
        TreeEntry* next = head->next;
        free(head);
        head = next;
    }
}

static void free_lcs_matrix(int** matrix, int rows) {
    if (!matrix) return;
    for (int i = 0; i < rows; i++) {
        free(matrix[i]);
    }
    free(matrix);
}

// --- ENGINE 1: Line-Based Diff (for 'diff' command) ---

static void free_lines(TextLine* head) {
    while (head) {
        TextLine* next = head->next;
        free(head->line);
        free(head);
        head = next;
    }
}

static void free_diff_ops(DiffOp* head) {
    while (head) {
        DiffOp* next = head->next;
        free(head->content);
        free(head);
        head = next;
    }
}

// Splits a file's content into a linked list of lines
static TextLine* split_content_to_lines(const char* content, size_t size, int* line_count_out) {
    *line_count_out = 0;
    if (size == 0) return NULL;
    TextLine* head = NULL;
    TextLine* tail = NULL;
    const char* start = content;
    const char* end = content + size;
    for (const char* p = start; p <= end; p++) {
        if (p == end || *p == '\n') {
            size_t len = p - start;
            char* line_content = malloc(len + 1);
            if (!line_content) { free_lines(head); return NULL; }
            memcpy(line_content, start, len);
            line_content[len] = '\0';
            TextLine* new_line = malloc(sizeof(TextLine));
            if (!new_line) { free(line_content); free_lines(head); return NULL; }
            new_line->line = line_content;
            new_line->len = len;
            new_line->next = NULL;
            if (tail) tail->next = new_line; else head = new_line;
            tail = new_line;
            (*line_count_out)++;
            start = p + 1;
        }
    }
    return head;
}

// Generates a diff op list from two line lists (LCS)
static DiffOp* generate_diff(TextLine* lines1, int count1, TextLine* lines2, int count2) {
    TextLine** arr1 = malloc(count1 * sizeof(TextLine*));
    TextLine** arr2 = malloc(count2 * sizeof(TextLine*));
    if (!arr1 || !arr2) { if (arr1) free(arr1); if (arr2) free(arr2); return NULL; }
    TextLine* l = lines1;
    for (int i = 0; i < count1; i++) { arr1[i] = l; l = l->next; }
    l = lines2;
    for (int i = 0; i < count2; i++) { arr2[i] = l; l = l->next; }

    int** lcs = malloc((count1 + 1) * sizeof(int*));
    if (!lcs) { free(arr1); free(arr2); return NULL; }
    for (int i = 0; i <= count1; i++) {
        lcs[i] = calloc((count2 + 1), sizeof(int));
        if (!lcs[i]) { free_lcs_matrix(lcs, i); free(arr1); free(arr2); return NULL; }
    }

    for (int i = 1; i <= count1; i++) {
        for (int j = 1; j <= count2; j++) {
            if (arr1[i - 1]->len == arr2[j - 1]->len && 
                strcmp(arr1[i - 1]->line, arr2[j - 1]->line) == 0) {
                lcs[i][j] = lcs[i - 1][j - 1] + 1;
            } else {
                lcs[i][j] = (lcs[i - 1][j] > lcs[i][j - 1]) ? lcs[i - 1][j] : lcs[i][j - 1];
            }
        }
    }

    DiffOp* head = NULL;
    int i = count1, j = count2;
    while (i > 0 || j > 0) {
        DiffOp* op = malloc(sizeof(DiffOp));
        if (!op) { free_diff_ops(head); break; }
        if (i > 0 && j > 0 && arr1[i-1]->len == arr2[j-1]->len &&
            strcmp(arr1[i-1]->line, arr2[j-1]->line) == 0) {
            op->type = DIFF_SAME;
            op->content = strdup(arr1[i - 1]->line);
            i--; j--;
        } else if (j > 0 && (i == 0 || lcs[i][j - 1] >= lcs[i - 1][j])) {
            op->type = DIFF_ADD;
            op->content = strdup(arr2[j - 1]->line);
            j--;
        } else if (i > 0 && (j == 0 || lcs[i][j - 1] < lcs[i - 1][j])) {
            op->type = DIFF_DEL;
            op->content = strdup(arr1[i - 1]->line);
            i--;
        }
        if (!op->content) { free(op); break; } // strdup failed
        op->next = head;
        head = op;
    }

    free_lcs_matrix(lcs, count1 + 1);
    free(arr1);
    free(arr2);
    return head;
}


static TextLine* patch_lines(TextLine* base_head, const char* script, size_t script_size) {
    TextLine* new_head = NULL;
    TextLine* new_tail = NULL;
    TextLine* base_ptr = base_head;
    
    char* script_copy = strndup(script, script_size);
    if (!script_copy) return NULL;
    
    char* line = script_copy;
    char* saveptr = NULL; // for strtok_r
    
    // Use strtok_r for safe line parsing
    for (line = strtok_r(script_copy, "\n", &saveptr); 
         line; 
         line = strtok_r(NULL, "\n", &saveptr)) 
    {
        if (line[0] == 'A') {
            // ADD
            char* content = line + 2;
            TextLine* new_line = malloc(sizeof(TextLine));
            if (!new_line) break; // Malloc failed
            
            new_line->line = strdup(content);
            if (!new_line->line) { free(new_line); break; } // Strdup failed
            
            new_line->len = strlen(content);
            new_line->next = NULL;
            if (new_tail) new_tail->next = new_line; else new_head = new_line;
            new_tail = new_line;
            
        } else if (line[0] == 'D') {
            // DELETE
            if (base_ptr) {
                base_ptr = base_ptr->next; // Just advance the base pointer, don't copy
            }
        } else if (line[0] == 'S') {
            // SAME
            int count = atoi(line + 2);
            for (int i = 0; i < count; i++) {
                if (!base_ptr) break; // Should not happen
                
                TextLine* new_line = malloc(sizeof(TextLine));
                if (!new_line) break; // Malloc failed
                
                new_line->line = strdup(base_ptr->line);
                if (!new_line->line) { free(new_line); break; } // Strdup failed
                
                new_line->len = base_ptr->len;
                new_line->next = NULL;
                if (new_tail) new_tail->next = new_line; else new_head = new_line;
                new_tail = new_line;
                base_ptr = base_ptr->next;
            }
        }
    }
    
    free(script_copy);
    return new_head;
}

/**
 * @brief Reconstructs a full content buffer from a list of lines
 * (Used for deprecated DELTA-LCS objects)
 */
static char* reconstruct_content_from_lines(TextLine* head, size_t* total_size_out) {
    size_t total_size = 0;
    int line_count = 0;
    for (TextLine* l = head; l; l = l->next) {
        total_size += l->len;
        line_count++;
    }
    // Add space for \n chars (all but the last line)
    total_size += (line_count > 0 ? line_count - 1 : 0); 

    char* buffer = malloc(total_size + 1); // +1 for final null
    if (!buffer) { *total_size_out = 0; return NULL; }
    
    char* ptr = buffer;
    for (TextLine* l = head; l; l = l->next) {
        memcpy(ptr, l->line, l->len);
        ptr += l->len;
        if (l->next) {
            *ptr = '\n';
            ptr++;
        }
    }
    *ptr = '\0';
    *total_size_out = total_size;
    return buffer;
}

// --- ENGINE 2: Byte-Level Delta (for Storage) ---

/**
 * @brief Appends data to the delta script, reallocating if needed.
 */
static int append_delta_data(DeltaScript* script, const void* data, size_t len) {
    if (script->size + len > script->capacity) {
        size_t new_cap = (script->capacity == 0) ? 1024 : script->capacity * 2;
        if (new_cap < script->size + len) {
            new_cap = script->size + len;
        }
        char* new_buf = realloc(script->buffer, new_cap);
        if (!new_buf) return -1;
        script->buffer = new_buf;
        script->capacity = new_cap;
    }
    memcpy(script->buffer + script->size, data, len);
    script->size += len;
    return 0;
}

/**
 * @brief Appends a COPY operation to the script.
 * Format: 'C' (1 byte), offset (size_t), len (size_t)
 */
static int append_delta_copy(DeltaScript* script, size_t offset, size_t len) {
    if (len == 0) return 0;
    char op = 'C';
    if (append_delta_data(script, &op, 1) != 0) return -1;
    if (append_delta_data(script, &offset, sizeof(size_t)) != 0) return -1;
    if (append_delta_data(script, &len, sizeof(size_t)) != 0) return -1;
    return 0;
}

/**
 * @brief Appends an INSERT operation to the script.
 * Format: 'I' (1 byte), len (size_t), data (len bytes)
 */
static int append_delta_insert(DeltaScript* script, const char* data, size_t len) {
    if (len == 0) return 0;
    char op = 'I';
    if (append_delta_data(script, &op, 1) != 0) return -1;
    if (append_delta_data(script, &len, sizeof(size_t)) != 0) return -1;
    if (append_delta_data(script, data, len) != 0) return -1;
    return 0;
}

static uint32_t adler32_checksum(const char* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + (uint8_t)data[i]) % ADLER_MOD;
        b = (b + a) % ADLER_MOD;
    }
    return (b << 16) | a;
}

/**
 * @brief Efficiently updates a rolling Adler-32 checksum.
 *
 * @param sum      The previous checksum.
 * @param out_byte The byte leaving the window.
 * @param in_byte  The byte entering the window.
 * @param len      The window length (BLOCK_SIZE).
 * @return The new checksum for the updated window.
 */
static uint32_t adler32_roll(uint32_t sum, uint8_t out_byte, uint8_t in_byte, size_t len) {
    uint32_t a = sum & 0xFFFF;
    uint32_t b = sum >> 16;
    
    // Remove the 'out' byte
    a = (a - out_byte + ADLER_MOD) % ADLER_MOD;
    b = (b - (len * out_byte) + ADLER_MOD * (len + 1)) % ADLER_MOD;
    
    // Add the 'in' byte
    a = (a + in_byte) % ADLER_MOD;
    b = (b + a) % ADLER_MOD;

    return (b << 16) | a;
}

/**
 * @brief Helper to get the raw 32-byte SHA-256 for a buffer.
 * (We use this for strong comparison, not the hex string).
 */
static void sha256_buffer(const uint8_t* data, size_t len, uint8_t hash_out[SHA256_BLOCK_SIZE]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash_out);
}

// --- Simple Hash Map for Signatures ---

typedef struct BlockSignature {
    uint8_t strong_hash[SHA256_BLOCK_SIZE];
    size_t offset;
    struct BlockSignature* next; // For hash collisions
} BlockSignature;

typedef struct SignatureMap {
    BlockSignature* buckets[HASH_MAP_BUCKETS];
} SignatureMap;

static SignatureMap* map_create() {
    SignatureMap* map = calloc(1, sizeof(SignatureMap));
    return map;
}

static void map_insert(SignatureMap* map, uint32_t weak_hash, const uint8_t* strong_hash, size_t offset) {
    uint32_t bucket_index = weak_hash % HASH_MAP_BUCKETS;

    // Create new signature entry
    BlockSignature* sig = malloc(sizeof(BlockSignature));
    if (!sig) return;
    memcpy(sig->strong_hash, strong_hash, SHA256_BLOCK_SIZE);
    sig->offset = offset;
    
    // Add to the front of the linked list for this bucket
    sig->next = map->buckets[bucket_index];
    map->buckets[bucket_index] = sig;
}

static long map_find(SignatureMap* map, uint32_t weak_hash, const uint8_t* strong_hash) {
    uint32_t bucket_index = weak_hash % HASH_MAP_BUCKETS;
    
    // Iterate through the linked list for this bucket
    for (BlockSignature* sig = map->buckets[bucket_index]; sig; sig = sig->next) {
        // Strong hash check (memcmp is 0 on match)
        if (memcmp(sig->strong_hash, strong_hash, SHA256_BLOCK_SIZE) == 0) {
            return (long)sig->offset; // Found!
        }
    }
    return -1; // Not found
}

static void map_free(SignatureMap* map) {
    if (!map) return;
    for (int i = 0; i < HASH_MAP_BUCKETS; i++) {
        BlockSignature* sig = map->buckets[i];
        while (sig) {
            BlockSignature* next = sig->next;
            free(sig);
            sig = next;
        }
    }
    free(map);
}

static int generate_byte_delta_script(
    const char* old_content, size_t old_size,
    const char* new_content, size_t new_size,
    DeltaScript* script_out) 
{
    // --- 0. Handle edge cases ---
    if (old_size == 0 || new_size == 0) {
        return -1; // Cannot delta, fallback to BLOB
    }

    // --- 1. SIGNATURE phase: Build hash map from old_content ---
    log_msg("  > Building signature map for base file (%.1fMB)", (double)old_size / (1024.0*1024.0));
    SignatureMap* map = map_create();
    if (!map) return -1;
    
    for (size_t offset = 0; (offset + DELTA_BLOCK_SIZE) <= old_size; offset += DELTA_BLOCK_SIZE) {
        const char* block = old_content + offset;
        
        // Calculate weak (Adler-32) and strong (SHA-256) hashes
        uint32_t weak_hash = adler32_checksum(block, DELTA_BLOCK_SIZE);
        uint8_t strong_hash[SHA256_BLOCK_SIZE];
        sha256_buffer((const uint8_t*)block, DELTA_BLOCK_SIZE, strong_hash);

        // Store in map
        map_insert(map, weak_hash, strong_hash, offset);
    }

    // --- 2. DELTA phase: Scan new_content with rolling hash ---
    log_msg("  > Scanning new file (%.1fMB) for deltas...", (double)new_size / (1024.0*1024.0));
    
    free(script_out->buffer); // Clear any old data
    *script_out = (DeltaScript){0}; // Re-init

    size_t i = 0; // Current position in new_content
    size_t last_match_end = 0; // End of the last match
    uint32_t rolling_hash = 0;

    while (i + DELTA_BLOCK_SIZE <= new_size) {
        
        const char* window = new_content + i;
        
        // --- Calculate weak hash (either full or rolled) ---
        if (i == 0) {
            rolling_hash = adler32_checksum(window, DELTA_BLOCK_SIZE);
        } else {
            rolling_hash = adler32_roll(
                rolling_hash, 
                (uint8_t)new_content[i - 1], 
                (uint8_t)new_content[i + DELTA_BLOCK_SIZE - 1],
                DELTA_BLOCK_SIZE
            );
        }

        // --- Check for a match in the map ---
        long match_offset = -1;
        if (map) {
            // Calculate strong hash *only* if we have a weak hit
            uint8_t strong_hash[SHA256_BLOCK_SIZE];
            sha256_buffer((const uint8_t*)window, DELTA_BLOCK_SIZE, strong_hash);
            
            // This does the weak-hash-bucket-lookup + strong-hash-compare
            match_offset = map_find(map, rolling_hash, strong_hash);
        }

        if (match_offset != -1) {
            // --- MATCH FOUND ---
            
            // 1. Emit an INSERT for any pending data before this match
            size_t insert_len = i - last_match_end;
            if (insert_len > 0) {
                if (append_delta_insert(script_out, new_content + last_match_end, insert_len) != 0) {
                    goto error_cleanup;
                }
            }

            // 2. Emit a COPY op for this block
            if (append_delta_copy(script_out, (size_t)match_offset, DELTA_BLOCK_SIZE) != 0) {
                goto error_cleanup;
            }

            // 3. Jump our pointers forward
            i += DELTA_BLOCK_SIZE;
            last_match_end = i;

        } else {
            // --- NO MATCH ---
            // Just advance the window by one byte
            i++;
        }
    }

    // --- 3. CLEANUP phase: Insert any remaining data ---
    size_t remaining_len = new_size - last_match_end;
    if (remaining_len > 0) {
        if (append_delta_insert(script_out, new_content + last_match_end, remaining_len) != 0) {
            goto error_cleanup;
        }
    }

    log_msg("  > Delta script generated (size: %.1fKB)", (double)script_out->size / 1024.0);
    map_free(map);
    return 0; // Success!

error_cleanup:
    log_msg("  > Error generating delta script. Aborting.");
    map_free(map);
    free(script_out->buffer);
    *script_out = (DeltaScript){0};
    return -1; // Indicate failure
}


static char* patch_from_byte_delta(
    const char* old_content, size_t old_size,
    const char* script_buf, size_t script_size,
    size_t* new_size_out) 
{
    // We use a DeltaScript struct as a dynamic buffer for the *output*
    DeltaScript out = {0}; 
    const char* ptr = script_buf;
    const char* end = script_buf + script_size;

    while (ptr < end) {
        char op = *ptr++;
        if (op == 'C') {
            // --- Handle COPY ---
            if (ptr + sizeof(size_t) * 2 > end) { free(out.buffer); return NULL; } // Malformed
            size_t offset = *(size_t*)ptr;
            ptr += sizeof(size_t);
            size_t len = *(size_t*)ptr;
            ptr += sizeof(size_t);
            
            if (offset + len > old_size) { free(out.buffer); return NULL; } // Out of bounds
            
            if (append_delta_data(&out, old_content + offset, len) != 0) { free(out.buffer); return NULL; }

        } else if (op == 'I') {
            // --- Handle INSERT ---
            if (ptr + sizeof(size_t) > end) { free(out.buffer); return NULL; } // Malformed
            size_t len = *(size_t*)ptr;
            ptr += sizeof(size_t);
            
            if (ptr + len > end) { free(out.buffer); return NULL; } // Malformed
            
            if (append_delta_data(&out, ptr, len) != 0) { free(out.buffer); return NULL; }
            ptr += len;
        } else {
            free(out.buffer);
            return NULL; // Unknown op
        }
    }
    
    *new_size_out = out.size;
    
    // Add null terminator for safety with string functions, though it's binary
    if (append_delta_data(&out, "\0", 1) != 0) {
        *new_size_out = out.size; // Size is correct, but buffer might be returned
        return out.buffer;
    }
    
    return out.buffer;
}

// --- Object Read/Write Logic (MODIFIED) ---

/**
 * @brief Compress and write a FULL BLOB object
 * Format: "BLOB\0"[data]
 */
static int write_blob_object(const char* hash, const char* content, size_t content_size) {
    char obj_path[PATH_MAX];
    get_object_path(hash, obj_path);
    struct stat st;
    if (stat(obj_path, &st) == 0) return 0; 
    
    char obj_dir[PATH_MAX];
    strncpy(obj_dir, obj_path, sizeof(obj_dir));
    *strrchr(obj_dir, '/') = '\0';
    if (mkdir(obj_dir, 0755) != 0 && errno != EEXIST) return -1;
    
    size_t header_len = 5; // "BLOB\0"
    size_t total_uncomp_size = header_len + content_size;
    Bytef* uncompressed_buf = malloc(total_uncomp_size);
    if (!uncompressed_buf) return -1;
    
    memcpy(uncompressed_buf, "BLOB\0", header_len);
    memcpy(uncompressed_buf + header_len, (const Bytef*)content, content_size);

    uLongf compressed_size = compressBound(total_uncomp_size);
    Bytef* compressed_buf = malloc(compressed_size);
    if (!compressed_buf) { free(uncompressed_buf); return -1; }

    int z_result = compress(compressed_buf, &compressed_size, uncompressed_buf, total_uncomp_size);
    free(uncompressed_buf);
    if (z_result != Z_OK) { free(compressed_buf); return -1; }

    FILE* f = fopen(obj_path, "wb");
    if (!f) { free(compressed_buf); return -1; }
    fwrite(compressed_buf, 1, compressed_size, f);
    fclose(f);
    free(compressed_buf);
    return 0;
}

/**
 * @brief Compress and write a BYTE-LEVEL DELTA object
 * Format: "DELTA-BYTE\0"[base_hash]\0[delta_script]
 */
static int write_byte_delta_object(const char* hash, const char* base_hash, const char* delta_script, size_t delta_size) {
    char obj_path[PATH_MAX];
    get_object_path(hash, obj_path);
    struct stat st;
    if (stat(obj_path, &st) == 0) return 0;

    char obj_dir[PATH_MAX];
    strncpy(obj_dir, obj_path, sizeof(obj_dir));
    *strrchr(obj_dir, '/') = '\0';
    if (mkdir(obj_dir, 0755) != 0 && errno != EEXIST) return -1;

    size_t header_len = 11 + HASH_LEN + 1; // "DELTA-BYTE\0" + "hash" + "\0"
    size_t total_uncomp_size = header_len + delta_size;
    Bytef* uncompressed_buf = malloc(total_uncomp_size);
    if (!uncompressed_buf) return -1;

    memcpy(uncompressed_buf, "DELTA-BYTE\0", 11);
    memcpy(uncompressed_buf + 11, base_hash, HASH_LEN + 1); // +1 for null
    memcpy(uncompressed_buf + header_len, (const Bytef*)delta_script, delta_size);

    uLongf compressed_size = compressBound(total_uncomp_size);
    Bytef* compressed_buf = malloc(compressed_size);
    if (!compressed_buf) { free(uncompressed_buf); return -1; }

    int z_result = compress(compressed_buf, &compressed_size, uncompressed_buf, total_uncomp_size);
    free(uncompressed_buf);
    if (z_result != Z_OK) { free(compressed_buf); return -1; }

    FILE* f = fopen(obj_path, "wb");
    if (!f) { free(compressed_buf); return -1; }
    fwrite(compressed_buf, 1, compressed_size, f);
    fclose(f);
    free(compressed_buf);
    return 0;
}

static char* read_object(const char* hash, size_t* uncompressed_size) {
    char obj_path[PATH_MAX];
    get_object_path(hash, obj_path);

    FILE* f = fopen(obj_path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long compressed_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (compressed_size <= 0) { fclose(f); return NULL; }

    Bytef* compressed_buf = malloc(compressed_size);
    if (!compressed_buf) { fclose(f); return NULL; }

    if (fread(compressed_buf, 1, compressed_size, f) != (size_t)compressed_size) {
        fclose(f); free(compressed_buf); return NULL;
    }
    fclose(f);

    uLongf uncomp_len_guess = compressed_size * 20; 
    if (uncomp_len_guess < 4096) uncomp_len_guess = 4096;
    
    Bytef* uncompressed_buf = NULL;
    int z_result;

    // Loop to handle zlib buffer too small error
    while (1) {
        free(uncompressed_buf); // Free previous attempt
        uncompressed_buf = malloc(uncomp_len_guess + 1); // +1 for null term
        if (!uncompressed_buf) { free(compressed_buf); return NULL; }
        
        uLongf current_guess_size = uncomp_len_guess;
        z_result = uncompress(uncompressed_buf, &current_guess_size, compressed_buf, compressed_size);
        
        if (z_result == Z_OK) {
            uncomp_len_guess = current_guess_size; // Store the actual size
            break;
        } else if (z_result == Z_BUF_ERROR) {
            uncomp_len_guess *= 2; // Double the buffer size and retry
        } else {
            log_msg("Zlib uncompress error %d for object %s", z_result, hash);
            free(compressed_buf);
            free(uncompressed_buf);
            return NULL;
        }
    }
    
    free(compressed_buf); 
    
    // Null-terminate the uncompressed buffer for safe string ops
    uncompressed_buf[uncomp_len_guess] = '\0';

    // --- Check the object format header ---
    if (uncomp_len_guess >= 5 && memcmp(uncompressed_buf, "BLOB\0", 5) == 0) {
        // --- This is a FULL OBJECT (BLOB) ---
        size_t header_len = 5;
        *uncompressed_size = uncomp_len_guess - header_len;

        char* final_content = malloc(*uncompressed_size + 1);
        if (final_content) {
            memcpy(final_content, uncompressed_buf + header_len, *uncompressed_size);
            final_content[*uncompressed_size] = '\0';
        }
        free(uncompressed_buf);
        return final_content;

    } else if (uncomp_len_guess > 77 && memcmp(uncompressed_buf, "DELTA-BYTE\0", 11) == 0) {
        // --- This is a DELTA-BYTE OBJECT ---
        char base_hash[HASH_STR_LEN];
        size_t header_len = 11 + HASH_LEN + 1; // "DELTA-BYTE\0" + "hash" + "\0"
        
        memcpy(base_hash, uncompressed_buf + 11, HASH_LEN);
        base_hash[HASH_LEN] = '\0';
        
        char* delta_script = (char*)uncompressed_buf + header_len;
        size_t delta_size = uncomp_len_guess - header_len;

        // --- RECURSIVE CALL ---
        size_t base_size;
        char* base_content = read_object(base_hash, &base_size);
        if (!base_content) {
            log_msg("Failed to read base object %s to reconstruct delta %s", base_hash, hash);
            free(uncompressed_buf); return NULL;
        }

        // --- Reconstruct by patching ---
        char* final_content = patch_from_byte_delta(base_content, base_size, delta_script, delta_size, uncompressed_size);
        
        // Cleanup
        free(base_content);
        free(uncompressed_buf);

        if (!final_content) {
            log_msg("Failed to patch delta object %s", hash);
            return NULL;
        }
        
        return final_content;
        
    } else {
        // --- THIS IS THE IMPLEMENTED FALLBACK ---
        if (uncomp_len_guess > 72 && memcmp(uncompressed_buf, "DELTA-LCS\0", 10) == 0) {
            log_msg("Warning: Reading deprecated DELTA-LCS object %s. Please re-commit to upgrade.", hash);
            
            // --- BEGIN: DEPRECATED DELTA-LCS PATCH LOGIC ---
            char base_hash[HASH_STR_LEN];
            size_t header_len = 10 + HASH_LEN + 1; // "DELTA-LCS\0" + "hash" + "\0"
            
            memcpy(base_hash, uncompressed_buf + 10, HASH_LEN);
            base_hash[HASH_LEN] = '\0';
            
            char* delta_script = (char*)uncompressed_buf + header_len;
            size_t delta_size = uncomp_len_guess - header_len;

            // --- RECURSIVE CALL ---
            size_t base_size;
            char* base_content = read_object(base_hash, &base_size);
            if (!base_content) {
                log_msg("Failed to read base object %s to reconstruct deprecated delta %s", base_hash, hash);
                free(uncompressed_buf); 
                return NULL;
            }

            // --- Reconstruct by patching (using line-based logic) ---
            int base_line_count;
            TextLine* base_lines = split_content_to_lines(base_content, base_size, &base_line_count);
            TextLine* new_lines = patch_lines(base_lines, delta_script, delta_size);
            char* final_content = reconstruct_content_from_lines(new_lines, uncompressed_size);
            
            // Cleanup
            free(base_content);
            free_lines(base_lines);
            free_lines(new_lines);
            free(uncompressed_buf);

            return final_content;
            // --- END: DEPRECATED DELTA-LCS PATCH LOGIC ---
        }

        log_msg("Unknown or corrupt object format in %s", hash);
        free(uncompressed_buf);
        return NULL;
    }
}


/**
 * @brief Writes a Binary Block (.bblk) object to the store.
 *
 * @param hash_hex          The 65-byte hex string hash of the block_data.
 * @param block_data        The raw data to store.
 * @param data_len          The length of block_data.
 * @param bblk_header       A fully-populated BinaryBlockHeader struct.
 * @return 0 on success, -1 on failure.
 */
static int write_bblk_object(const char* hash_hex, const char* block_data, size_t data_len, 
                             BinaryBlockHeader* bblk_header) 
{
    char obj_path[PATH_MAX];
    get_bblk_object_path(hash_hex, obj_path);
    if (obj_path[0] == '\0') return -1;

    // Check if file already exists
    struct stat st;
    if (stat(obj_path, &st) == 0) {
        return 0; // Already exists
    }

    // --- Padding Calculation ---
    // The total size of headers (EBOF + BBLK) is 16 + 56 = 72 bytes, which is 8-byte aligned.
    // We only need to pad the data_len.
    size_t data_size_with_padding = data_len;
    size_t padding_needed = (8 - (data_len % 8)) % 8;
    data_size_with_padding += padding_needed;
    // --- End Padding Calculation ---

    FILE* f = fopen(obj_path, "wb");
    if (!f) {
        log_msg("Failed to open .bblk for writing: %s", obj_path);
        return -1;
    }

    // 1. Write EBOF v4 Header
    // The payload_size includes the BinaryBlockHeader, the data, and the padding.
    EBOFv4Header header = {
        .magic = EBOF_MAGIC,
        .version = EBOF_VERSION,
        .type = EBOF_TYPE_BBLK,
        .payload_size = sizeof(BinaryBlockHeader) + data_size_with_padding
    };
    if (fwrite(&header, sizeof(EBOFv4Header), 1, f) != 1) {
        log_msg("Failed to write EBOFv4Header to %s", obj_path);
        fclose(f); return -1;
    }

    // 2. Write Binary Block Header
    if (fwrite(bblk_header, sizeof(BinaryBlockHeader), 1, f) != 1) {
        log_msg("Failed to write BinaryBlockHeader to %s", obj_path);
        fclose(f); return -1;
    }

    // 3. Write Data
    if (data_len > 0 && fwrite(block_data, data_len, 1, f) != 1) {
        log_msg("Failed to write block data to %s", obj_path);
        fclose(f); return -1;
    }

    // 4. Write 8-byte alignment padding
    if (padding_needed > 0) {
        static const char zero_padding[8] = {0}; // A buffer of null bytes
        if (fwrite(zero_padding, 1, padding_needed, f) != padding_needed) {
            log_msg("Failed to write alignment padding to %s", obj_path);
            fclose(f); return -1;
        }
    }
    
    fclose(f);
    return 0;
}

static char* read_bblk_object(const char* hash_hex, size_t* data_len_out, 
                              BinaryBlockHeader* bblk_header_out) 
{
    char obj_path[PATH_MAX];
    get_bblk_object_path(hash_hex, obj_path);
    if (obj_path[0] == '\0') return NULL;

    FILE* f = fopen(obj_path, "rb");
    if (!f) return NULL; // Don't log, this is a fast path

    // 1. Read and verify EBOF v4 Header
    EBOFv4Header header;
    if (fread(&header, sizeof(EBOFv4Header), 1, f) != 1) {
        fclose(f); return NULL;
    }
    if (header.magic != EBOF_MAGIC || header.type != EBOF_TYPE_BBLK) {
        log_msg("Corrupt .bblk object or bad magic: %s", obj_path);
        fclose(f); return NULL;
    }

    // 2. Read Binary Block Header
    BinaryBlockHeader bblk_header;
    if (fread(&bblk_header, sizeof(BinaryBlockHeader), 1, f) != 1) {
        log_msg("Failed to read BinaryBlockHeader from %s", obj_path);
        fclose(f); return NULL;
    }
    if (bblk_header_out) {
        memcpy(bblk_header_out, &bblk_header, sizeof(BinaryBlockHeader));
    }

    // 3. Calculate data size and read data
    size_t data_len = bblk_header.original_length; // Use the *original* length
    size_t total_payload_size = header.payload_size - sizeof(BinaryBlockHeader);

    if (data_len > total_payload_size) {
        log_msg("Block %s is corrupt. Original length exceeds payload.", hash_hex);
        fclose(f); return NULL;
    }
    if (data_len > 1024 * 1024 * 1024) { // 1GB safety limit per block
        log_msg("Block %s is too large (%.1f GB)", hash_hex, (double)data_len / (1024*1024*1024));
        fclose(f); return NULL;
    }
    
    *data_len_out = data_len;
    if (data_len == 0) {
        fclose(f);
        return strdup(""); // Return empty, allocated string
    }

    char* data_buf = malloc(data_len);
    if (!data_buf) {
        fclose(f); return NULL;
    }

    // We only read data_len, skipping any 8-byte alignment padding
    if (fread(data_buf, data_len, 1, f) != 1) {
        log_msg("Failed to read block data from %s", obj_path);
        free(data_buf);
        fclose(f); return NULL;
    }
    
    fclose(f); // Close file *before* CPU-intensive check

    // --- NEW: Verify CRC32 Checksum ---
    uint32_t computed_crc = crc32(0L, (const Bytef*)data_buf, data_len);
    if (computed_crc != bblk_header.crc32_checksum) {
        log_msg("Error: BLOCK CORRUPTION detected in %s", hash_hex);
        log_msg("  > Expected CRC32: %u, Got: %u", bblk_header.crc32_checksum, computed_crc);
        free(data_buf);
        return NULL;
    }
    // --- END NEW ---

    return data_buf;
}

/**
 * @brief Writes a Manifest (.mobj) object to the store.
 *
 * @param hash_hex      The 65-byte hex string hash of the *manifest content*.
 * @param manifest      The in-memory ManifestData struct to write.
 * @return 0 on success, -1 on failure.
 */
static int write_mobj_object(const ManifestData* manifest, char* hash_hex_out) {
    // --- 1. Serialize the payload to a buffer ---
    // The payload is the ManifestObjectHeader, then the path, then the blocks.
    DeltaScript payload_buf = {0}; // Re-using DeltaScript as a dynamic buffer
    
    size_t file_path_len = strlen(manifest->file_path);
    size_t blocks_size = sizeof(ManifestBlockEntry) * manifest->block_count;

    // --- NEW: Generate File Signature ---
    // This signature is a SHA-256 over the *concatenated block hashes*.
    // It proves the *list* of blocks is correct.
    uint8_t file_signature_raw[SHA256_BLOCK_SIZE];
    if (manifest->block_count > 0) {
        // We hash the raw block list memory
        sha256_buffer((const uint8_t*)manifest->blocks, blocks_size, file_signature_raw);
    } else {
        // Handle zero-block file
        memset(file_signature_raw, 0, SHA256_BLOCK_SIZE);
    }
    // --- END NEW ---

    ManifestObjectHeader mobj_header = {
        .file_path_len = (uint16_t)file_path_len,
        .file_mode = manifest->file_mode,
        .total_size = manifest->total_size,
        .block_count = manifest->block_count,
        .entropy_mean = manifest->entropy_mean
    };
    // Copy the 32-byte SHA-256 into the first 32 bytes of the 64-byte signature field
    memset(mobj_header.file_signature, 0, 64); 
    memcpy(mobj_header.file_signature, file_signature_raw, SHA256_BLOCK_SIZE);
    
    // Append header, path, and blocks to the buffer
    if (append_delta_data(&payload_buf, &mobj_header, sizeof(ManifestObjectHeader)) != 0) goto error;
    if (file_path_len > 0 && append_delta_data(&payload_buf, manifest->file_path, file_path_len) != 0) goto error;
    if (blocks_size > 0 && append_delta_data(&payload_buf, manifest->blocks, blocks_size) != 0) goto error;

    // --- 2. Hash the payload buffer ---
    uint8_t hash_raw[SHA256_BLOCK_SIZE];
    sha256_buffer((const uint8_t*)payload_buf.buffer, payload_buf.size, hash_raw);
    hex_encode(hash_raw, SHA256_BLOCK_SIZE, hash_hex_out);

    // --- 3. Write the final EBOF file ---
    char obj_path[PATH_MAX];
    get_mobj_object_path(hash_hex_out, obj_path);
    if (obj_path[0] == '\0') goto error;

    struct stat st;
    if (stat(obj_path, &st) == 0) {
        free(payload_buf.buffer);
        return 0; // Already exists
    }

    FILE* f = fopen(obj_path, "wb");
    if (!f) {
        log_msg("Failed to open .mobj for writing: %s", obj_path);
        goto error;
    }

    // Write EBOF v4 Header
    EBOFv4Header header = {
        .magic = EBOF_MAGIC,
        .version = EBOF_VERSION,
        .type = EBOF_TYPE_MOBJ,
        .payload_size = payload_buf.size
    };
    if (fwrite(&header, sizeof(EBOFv4Header), 1, f) != 1) {
        log_msg("Failed to write EBOFv4Header to %s", obj_path);
        fclose(f); goto error;
    }

    // Write the payload
    if (payload_buf.size > 0 && fwrite(payload_buf.buffer, payload_buf.size, 1, f) != 1) {
        log_msg("Failed to write .mobj payload to %s", obj_path);
        fclose(f); goto error;
    }
    
    fclose(f);
    free(payload_buf.buffer);
    return 0;

error:
    free(payload_buf.buffer);
    return -1;
}

static int deconstruct_file(const char* fpath, size_t fsize, uint32_t file_mode, 
                            const char* relative_path, char* manifest_hash_out, 
                            float* entropy_mean_out, const char* old_manifest_hash)
{
    // --- 1. Handle Zero-Size File ---
    if (fsize == 0) {
        ManifestData manifest = {0};
        manifest.file_path = (char*)relative_path;
        manifest.file_mode = file_mode;
        manifest.total_size = 0;
        manifest.block_count = 0;
        manifest.blocks = NULL;
        manifest.entropy_mean = 0.0;
        
        return write_mobj_object(&manifest, manifest_hash_out);
    }

    ManifestData* old_manifest = NULL;
    if (old_manifest_hash) {
    old_manifest = read_mobj_object(old_manifest_hash);
    if (old_manifest) {
        log_msg("  > Comparing against %u blocks from parent manifest.", old_manifest->block_count);
        }
    }

    // --- 2. Open and mmap the file for fast scanning ---
    int fd = open(fpath, O_RDONLY);
    if (fd == -1) {
        log_msg("Failed to open file for mmap: %s", fpath);
        return -1;
    }
    
    const char* file_data = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); // mmap keeps file open, we can close the fd
    
    if (file_data == MAP_FAILED) {
        log_msg("Failed to mmap file: %s", fpath);
        return -1;
    }

    // --- 3. Scan file and find content-defined boundaries ---
    log_msg("  > Deconstructing %s (%.1fMB)...", relative_path, (double)fsize / (1024.0*1024.0));
    
    ManifestBlockList block_list = {0};
    uint32_t rolling_hash = 0;
    size_t block_start = 0;
    double total_entropy_sum = 0.0;

    // We scan byte by byte
    for (size_t scan_pos = 0; scan_pos < fsize; scan_pos++) {
        size_t current_block_len = (scan_pos - block_start) + 1;
        
        // We can't calculate a hash until we have a full window
        if (current_block_len < CDC_WINDOW_SIZE) {
            // But if it's the end of the file, we must take what we have
            if (scan_pos == fsize - 1) {
                goto found_boundary; // Force final block
            }
            continue; // Not enough data for a hash yet
        }
        
        // --- Calculate/Roll the hash ---
        if (current_block_len == CDC_WINDOW_SIZE) {
            // Calculate full hash for the first window of this block
            rolling_hash = adler32_checksum(file_data + block_start, CDC_WINDOW_SIZE);
        } else {
            // Roll the hash forward
            rolling_hash = adler32_roll(
                rolling_hash,
                (uint8_t)file_data[scan_pos - CDC_WINDOW_SIZE],
                (uint8_t)file_data[scan_pos],
                CDC_WINDOW_SIZE
            );
        }

        // --- Check for a boundary ---
        int at_min_size = (current_block_len >= CDC_MIN_BLOCK);
        int at_target   = (rolling_hash & CDC_MASK) == CDC_TARGET;
        int at_max_size = (current_block_len == CDC_MAX_BLOCK);
        int at_eof      = (scan_pos == fsize - 1);

if (at_eof || at_max_size || (at_min_size && at_target)) {
    found_boundary:; // Label for goto
        size_t block_len = current_block_len;
        const char* block_data = file_data + block_start;
        uint64_t block_offset = (uint64_t)block_start;

        // --- 4. Process the block ---
        uint8_t hash_raw[SHA256_BLOCK_SIZE];
        char hash_hex[HASH_STR_LEN];

        sha256_buffer((const uint8_t*)block_data, block_len, hash_raw);
        hex_encode(hash_raw, SHA256_BLOCK_SIZE, hash_hex);

        double entropy = calculate_entropy(block_data, block_len);
        total_entropy_sum += entropy;

        // Populate the .bblk header
        BinaryBlockHeader bblk_header = {0}; // Zeros parent_block_hash by default
        bblk_header.entropy_score = (float)entropy;
        bblk_header.original_offset = block_offset;
        bblk_header.original_length = block_len;
        bblk_header.crc32_checksum = crc32(0L, (const Bytef*)block_data, block_len);

        //Find parent block hash (This is your "level" logic) ---
        if (old_manifest) {
            int hash_found_in_old = 0;
            // Check if this *exact block* already existed (content-addressing)
            // If so, it's not "new" and doesn't need a parent link.
            for (uint32_t i = 0; i < old_manifest->block_count; i++) {
                if (memcmp(old_manifest->blocks[i].block_hash, hash_raw, 32) == 0) {
                    hash_found_in_old = 1;
                    break;
                }
            }

            // If it's a new or modified block, find what it replaces
            if (!hash_found_in_old) {
                // Find a block in the old manifest that started at the *same offset*
                for (uint32_t i = 0; i < old_manifest->block_count; i++) {
                    if (old_manifest->blocks[i].offset == block_offset) {

                        // We link this new block's header to the old block's hash.
                        memcpy(bblk_header.parent_block_hash, old_manifest->blocks[i].block_hash, 32);

                        // This log message shows the "level" link being created
                        char parent_hash_hex[HASH_STR_LEN];
                        hex_encode(old_manifest->blocks[i].block_hash, 32, parent_hash_hex);
                        log_msg("    > Block %s (New) replaces %s (Old/Level1) at offset %llu",
                                hash_hex, parent_hash_hex, (unsigned long long)block_offset);
                        break;
                    }
                }
            }
        }


        // Write the .bblk object to disk.
        // It's saved at its hash location, *with* the parent hash inside it.
        if (write_bblk_object(hash_hex, block_data, block_len, &bblk_header) != 0) {
            log_msg("Failed to write binary block: %s", hash_hex);
            munmap((void*)file_data, fsize);
            free(block_list.blocks);
            if (old_manifest) free_manifest_data(old_manifest);
            return -1;
        }

        // Add this block to our in-memory manifest list
        if (append_manifest_block(&block_list, hash_raw, block_offset, block_len) != 0) {
            munmap((void*)file_data, fsize);
            free(block_list.blocks);
            if (old_manifest) free_manifest_data(old_manifest);
            return -1;
        }

        // Start the next block
        block_start = scan_pos + 1;
        rolling_hash = 0;
    }
}
    
    // --- 5. Cleanup mmap ---
    munmap((void*)file_data, fsize);

    // --- 6. Create and write the final manifest object ---
    ManifestData manifest = {0};
    manifest.file_path = (char*)relative_path; // This is safe, life-cycle is OK
    manifest.file_mode = file_mode;
    manifest.total_size = fsize;
    manifest.block_count = block_list.count;
    manifest.blocks = block_list.blocks;
    manifest.entropy_mean = (block_list.count > 0) ? (float)(total_entropy_sum / (double)block_list.count) : 0.0f;

    int result = write_mobj_object(&manifest, manifest_hash_out);

    if (old_manifest) {
        free_manifest_data(old_manifest);
    }

    *entropy_mean_out = manifest.entropy_mean;
    
    // --- 7. Final cleanup ---
    free(block_list.blocks); // The blocks array is now owned by the manifest
    
    if (result == 0) {
        log_msg("  > Deconstruction complete. Manifest: %s (%d blocks)", manifest_hash_out, manifest.block_count);
    }
    return result;
}


static ManifestData* read_mobj_object(const char* hash_hex) {
    char obj_path[PATH_MAX];
    get_mobj_object_path(hash_hex, obj_path);
    if (obj_path[0] == '\0') return NULL;

    FILE* f = fopen(obj_path, "rb");
    if (!f) return NULL; // Don't log, fast path

    // 1. Read and verify EBOF v4 Header
    EBOFv4Header header;
    if (fread(&header, sizeof(EBOFv4Header), 1, f) != 1) {
        fclose(f); return NULL;
    }
    if (header.magic != EBOF_MAGIC || header.type != EBOF_TYPE_MOBJ) {
        log_msg("Corrupt .mobj object or bad magic: %s", obj_path);
        fclose(f); return NULL;
    }

    // 2. Read Manifest Object Header
    ManifestObjectHeader mobj_header;
    if (fread(&mobj_header, sizeof(ManifestObjectHeader), 1, f) != 1) {
        log_msg("Failed to read ManifestObjectHeader from %s", obj_path);
        fclose(f); return NULL;
    }
    
    // Allocate the in-memory struct
    ManifestData* manifest = calloc(1, sizeof(ManifestData));
    if (!manifest) { fclose(f); return NULL; }

    manifest->file_mode = mobj_header.file_mode;
    manifest->total_size = mobj_header.total_size;
    manifest->block_count = mobj_header.block_count;
    manifest->entropy_mean = mobj_header.entropy_mean;
    memcpy(manifest->file_signature, mobj_header.file_signature, 64);

    // 3. Read File Path
    if (mobj_header.file_path_len > 0) {
        manifest->file_path = malloc(mobj_header.file_path_len + 1);
        if (!manifest->file_path) {
            free(manifest); fclose(f); return NULL;
        }
        if (fread(manifest->file_path, mobj_header.file_path_len, 1, f) != 1) {
            free(manifest->file_path); free(manifest); fclose(f); return NULL;
        }
        manifest->file_path[mobj_header.file_path_len] = '\0';
    } else {
        manifest->file_path = strdup("");
    }

    // 4. Read Block Entries
    size_t blocks_size = sizeof(ManifestBlockEntry) * manifest->block_count;
    if (manifest->block_count > 0) {
        manifest->blocks = malloc(blocks_size);
        if (!manifest->blocks) {
            free(manifest->file_path); free(manifest); fclose(f); return NULL;
        }
        if (fread(manifest->blocks, blocks_size, 1, f) != 1) {
            free(manifest->blocks); free(manifest->file_path); free(manifest);
            fclose(f); return NULL;
        }
    } else {
        manifest->blocks = NULL;
    }
    
    fclose(f);
    return manifest;
}

static int reconstruct_file_from_manifest(const ManifestData* manifest, const char* dest_path) {
    // --- 1. Verify Manifest Integrity ---
    // (This checks the *list* of blocks, not the file content)
    uint8_t computed_sig[SHA256_BLOCK_SIZE];
    size_t blocks_size = sizeof(ManifestBlockEntry) * manifest->block_count;
    
    if (manifest->block_count > 0) {
        sha256_buffer((const uint8_t*)manifest->blocks, blocks_size, computed_sig);
    } else {
        memset(computed_sig, 0, SHA256_BLOCK_SIZE);
    }
    
    // We compare the first 32 bytes of the 64-byte signature field
    if (memcmp(manifest->file_signature, computed_sig, SHA256_BLOCK_SIZE) != 0) {
        log_msg("Error: MANIFEST CORRUPTION detected for %s", manifest->file_path);
        log_msg("  > Manifest signature does not match block list. File may be tampered.");
        return -1;
    }
    // --- End Integrity Check ---

    // --- 2. Reconstruct File ---
    if (manifest->block_count == 0) {
        // Handle zero-byte file
        FILE* f_empty = fopen(dest_path, "wb");
        if (!f_empty) {
            log_msg("Failed to create empty file at %s", dest_path);
            return -1;
        }
        fclose(f_empty);
        chmod(dest_path, manifest->file_mode);
        return 0;
    }

    FILE* f = fopen(dest_path, "wb");
    if (!f) {
        log_msg("Failed to open destination file for reconstruction: %s", dest_path);
        return -1;
    }

    log_msg("  > Reassembling %s from %u blocks...", manifest->file_path, manifest->block_count);

    for (uint32_t i = 0; i < manifest->block_count; i++) {
        ManifestBlockEntry* entry = &manifest->blocks[i];
        
        char hash_hex[HASH_STR_LEN];
        hex_encode(entry->block_hash, 32, hash_hex);
        
        size_t block_data_len = 0;
        // The read_bblk_object function now internally verifies the CRC32
        char* block_data = read_bblk_object(hash_hex, &block_data_len, NULL); 
        
        if (!block_data || block_data_len != entry->length) {
            log_msg("Error: Failed to read block %s or length mismatch (expected %llu, got %zu)", 
                    hash_hex, (unsigned long long)entry->length, block_data_len);
            if (block_data) free(block_data);
            fclose(f);
            remove(dest_path); // Delete partial file
            return -1;
        }

        // Seek to the correct offset and write the block
        if (fseek(f, entry->offset, SEEK_SET) != 0) {
            log_msg("Error: fseek failed for offset %llu in %s", (unsigned long long)entry->offset, dest_path);
            free(block_data);
            fclose(f);
            remove(dest_path);
            return -1;
        }

        if (fwrite(block_data, 1, block_data_len, f) != block_data_len) {
            log_msg("Error: fwrite failed for block %s in %s", hash_hex, dest_path);
            free(block_data);
            fclose(f);
            remove(dest_path);
            return -1;
        }
        
        free(block_data);
    }
    
    fclose(f);
    
    // Set final file permissions
    chmod(dest_path, manifest->file_mode);

    return 0;
}

static int hash_and_write_blob(const char* fpath, const char* parent_tree_hash, 
                               const char* relative_path, char* hash_out, 
                               double* entropy_out, char* type_out) // <-- ADDED type_out
{
    // 1. Get file size first
    struct stat st_fsize;
    if (stat(fpath, &st_fsize) != 0) {
        log_msg("Failed to stat blob: %s: %s", fpath, strerror(errno)); 
        return -1;
    }
    
    // We only process regular files
    if (!S_ISREG(st_fsize.st_mode)) {
         log_msg("Skipping non-regular file: %s", fpath); 
         return -1;
    }
    size_t fsize = st_fsize.st_size;

    // --- NEW: DECONSTRUCTION PATH (SBDS) ---
    // Files over 5GB get deconstructed into manifests and blocks.
    if (fsize > DECONSTRUCT_THRESHOLD) {
        *type_out = 'M'; // We are creating a Manifest
        float mean_entropy = 0.0f;
        
        log_msg("  > DECONSTRUCT: %s (%.1fGB). Using SBDS.", 
                relative_path, (double)fsize / (1024.0*1024.0*1024.0));

        char old_manifest_hash[HASH_STR_LEN] = {0};
        if (parent_tree_hash) {
            char old_hash[HASH_STR_LEN];
            mode_t old_mode;
            double old_entropy;
            char old_type = 0;
            // Check the parent tree for this same file path
            if (find_file_in_tree(parent_tree_hash, relative_path, old_hash, &old_mode, &old_entropy, &old_type) == 0) {
                if (old_type == 'M') {
                    // This file was a manifest in the parent. Use it to find block parents.
                    strncpy(old_manifest_hash, old_hash, HASH_STR_LEN);
                    log_msg("  > Found parent manifest %s to track block-level changes.", old_manifest_hash);
                }
            }
        }
        
    int result = deconstruct_file(fpath, fsize, st_fsize.st_mode, relative_path, 
                                      hash_out, &mean_entropy, 
                                      old_manifest_hash[0] ? old_manifest_hash : NULL);
        
        *entropy_out = (double)mean_entropy;
        return result; // Return immediately
    }
    
    // --- If not deconstructing, it's an old-style BLOB ---
    *type_out = 'B'; 

    // --- LARGE FILE PATH (STREAMING BLOB) ---
    // Files over 512MB (but under 5GB) are stored as streaming blobs.
    if (fsize > IN_MEMORY_FILE_LIMIT) {
        log_msg("  > LARGE FILE: %s (%.1fMB). Using streaming blob.", 
                relative_path, (double)fsize / (1024.0*1024.0));

        // 2. Get hash by streaming
        size_t streamed_size = 0;
        if (get_file_hash_stream(fpath, hash_out, &streamed_size) != 0) {
            log_msg("  > FAILED to stream-hash large file: %s", fpath);
            return -1;
        }
        
        // 3. Get entropy by streaming
        *entropy_out = calculate_entropy_stream(fpath, streamed_size);

        // 4. Check if this exact object already exists
        char obj_path[PATH_MAX];
        get_object_path(hash_out, obj_path);
        struct stat st_obj;
        if (stat(obj_path, &st_obj) == 0) {
            return 0; // Success, object already exists
        }

        // 5. Write blob by streaming
        // We explicitly DO NOT attempt delta compression on large files.
        log_msg("  > BLOB (Stream): %s (%.1fMB) E:%.4f", 
                relative_path, (double)streamed_size/1024.0, *entropy_out);
        
        if (write_blob_object_stream(hash_out, fpath) != 0) {
            log_msg("  > FAILED to stream-write large blob: %s", hash_out);
            return -1;
        }
        return 0; // Success for large file
    }

    // --- SMALL FILE PATH (IN-MEMORY, DELTA-ENABLED) ---
    // Files under 512MB are read into memory to attempt delta.
    else {
        // 1. Read new content from disk (original in-memory method)
        FILE* f = fopen(fpath, "rb");
        if (!f) { log_msg("Failed to open blob: %s", fpath); return -1; }
        
        // fsize is already known from stat, no need to fseek
        char *new_content = malloc(fsize + 1); // +1 for safety
        if (!new_content) { fclose(f); return -1; }
        if (fsize > 0 && fread(new_content, 1, fsize, f) != (size_t)fsize) {
            log_msg("Failed to read blob: %s", fpath);
            fclose(f); free(new_content); return -1;
        }
        new_content[fsize] = '\0'; // Null terminate
        fclose(f);

        // 2. Get hash of new content. This hash is *always* for the full content.
        get_buffer_hash(new_content, fsize, hash_out);
        *entropy_out = calculate_entropy(new_content, fsize);
        
        // 3. Check if this exact object already exists
        char obj_path[PATH_MAX];
        get_object_path(hash_out, obj_path);
        struct stat st_obj;
        if (stat(obj_path, &st_obj) == 0) {
            free(new_content);
            return 0; // Success, object already exists
        }
        
        // 5. Try to find parent blob to use as a base
        char old_hash[HASH_STR_LEN];
        mode_t old_mode;
        double old_entropy;
        char old_type = 0; // <-- Check the parent's type
        char* old_content = NULL;
        size_t old_size = 0;

        if (parent_tree_hash && 
            find_file_in_tree(parent_tree_hash, relative_path, old_hash, &old_mode, &old_entropy, &old_type) == 0) 
        {
            // --- MODIFIED: ONLY read object if it's a BLOB or LINK. 
            // Do NOT try to read a MANIFEST ('M') for delta.
            if (old_type == 'B' || old_type == 'L') {
                old_content = read_object(old_hash, &old_size);
            } else if (old_type == 'M') {
                log_msg("  > Parent file '%s' is a manifest. Storing new version as full blob.", relative_path);
            }
        }
        
        // 6. Try to compute a BYTE-LEVEL delta
        if (old_content && old_size > 0) {
            
            DeltaScript script = {0};
            
            // generate_byte_delta_script returns -1 if files are too big or on error
            if (generate_byte_delta_script(old_content, old_size, new_content, fsize, &script) == 0) {
                
                // --- Delta Decision Logic (75% rule) ---
                if (script.buffer && script.size > 0 && script.size < (fsize * 0.75)) {
                    log_msg("  > DELTA: %s (%.1fKB -> %.1fKB) E:%.4f", 
                            relative_path, (double)fsize/1024.0, (double)script.size/1024.0, *entropy_out);
                    
                    write_byte_delta_object(hash_out, old_hash, script.buffer, script.size);
                    
                    // Cleanup and return
                    free(script.buffer);
                    free(old_content);
                    free(new_content);
                    return 0; // Success
                }
                free(script.buffer);
            }
            
            free(old_content);
        }
        
        // --- Fallback: Write a full BLOB object ---
        log_msg("  > BLOB:  %s (%.1fKB) E:%.4f", relative_path, (double)fsize/1024.0, *entropy_out);
        write_blob_object(hash_out, new_content, fsize);
        free(new_content);
        return 0;
    }
}



// Recursively builds tree objects bottom-up
static char* build_tree_recursive(const char* current_path, const char* parent_tree_hash, 
                                  ctz_json_value* history_json, char* tree_hash_out) 
{
    DIR* dir = opendir(current_path);
    if (!dir) {
        log_msg("Failed to open dir for tree build: %s", current_path);
        return NULL;
    }

    TreeEntry* head = NULL;
    TreeEntry* tail = NULL;
    struct dirent* entry;

    const char* relative_path_dir = current_path + strlen(g_node_root_path);
    if (*relative_path_dir == '/') relative_path_dir++;
    if (strlen(relative_path_dir) > 0) {
        log_msg("Processing dir: %s", relative_path_dir);
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);
        const char* relative_path_entry = full_path + strlen(g_node_root_path);
        if (*relative_path_entry == '/') relative_path_entry++;

        if (is_path_ignored(relative_path_entry)) {
            continue;
        }
        
        struct stat st;
        if (lstat(full_path, &st) == -1) continue;

        TreeEntry* new_entry = calloc(1, sizeof(TreeEntry));
        if (!new_entry) goto error_cleanup;
        snprintf(new_entry->name, sizeof(new_entry->name), "%s", entry->d_name);

        if (S_ISDIR(st.st_mode)) {
            new_entry->type = 'T';
            new_entry->mode = st.st_mode;
            new_entry->entropy = 0.0;
            strncpy(new_entry->author, "n/a", sizeof(new_entry->author) - 1);
            
            char parent_subdir_hash[HASH_STR_LEN] = {0};
            mode_t parent_subdir_mode;
            double parent_subdir_entropy;
            
            // --- MODIFIED: Pass NULL for type_out ---
            if (parent_tree_hash) {
                find_file_in_tree(parent_tree_hash, relative_path_entry, parent_subdir_hash, &parent_subdir_mode, &parent_subdir_entropy, NULL);
            }
            if (build_tree_recursive(full_path, parent_subdir_hash[0] ? parent_subdir_hash : NULL, history_json, new_entry->hash) == NULL) {
                free(new_entry);
                goto error_cleanup; 
            }
        } else if (S_ISREG(st.st_mode)) {
            // --- MODIFIED: Handle 'B' vs 'M' type from hash_and_write_blob ---
            new_entry->mode = st.st_mode;
            const char* user = find_user_for_file(history_json, relative_path_entry);
            strncpy(new_entry->author, user, sizeof(new_entry->author) - 1);
            
            char object_type = 'B'; // Default to blob
            
            if (hash_and_write_blob(full_path, parent_tree_hash, relative_path_entry, 
                                    new_entry->hash, &new_entry->entropy, &object_type) != 0) // <-- MODIFIED CALL
            {
                log_msg("Failed to hash/write blob: %s", full_path);
                free(new_entry);
                continue;
            }
            
            new_entry->type = object_type; // Set type to 'B' (Blob) or 'M' (Manifest)
            // --- END MODIFICATION ---
            
        } else if (S_ISLNK(st.st_mode)) {
            new_entry->type = 'L';
            new_entry->mode = st.st_mode;
            const char* user = find_user_for_file(history_json, relative_path_entry);
            strncpy(new_entry->author, user, sizeof(new_entry->author) - 1);
            char target[PATH_MAX];
            ssize_t len = readlink(full_path, target, sizeof(target)-1);
            if (len == -1) { free(new_entry); continue; }
            target[len] = '\0';
            get_buffer_hash(target, len, new_entry->hash);
            new_entry->entropy = calculate_entropy(target, len);
            write_blob_object(new_entry->hash, target, len);
        } else {
            free(new_entry);
            continue;
        }
        if (tail) tail->next = new_entry; else head = new_entry;
        tail = new_entry;
    }
    closedir(dir);

    // ... (rest of the function is unchanged) ...
    size_t tree_content_size = 0;
    TreeEntry* current = head;
    while (current) {
        tree_content_size += snprintf(NULL, 0, "%o %c %s E:%.4f U:%s\t%s\n",
                                      current->mode & 07777,
                                      current->type, current->hash, 
                                      current->entropy,
                                      current->author[0] ? current->author : "unknown",
                                      current->name);
        current = current->next;
    }
    if (tree_content_size == 0) tree_content_size = 1; 
    
    char* tree_content = malloc(tree_content_size + 1);
    if (!tree_content) goto error_cleanup;
    char* ptr = tree_content;
    current = head;
    while (current) {
        ptr += sprintf(ptr, "%o %c %s E:%.4f U:%s\t%s\n",
                       current->mode & 07777, current->type, current->hash, 
                       current->entropy,
                       current->author[0] ? current->author : "unknown",
                       current->name);
        current = current->next;
    }
    *ptr = '\0'; 
    if (tree_content_size == 1 && *tree_content == '\0') {
        tree_content_size = 0; // Handle empty directory case
    }

    get_buffer_hash(tree_content, tree_content_size, tree_hash_out);
    write_blob_object(tree_hash_out, tree_content, tree_content_size); 

    free(tree_content);
    free_tree_list(head);
    return tree_hash_out; // Success

error_cleanup:
    if (dir) closedir(dir); 
    free_tree_list(head);
    return NULL; // Indicate error
}


/**
 * @brief Finds a commit hash by walking the history of the active subsection
 * and matching the commit message (tag).
 * @param node_path Path to the node.
 * @param version_tag The commit message to search for, or "LATEST_HEAD".
 * @param commit_hash_out Output buffer for the found commit hash.
 * @return 0 on success, -1 on failure.
 */
static int find_commit_hash_by_tag(const char* node_path, const char* version_tag, char* commit_hash_out) {
    char active_head_file[PATH_MAX];
    get_active_head_file(node_path, active_head_file, sizeof(active_head_file));

    char current_commit_hash[HASH_STR_LEN];
    if (read_string_from_file(active_head_file, current_commit_hash, sizeof(current_commit_hash)) != 0) {
        log_msg("Error: No history found for subsection '%s'.", g_current_subsection);
        return -1;
    }

    // Handle "LATEST_HEAD" special tag
    if (strcmp(version_tag, "LATEST_HEAD") == 0) {
        strncpy(commit_hash_out, current_commit_hash, HASH_STR_LEN);
        return 0;
    }

    // Walk the history
    int depth = 0;
    while (current_commit_hash[0] != '\0' && depth < 2000) { // 2000-commit safety limit
        size_t commit_size;
        char* commit_content = read_object(current_commit_hash, &commit_size);
        if (!commit_content) {
            log_msg("Error: Failed to read commit object %s while searching for tag.", current_commit_hash);
            return -1;
        }

        // Find the commit message (it's after the two \n\n)
        char* msg_start = strstr(commit_content, "\n\n");
        if (msg_start) {
            msg_start += 2; // Skip the \n\n
            char* msg_end = strchr(msg_start, '\n');
            if (msg_end) *msg_end = '\0'; // Temporarily terminate message

            if (strcmp(msg_start, version_tag) == 0) {
                // FOUND!
                strncpy(commit_hash_out, current_commit_hash, HASH_STR_LEN);
                free(commit_content);
                return 0;
            }
        }

        // Not found, find the parent and continue
        char* parent_line = strstr(commit_content, "parent ");
        if (parent_line) {
            sscanf(parent_line, "parent %64s", current_commit_hash);
        } else {
            current_commit_hash[0] = '\0'; // End of history
        }

        free(commit_content);
        depth++;
    }

    log_msg("Error: Could not find tag '%s' in history of subsection '%s'.", version_tag, g_current_subsection);
    return -1;
}


/**
 * @brief Helper to get parent/anchor data for a new commit.
 * @param node_path Path to the node.
 * @param parent_commit_hash_out Output for the parent commit (for delta).
 * @param parent_tree_hash_out Output for the parent tree (for delta).
 * @param anchor_hash_out Output for the anchor commit (for S-Commits).
 * @return 0 on success.
 */
static int get_parent_commit_data(const char* node_path, char* parent_commit_hash_out, 
                                  char* parent_tree_hash_out, char* anchor_hash_out) 
{
    parent_commit_hash_out[0] = '\0';
    parent_tree_hash_out[0] = '\0';
    anchor_hash_out[0] = '\0';

    if (strcmp(g_current_subsection, "master") == 0) {
        // --- TRUNK COMMIT ---
        // Parent is simply the current TRUNK_HEAD
        char trunk_head_file[PATH_MAX];
        get_trunk_head_file(node_path, trunk_head_file, sizeof(trunk_head_file));
        read_string_from_file(trunk_head_file, parent_commit_hash_out, HASH_STR_LEN);
        // Anchor is irrelevant for T-Commits
    } else {
        // --- SUBSECTION COMMIT ---
        char subsec_head_file[PATH_MAX];
        get_subsection_head_file(node_path, g_current_subsection, subsec_head_file, sizeof(subsec_head_file));

        char parent_or_anchor_hash[HASH_STR_LEN];
        if (read_string_from_file(subsec_head_file, parent_or_anchor_hash, sizeof(parent_or_anchor_hash)) != 0 || parent_or_anchor_hash[0] == '\0') {
            // 'unborn' branch, file is empty. (Shouldn't happen with new add-subs, but good to check)
            parent_commit_hash_out[0] = '\0';
            // Anchor is the current TRUNK_HEAD.
            char trunk_head_file[PATH_MAX];
            get_trunk_head_file(node_path, trunk_head_file, sizeof(trunk_head_file));
            read_string_from_file(trunk_head_file, anchor_hash_out, HASH_STR_LEN);
            // We can use the ANCHOR as the base for deltas
            strncpy(parent_commit_hash_out, anchor_hash_out, HASH_STR_LEN);
        } else {
            // File has a hash. Is it a T-COMMIT (anchor) or an S-COMMIT (parent)?
            size_t commit_size;
            char* commit_content = read_object(parent_or_anchor_hash, &commit_size);
            if (!commit_content) {
                 log_msg("Error: could not read commit %s from subsec file.", parent_or_anchor_hash);
                 return 0; // Will fail later, but won't crash here
            }

            if (strstr(commit_content, "type: S-COMMIT")) {
                // --- We are on an existing S-Commit chain ---
                log_msg("  > Found parent S-Commit.");
                // Parent is this S-Commit
                strncpy(parent_commit_hash_out, parent_or_anchor_hash, HASH_STR_LEN);
                // Anchor is this S-Commit's anchor
                char* anchor_line = strstr(commit_content, "anchor ");
                if (anchor_line) {
                    sscanf(anchor_line, "anchor %64s", anchor_hash_out);
                }
            } else {
                // --- This is the FIRST S-Commit on this branch ---
                // The hash in the file is our ANCHOR
                log_msg("  > Found T-Commit anchor.");
                strncpy(anchor_hash_out, parent_or_anchor_hash, HASH_STR_LEN);
                // The "parent" for deltas is this anchor
                strncpy(parent_commit_hash_out, parent_or_anchor_hash, HASH_STR_LEN);
                // The "parent" for the S-Commit chain is NULL
                // (which execute_commit_job will handle correctly)
            }
            free(commit_content);
        }
    }

    // Now, get the tree from whichever parent we found
    if (parent_commit_hash_out[0] != '\0') {
        size_t commit_size;
        char* commit_content = read_object(parent_commit_hash_out, &commit_size);
        if (commit_content) {
            char* tree_line = strstr(commit_content, "tree ");
            if (tree_line) {
                sscanf(tree_line, "tree %64s", parent_tree_hash_out);
            }
            free(commit_content);
        }
    }

    if (parent_commit_hash_out[0] == '\0') {
         log_msg("No parent commit found. Storing all files as new objects.");
    } else {
        log_msg("Using parent commit %s (tree %s) as base for deltas.", 
            parent_commit_hash_out, parent_tree_hash_out[0] ? parent_tree_hash_out : "n/a");
    }

    return 0;
}

static void generate_versions_json(const char* node_path) {
    char active_head_file[PATH_MAX];
    get_active_head_file(node_path, active_head_file, sizeof(active_head_file));

    char current_commit_hash[HASH_STR_LEN];
    if (read_string_from_file(active_head_file, current_commit_hash, sizeof(current_commit_hash)) != 0 || current_commit_hash[0] == '\0') {
        log_msg("No commits found for subsection '%s', skipping versions.json.", g_current_subsection);
        return;
    }

    ctz_json_value* root_array = ctz_json_new_array();
    if (!root_array) return;

    int depth = 0;
    while (current_commit_hash[0] != '\0' && depth < 2000) { // Safety limit
        size_t commit_size;
        char* commit_content = read_object(current_commit_hash, &commit_size);
        if (!commit_content) break;

        ctz_json_value* commit_obj = ctz_json_new_object();
        ctz_json_object_set_value(commit_obj, "commit_hash", ctz_json_new_string(current_commit_hash));

        // --- Extract data from commit object ---
        char* line_start = commit_content;
        char* line_end;
        char parent_hash[HASH_STR_LEN] = {0};
        int header_done = 0;

        while (line_start && line_start < (commit_content + commit_size)) {
            if (header_done) {
                // --- This is the message body ---
                if (line_start[0] == '\0') break; // End of content
                
                char* msg_end = strchr(line_start, '\n');
                if (msg_end) *msg_end = '\0'; // Remove trailing newline
                
                ctz_json_object_set_value(commit_obj, "version_tag", ctz_json_new_string(line_start));
                
                if (msg_end) *msg_end = '\n'; // Restore (though not strictly needed)
                break; // Message found, stop parsing
            }
            
            line_end = strchr(line_start, '\n');
            if (!line_end) break; // End of buffer
                
            if (line_start == line_end) {
                // This is the empty line "\n" separator
                header_done = 1;
                line_start = line_end + 1;
                continue;
            }

            *line_end = '\0'; // Temporarily terminate line
            
            if (strncmp(line_start, "type: ", 6) == 0) {
                ctz_json_object_set_value(commit_obj, "type", ctz_json_new_string(line_start + 6));
            } else if (strncmp(line_start, "tree ", 5) == 0) {
                ctz_json_object_set_value(commit_obj, "tree", ctz_json_new_string(line_start + 5));
            } else if (strncmp(line_start, "parent ", 7) == 0) {
                strncpy(parent_hash, line_start + 7, HASH_LEN);
                ctz_json_object_set_value(commit_obj, "parent", ctz_json_new_string(parent_hash));
            } else if (strncmp(line_start, "anchor ", 7) == 0) {
                ctz_json_object_set_value(commit_obj, "anchor", ctz_json_new_string(line_start + 7));
            } else if (strncmp(line_start, "promoted ", 9) == 0) {
                ctz_json_object_set_value(commit_obj, "promoted_commit", ctz_json_new_string(line_start + 9));
            } else if (strncmp(line_start, "author ", 7) == 0) {
                char* date_start = strstr(line_start, "> ");
                if (date_start) {
                    date_start += 2;
                    char* date_end_ptr = strstr(date_start, " +");
                    if (date_end_ptr) *date_end_ptr = '\0';
                    long long ts = atoll(date_start);
                    ctz_json_object_set_value(commit_obj, "timestamp", ctz_json_new_number((double)ts));
                }
            }
            
            *line_end = '\n'; // Restore newline
            line_start = line_end + 1;
        }
        
        ctz_json_array_push_value(root_array, commit_obj);
        
        // --- Move to next commit ---
        if (parent_hash[0] != '\0') {
            strncpy(current_commit_hash, parent_hash, HASH_STR_LEN);
        } else {
            current_commit_hash[0] = '\0'; // End of chain
        }
        
        free(commit_content);
        depth++;
    }

    // Write the JSON file
    char versions_file_path[PATH_MAX];
    get_subsection_versions_file(node_path, g_current_subsection, versions_file_path, sizeof(versions_file_path));
    
    char* json_string = ctz_json_stringify(root_array, 1); // Pretty print
    if (json_string) {
        write_string_to_file(versions_file_path, json_string);
        free(json_string);
        log_msg("Generated versions file at %s", versions_file_path);
    }
    ctz_json_free(root_array);
}


static int find_file_in_tree(const char* current_tree_hash, const char* path_to_find, 
                             char* blob_hash_out, mode_t* mode_out, 
                             double* entropy_out, char* type_out)
{
    if (type_out) *type_out = 0; // Default to not found

    size_t tree_size;
    char* tree_content = read_object(current_tree_hash, &tree_size);
    if (!tree_content) {
        log_msg("Error: Failed to read tree object: %s", current_tree_hash);
        return -1;
    }

    const char* separator = strchr(path_to_find, '/');
    char component[NAME_MAX + 1];
    const char* remainder = NULL;

    if (separator) {
        size_t len = separator - path_to_find;
        if (len > NAME_MAX) len = NAME_MAX;
        strncpy(component, path_to_find, len);
        component[len] = '\0';
        remainder = separator + 1;
    } else {
        snprintf(component, sizeof(component), "%s", path_to_find);
        remainder = NULL;
    }

    char* line = tree_content;
    char* next_line;
    int found = -1;

    while (line < tree_content + tree_size) {
        next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0';
        else if (strlen(line) == 0) break; // end of content

        mode_t mode;
        char type;
        char hash[HASH_STR_LEN];
        double entropy;
        char name[NAME_MAX + 1] = {0};
        char author_buf[128];

        if (sscanf(line, "%o %c %64s E:%lf U:%127[^\t]\t%[^\n]", 
                   &mode, &type, hash, &entropy, author_buf, name) != 6) 
        {
            if (sscanf(line, "%o %c %64s E:%lf\t%[^\n]", 
                       &mode, &type, hash, &entropy, name) != 5) 
            {
                if (next_line) line = next_line + 1; else break;
                continue;
            }
        }

        if (strcmp(name, component) == 0) {
            if (remainder && type == 'T') {
                found = find_file_in_tree(hash, remainder, blob_hash_out, mode_out, entropy_out, type_out);
            } 
            // --- MODIFIED: Recognize 'M' (Manifest) as a file-like type ---
            else if (!remainder && (type == 'B' || type == 'L' || type == 'M')) {
                strncpy(blob_hash_out, hash, HASH_STR_LEN);
                *mode_out = mode;
                *entropy_out = entropy;
                if (type_out) *type_out = type; // Pass back the type
                found = 0;
            }
            break; 
        }

        if (next_line) {
            line = next_line + 1;
        } else {
            break;
        }
    }

    free(tree_content);
    return found;
}


static void print_file_diff(const char* blob1_hash, const char* blob2_hash) {
    size_t size1, size2;
    char* content1 = read_object(blob1_hash, &size1);
    char* content2 = read_object(blob2_hash, &size2);
    if (!content1 || !content2) {
        if(content1) free(content1);
        if(content2) free(content2);
        return;
    }

    if (is_binary_file_for_diff(content1, size1) || is_binary_file_for_diff(content2, size2)) {
        log_msg_diff("    %s(Binary files differ)%s", C_CYAN, C_RESET);
        free(content1);
        free(content2);
        return;
    }

    int count1, count2;
    TextLine* lines1 = split_content_to_lines(content1, size1, &count1);
    TextLine* lines2 = split_content_to_lines(content2, size2, &count2);
    free(content1); // Free original buffers
    free(content2);

    if (!lines1 && !lines2) { // Both empty or failed
        return;
    }

    DiffOp* diffs = generate_diff(lines1, count1, lines2, count2);
    
    int added = 0, deleted = 0, same = 0;
    for (DiffOp* op = diffs; op; op = op->next) {
        if (op->type == DIFF_ADD) {
            log_msg_diff("%s+  %s%s", C_GREEN, op->content, C_RESET);
            added++;
        } else if (op->type == DIFF_DEL) {
            log_msg_diff("%s-  %s%s", C_RED, op->content, C_RESET);
            deleted++;
        } else {
            same++;
            log_msg_diff("   %s", op->content);
        }
    }


    if (added == 0 && deleted == 0 && same > 0) {
         log_msg_diff("    %s(Files are identical)%s", C_CYAN, C_RESET);
    }
    
    free_lines(lines1);
    free_lines(lines2);
    free_diff_ops(diffs);
}

static void diff_trees(const char* tree1_hash, const char* tree2_hash, const char* current_path) {
    if (strcmp(tree1_hash, tree2_hash) == 0) return;

    size_t size1, size2;
    // Tree objects are always full BLOBs
    char* content1 = read_object(tree1_hash, &size1);
    char* content2 = read_object(tree2_hash, &size2);
    if (!content1 || !content2) {
        if(content1) free(content1);
        if(content2) free(content2);
        return;
    }

    TreeEntry* list1 = NULL;
    TreeEntry* list2 = NULL;
    TreeEntry* tail = NULL;

    // Parse tree 1
    char* line = content1;
    while (line < content1 + size1) {
        char* next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0';
        else if (strlen(line) == 0) break;
        TreeEntry* e = calloc(1, sizeof(TreeEntry));
        if (!e) break;
        if (sscanf(line, "%o %c %64s E:%lf U:%127[^\t]\t%[^\n]", 
                   &e->mode, &e->type, e->hash, &e->entropy, e->author, e->name) != 6) {
            if (sscanf(line, "%o %c %64s E:%lf\t%[^\n]", 
                       &e->mode, &e->type, e->hash, &e->entropy, e->name) != 5) {
                free(e); if (next_line) line = next_line + 1; else break; continue;
            }
            e->author[0] = '\0';
        }
        if (tail) tail->next = e; else list1 = e;
        tail = e;
        if (next_line) line = next_line + 1; else break;
    }
    
    // Parse tree 2
    tail = NULL;
    line = content2;
    while (line < content2 + size2) {
        char* next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0';
        else if (strlen(line) == 0) break;
        TreeEntry* e = calloc(1, sizeof(TreeEntry));
        if (!e) break;
        if (sscanf(line, "%o %c %64s E:%lf U:%127[^\t]\t%[^\n]", 
                   &e->mode, &e->type, e->hash, &e->entropy, e->author, e->name) != 6) {
             if (sscanf(line, "%o %c %64s E:%lf\t%[^\n]", 
                       &e->mode, &e->type, e->hash, &e->entropy, e->name) != 5) {
                free(e); if (next_line) line = next_line + 1; else break; continue;
            }
            e->author[0] = '\0';
        }
        if (tail) tail->next = e; else list2 = e;
        tail = e;
        if (next_line) line = next_line + 1; else break;
    }
    free(content1);
    free(content2);

    // Compare lists
    for (TreeEntry* p1 = list1; p1; p1 = p1->next) {
        TreeEntry* p2 = NULL;
        for (p2 = list2; p2; p2 = p2->next) {
            if (strcmp(p1->name, p2->name) == 0) break;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s%s", current_path, p1->name);

        if (p2) {
            p2->mode = 0; // Mark as "visited"
            if (strcmp(p1->hash, p2->hash) != 0) {
                if (p1->type == 'T' && p2->type == 'T') {
                    char dir_path[PATH_MAX];
                    snprintf(dir_path, sizeof(dir_path), "%s/", full_path);
                    diff_trees(p1->hash, p2->hash, dir_path);
                } else if ((p1->type == 'B' || p1->type == 'L') && (p2->type == 'B' || p2->type == 'L')) {
                        log_msg_diff("%s--- Modified: %s%s (E: %.4f -> %.4f) (By: %s)", 
                                 C_YELLOW, full_path, C_RESET, 
                                 p1->entropy, p2->entropy, p2->author[0] ? p2->author : "unknown");
                    print_file_diff(p1->hash, p2->hash);
                } else {
                    log_msg_diff("%s--- Type changed: %s%s", C_CYAN, full_path, C_RESET);
                }
            }
            else if (p1->entropy != p2->entropy || strcmp(p1->author, p2->author) != 0) {
               log_msg_diff("%s--- Metadata Change: %s%s (E: %.4f -> %.4f) (By: %s -> %s)", 
                    C_CYAN, full_path, C_RESET, p1->entropy, p2->entropy, 
                    p1->author[0] ? p1->author : "unknown",
                    p2->author[0] ? p2->author : "unknown");
            }
        } else {
            log_msg_diff("%s--- Deleted: %s%s", C_RED, full_path, C_RESET);
        }
    }

    for (TreeEntry* p2 = list2; p2; p2 = p2->next) {
        if (p2->mode != 0) {
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s%s", current_path, p2->name);
            log_msg_diff("%s--- Added: %s%s (E: %.4f) (By: %s)", 
                         C_GREEN, full_path, C_RESET, 
                         p2->entropy, p2->author[0] ? p2->author : "unknown");
        }
    }
    free_tree_list(list1);
    free_tree_list(list2);
}


// --- NEW Job Functions ---

// Helper struct for merge_trees
typedef struct MergeEntry {
    char name[NAME_MAX + 1];
    char type_b, type_o, type_t; // 'B', 'T', 'L', or 0 (not present)
    char hash_b[HASH_STR_LEN];
    char hash_o[HASH_STR_LEN];
    char hash_t[HASH_STR_LEN];
    mode_t mode_o, mode_t;
    double ent_o, ent_t;
    char auth_o[128], auth_t[128];
    struct MergeEntry* next;
} MergeEntry;

// Helper to parse a tree into a linked list
static MergeEntry* parse_tree_for_merge(const char* tree_hash, char type_char) {
    if (!tree_hash || tree_hash[0] == '\0') return NULL;
    
    size_t tree_size;
    char* tree_content = read_object(tree_hash, &tree_size);
    if (!tree_content) return NULL;

    MergeEntry* head = NULL;
    char* line = tree_content;
    while (line < tree_content + tree_size) {
        char* next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0';
        else if (strlen(line) == 0) break;

        MergeEntry* e = calloc(1, sizeof(MergeEntry));
        if (!e) break;

        char hash[HASH_STR_LEN];
        char type;
        mode_t mode;
        double entropy;
        char author[128];
        
        if (sscanf(line, "%o %c %64s E:%lf U:%127[^\t]\t%[^\n]", 
                   &mode, &type, hash, &entropy, author, e->name) != 6) {
            if (sscanf(line, "%o %c %64s E:%lf\t%[^\n]", 
                       &mode, &type, hash, &entropy, e->name) != 5) {
                free(e); if (next_line) line = next_line + 1; else break; continue;
            }
            author[0] = '\0';
        }

        if (type_char == 'b') {
            e->type_b = type;
            strncpy(e->hash_b, hash, HASH_STR_LEN);
        } else if (type_char == 'o') {
            e->type_o = type; strncpy(e->hash_o, hash, HASH_STR_LEN);
            e->mode_o = mode; e->ent_o = entropy; strncpy(e->auth_o, author, 127);
        } else if (type_char == 't') {
            e->type_t = type; strncpy(e->hash_t, hash, HASH_STR_LEN);
            e->mode_t = mode; e->ent_t = entropy; strncpy(e->auth_t, author, 127);
        }
        
        e->next = head;
        head = e;
        if (next_line) line = next_line + 1; else break;
    }
    free(tree_content);
    return head; // Note: This list is in reverse alphabetical order
}

static void free_merge_list(MergeEntry* head) {
    while(head) {
        MergeEntry* next = head->next;
        free(head);
        head = next;
    }
}

// Find/create a merge entry in the master list
static MergeEntry* find_or_create_merge_entry(MergeEntry** master_head, const char* name) {
    for (MergeEntry* e = *master_head; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    MergeEntry* new_e = calloc(1, sizeof(MergeEntry));
    if (!new_e) return NULL;
    strncpy(new_e->name, name, NAME_MAX);
    new_e->next = *master_head;
    *master_head = new_e;
    return new_e;
}

// Helper to populate the master list from a component list
static void populate_master_list(MergeEntry** master_head, MergeEntry* component_list, char type_char) {
    for (MergeEntry* e = component_list; e; e = e->next) {
        MergeEntry* me = find_or_create_merge_entry(master_head, e->name);
        if (!me) continue;
        if (type_char == 'b') {
            me->type_b = e->type_b; strncpy(me->hash_b, e->hash_b, HASH_STR_LEN);
        } else if (type_char == 'o') {
            me->type_o = e->type_o; strncpy(me->hash_o, e->hash_o, HASH_STR_LEN);
            me->mode_o = e->mode_o; me->ent_o = e->ent_o; strncpy(me->auth_o, e->auth_o, 127);
        } else if (type_char == 't') {
            me->type_t = e->type_t; strncpy(me->hash_t, e->hash_t, HASH_STR_LEN);
            me->mode_t = e->mode_t; me->ent_t = e->ent_t; strncpy(me->auth_t, e->auth_t, 127);
        }
    }
}

static int merge_trees(const char* base_tree_hash, const char* ours_tree_hash, 
                       const char* theirs_tree_hash, char* merged_tree_hash_out,
                       ctz_json_value* history_json)
{
    (void)history_json; // Unused for now
    log_msg("--- Starting 3-Way Tree Merge ---");
    log_msg("Base:   %s", base_tree_hash[0] ? base_tree_hash : "NULL");
    log_msg("Ours:   %s (Trunk)", ours_tree_hash[0] ? ours_tree_hash : "NULL");
    log_msg("Theirs: %s (Subsection)", theirs_tree_hash[0] ? theirs_tree_hash : "NULL");

    // 1. Handle fast-forwards and trivial cases
    if (strcmp(base_tree_hash, ours_tree_hash) == 0 && strcmp(base_tree_hash, theirs_tree_hash) != 0) {
        log_msg("Trunk unchanged. Fast-forwarding to subsection tree.");
        strncpy(merged_tree_hash_out, theirs_tree_hash, HASH_STR_LEN);
        return 0; // Success
    }
    if (strcmp(base_tree_hash, theirs_tree_hash) == 0 && strcmp(base_tree_hash, ours_tree_hash) != 0) {
        log_msg("Subsection unchanged. Nothing to promote.");
        strncpy(merged_tree_hash_out, ours_tree_hash, HASH_STR_LEN); // Just use trunk
        return 0; // Success
    }
    if (strcmp(ours_tree_hash, theirs_tree_hash) == 0) {
        log_msg("Trunk and subsection are identical.");
        strncpy(merged_tree_hash_out, ours_tree_hash, HASH_STR_LEN);
        return 0; // Success
    }

    // 2. Parse all three trees into component lists
    MergeEntry* list_b = parse_tree_for_merge(base_tree_hash, 'b');
    MergeEntry* list_o = parse_tree_for_merge(ours_tree_hash, 'o');
    MergeEntry* list_t = parse_tree_for_merge(theirs_tree_hash, 't');

    // 3. Create one master list of all file/dir names
    MergeEntry* master_list = NULL;
    populate_master_list(&master_list, list_b, 'b');
    populate_master_list(&master_list, list_o, 'o');
    populate_master_list(&master_list, list_t, 't');

    free_merge_list(list_b);
    free_merge_list(list_o);
    free_merge_list(list_t);

    // 4. Iterate the master list and build the new tree content
    TreeEntry* new_tree_head = NULL;
    TreeEntry* new_tree_tail = NULL;
    int conflict = 0;

    for (MergeEntry* me = master_list; me; me = me->next) {
        int changed_o = (me->type_o != me->type_b || strcmp(me->hash_o, me->hash_b) != 0);
        int changed_t = (me->type_t != me->type_b || strcmp(me->hash_t, me->hash_b) != 0);

        char result_hash[HASH_STR_LEN] = {0};
        char result_type = 0;
        mode_t result_mode = 0;
        double result_entropy = 0;
        char* result_author = NULL;

        // --- MERGE SCENARIOS ---
        if (!changed_o && !changed_t) { // 1. Unchanged
            if (me->type_b) { // Existed in base
                result_type = me->type_b; strncpy(result_hash, me->hash_b, HASH_STR_LEN);
                result_mode = me->mode_o; result_entropy = me->ent_o; result_author = me->auth_o;
            } else { // Was not in base, ours, or theirs (should be impossible)
                continue;
            }
        } else if (!changed_o && changed_t) { // 2. Changed in Theirs only
            result_type = me->type_t; strncpy(result_hash, me->hash_t, HASH_STR_LEN);
            result_mode = me->mode_t; result_entropy = me->ent_t; result_author = me->auth_t;
        } else if (changed_o && !changed_t) { // 3. Changed in Ours only
            result_type = me->type_o; strncpy(result_hash, me->hash_o, HASH_STR_LEN);
            result_mode = me->mode_o; result_entropy = me->ent_o; result_author = me->auth_o;
        } else { // 4. Changed in BOTH (Conflict or Recursive Merge)
            if (me->type_o == 'T' && me->type_t == 'T') {
                // --- Recursive Merge for Subdirectory ---
                log_msg("  Recursing into subdir: %s", me->name);
                if (merge_trees(me->hash_b, me->hash_o, me->hash_t, result_hash, history_json) != 0) {
                    log_msg("Error: Conflict in subdirectory '%s'. Aborting merge.", me->name);
                    conflict = 1; break;
                }
                result_type = 'T';
                result_mode = me->mode_o; // Use 'Ours' mode
                result_entropy = 0;
                result_author = "n/a";
            } else {
                // --- File-level Conflict ---
                if (strcmp(me->hash_o, me->hash_t) == 0) { // Both changed to the same thing
                    result_type = me->type_o; strncpy(result_hash, me->hash_o, HASH_STR_LEN);
                    result_mode = me->mode_o; result_entropy = me->ent_o; result_author = me->auth_o;
                } else {
                    log_msg("Error: CONFLICT (content) in '%s'.", me->name);
                    log_msg("  Base:   %s", me->hash_b[0] ? me->hash_b : "NULL");
                    log_msg("  Ours:   %s", me->hash_o[0] ? me->hash_o : "NULL");
                    log_msg("  Theirs: %s", me->hash_t[0] ? me->hash_t : "NULL");
                    conflict = 1; break;
                }
            }
        }
        
        // --- Add result to new tree list ---
        if (result_type != 0) { // type=0 means file was deleted (from both, or one and not changed in other)
            TreeEntry* new_e = calloc(1, sizeof(TreeEntry));
            if (!new_e) { conflict = 1; break; }
            
            strncpy(new_e->name, me->name, NAME_MAX);
            new_e->type = result_type;
            strncpy(new_e->hash, result_hash, HASH_STR_LEN);
            new_e->mode = result_mode;
            new_e->entropy = result_entropy;
            strncpy(new_e->author, result_author ? result_author : "unknown", 127);
            
            if (new_tree_tail) new_tree_tail->next = new_e; else new_tree_head = new_e;
            new_tree_tail = new_e;
        }
    }
    
    free_merge_list(master_list);
    if (conflict) {
        free_tree_list(new_tree_head);
        return -1; // Abort
    }

    // 5. Serialize the new tree list and write the object
    size_t tree_content_size = 0;
    TreeEntry* current = new_tree_head;
    while (current) {
        tree_content_size += snprintf(NULL, 0, "%o %c %s E:%.4f U:%s\t%s\n",
                                      current->mode & 07777,
                                      current->type, current->hash, 
                                      current->entropy,
                                      current->author[0] ? current->author : "unknown",
                                      current->name);
        current = current->next;
    }
    if (tree_content_size == 0) tree_content_size = 1; 
    
    char* tree_content = malloc(tree_content_size + 1);
    if (!tree_content) { free_tree_list(new_tree_head); return -1; }
    char* ptr = tree_content;
    current = new_tree_head;
    while (current) {
        ptr += sprintf(ptr, "%o %c %s E:%.4f U:%s\t%s\n",
                       current->mode & 07777, current->type, current->hash, 
                       current->entropy,
                       current->author[0] ? current->author : "unknown",
                       current->name);
        current = current->next;
    }
    *ptr = '\0'; 
    if (tree_content_size == 1 && *tree_content == '\0') {
        tree_content_size = 0; // Handle empty directory case
    }

    get_buffer_hash(tree_content, tree_content_size, merged_tree_hash_out);
    write_blob_object(merged_tree_hash_out, tree_content, tree_content_size); // Trees are BLOBs

    free(tree_content);
    free_tree_list(new_tree_head);
    
    log_msg("--- Merge Succeeded. New Tree: %s ---", merged_tree_hash_out);
    return 0; // Success
}

static void execute_add_subs_job(const char* node_path, const char* new_subsection_name) {
    if (strcmp(new_subsection_name, "master") == 0) {
        log_msg("Error: Cannot create subsection named 'master'. It is reserved.");
        return;
    }

    // --- 1. Get the current TRUNK_HEAD commit hash ---
    char trunk_head_file[PATH_MAX];
    char trunk_commit_hash[HASH_STR_LEN];
    get_trunk_head_file(node_path, trunk_head_file, sizeof(trunk_head_file));
    
    if (read_string_from_file(trunk_head_file, trunk_commit_hash, sizeof(trunk_commit_hash)) != 0 || trunk_commit_hash[0] == '\0') {
        log_msg("Error: Cannot create subsection. The 'master' (Trunk) has no commits.");
        return;
    }

    // --- 2. Ensure subsections directory exists ---
    char subsections_dir[PATH_MAX];
    get_subsections_dir(node_path, subsections_dir, sizeof(subsections_dir));

    char mkdir_cmd[PATH_MAX + 10];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", subsections_dir);
    if (system(mkdir_cmd) != 0) {
        log_msg("Error: Failed to create subsections directory at %s.", subsections_dir);
        return;
    }

    // --- 3. Check if new subsection file already exists ---
    char subsec_file_path[PATH_MAX];
    get_subsection_head_file(node_path, new_subsection_name, subsec_file_path, sizeof(subsec_file_path));

    struct stat st;
    if (stat(subsec_file_path, &st) == 0) {
        log_msg("Error: Subsection '%s' already exists.", new_subsection_name);
        return;
    }

    // --- 4. Write the TRUNK_HEAD hash into the new file ---
    if (write_string_to_file(subsec_file_path, trunk_commit_hash) != 0) {
        log_msg("Error: Failed to create subsection file at %s: %s", subsec_file_path, strerror(errno));
        return;
    }

    log_msg("Successfully created subsection '%s'.", new_subsection_name);
    log_msg("It is now anchored to TRUNK_HEAD commit: %s", trunk_commit_hash);
}

static void execute_promote_job(const char* node_path, const char* subsection_name, const char* message, uid_t user_id, const char* username, const char* delete_flag) {
    log_msg("Attempting to promote subsection '%s' to Trunk...", subsection_name);

    char trunk_head_file[PATH_MAX];
    char subsec_head_file[PATH_MAX];
    get_trunk_head_file(node_path, trunk_head_file, sizeof(trunk_head_file));
    get_subsection_head_file(node_path, subsection_name, subsec_head_file, sizeof(subsec_head_file));

    char ours_commit_hash[HASH_STR_LEN]; // Current Trunk
    char theirs_commit_hash[HASH_STR_LEN]; // Head of subsection
    char base_commit_hash[HASH_STR_LEN];   // Anchor of subsection

    if (read_string_from_file(trunk_head_file, ours_commit_hash, sizeof(ours_commit_hash)) != 0) {
        ours_commit_hash[0] = '\0'; // Empty trunk
    }
    if (read_string_from_file(subsec_head_file, theirs_commit_hash, sizeof(theirs_commit_hash)) != 0) {
        log_msg("Error: Subsection '%s' is empty. Nothing to promote.", subsection_name);
        return;
    }

    // Find the anchor (base)
    size_t theirs_commit_size;
    char* theirs_commit_content = read_object(theirs_commit_hash, &theirs_commit_size);
    if (!theirs_commit_content) {
        log_msg("Error: Failed to read subsection commit object %s.", theirs_commit_hash);
        return;
    }
    char* anchor_line = strstr(theirs_commit_content, "anchor ");
    if (anchor_line) {
        sscanf(anchor_line, "anchor %64s", base_commit_hash);
    } else {
        log_msg("Error: Invalid subsection commit %s. Missing 'anchor' field.", theirs_commit_hash);
        free(theirs_commit_content);
        return;
    }

    // Get tree hashes for all three commits
    char ours_tree_hash[HASH_STR_LEN] = {0};
    char theirs_tree_hash[HASH_STR_LEN] = {0};
    char base_tree_hash[HASH_STR_LEN] = {0};

    sscanf(strstr(theirs_commit_content, "tree "), "tree %64s", theirs_tree_hash);
    free(theirs_commit_content);

    if (ours_commit_hash[0] != '\0') {
        size_t ours_commit_size;
        char* ours_commit_content = read_object(ours_commit_hash, &ours_commit_size);
        if (ours_commit_content) {
            sscanf(strstr(ours_commit_content, "tree "), "tree %64s", ours_tree_hash);
            free(ours_commit_content);
        }
    }
    if (base_commit_hash[0] != '\0') {
        size_t base_commit_size;
        char* base_commit_content = read_object(base_commit_hash, &base_commit_size);
        if (base_commit_content) {
            sscanf(strstr(base_commit_content, "tree "), "tree %64s", base_tree_hash);
            free(base_commit_content);
        }
    }

    // Perform the 3-way merge
    char merged_tree_hash[HASH_STR_LEN];
    if (merge_trees(base_tree_hash, ours_tree_hash, theirs_tree_hash, merged_tree_hash, NULL) != 0) {
        log_msg("Merge failed. Aborting promotion.");
        return;
    }

    // Create the new promotion commit (a T-Commit)
    char commit_content[PATH_MAX * 2];
    long long now = (long long)time(NULL);
    int content_len;
    char full_message[PATH_MAX];
    snprintf(full_message, sizeof(full_message), "Promoted subsection '%s': %s", subsection_name, message);

    if (ours_commit_hash[0] == '\0') { // Empty trunk
        content_len = snprintf(commit_content, sizeof(commit_content),
                               "type: T-COMMIT\ntree %s\npromoted %s\nauthor %s <%d@exodus> %lld +0000\ncommitter %s <%d@exodus> %lld +0000\n\n%s\n",
                               merged_tree_hash, theirs_commit_hash, 
                               username, (int)user_id, now, username, (int)user_id, now, full_message);
    } else {
        content_len = snprintf(commit_content, sizeof(commit_content),
                               "type: T-COMMIT\ntree %s\nparent %s\npromoted %s\nauthor %s <%d@exodus> %lld +0000\ncommitter %s <%d@exodus> %lld +0000\n\n%s\n",
                               merged_tree_hash, ours_commit_hash, theirs_commit_hash, 
                               username, (int)user_id, now, username, (int)user_id, now, full_message);
    }

    char new_commit_hash[HASH_STR_LEN];
    get_buffer_hash(commit_content, content_len, new_commit_hash);
    if (write_blob_object(new_commit_hash, commit_content, content_len) != 0) {
        log_msg("Error: Failed to write promotion commit object.");
        return;
    }

    // Update TRUNK_HEAD
    write_string_to_file(trunk_head_file, new_commit_hash);
    
    if (strcmp(delete_flag, "--delete") == 0) {
        log_msg("Promotion successful. Deleting subsection file: %s", subsec_head_file);
        if (remove(subsec_head_file) != 0) {
            log_msg("Warning: Failed to remove subsection file '%s'.", subsec_head_file);
        } else {
            // Also remove its versions.json file
            char versions_file_path[PATH_MAX];
            get_subsection_versions_file(node_path, subsection_name, versions_file_path, sizeof(versions_file_path));
            if (remove(versions_file_path) != 0) {
                // This is not critical, just a warning.
                log_msg("Warning: Failed to remove subsection versions file '%s'.", versions_file_path);
            }
        }
    } else {
        log_msg("Subsection '%s' was kept as requested.", subsection_name);
    }

    char original_subsection[NAME_MAX + 1];
    strncpy(original_subsection, g_current_subsection, sizeof(original_subsection));
    strncpy(g_current_subsection, "master", sizeof(g_current_subsection));
    
    generate_versions_json(node_path); // This will now correctly use TRUNK_HEAD
    
    strncpy(g_current_subsection, original_subsection, sizeof(g_current_subsection)); // Restore it
    // --- END FIX ---

    log_msg("Successfully promoted '%s' to Trunk with commit %s.", subsection_name, new_commit_hash);
}

static void execute_commit_job(const char* node_name, const char* node_path, const char* version_tag, uid_t user_id, const char* username) {
    (void)node_name; 
    
    char active_head_file[PATH_MAX];
    get_active_head_file(node_path, active_head_file, sizeof(active_head_file));

    log_msg("Initializing object database...");
    load_ignore_list(node_path);

    char mkdir_cmd_obj[PATH_MAX + 10];
    snprintf(mkdir_cmd_obj, sizeof(mkdir_cmd_obj), "mkdir -p \"%s\"", g_objects_dir);
    (void)system(mkdir_cmd_obj); 

    char mkdir_cmd_bblk[PATH_MAX + 10];
    snprintf(mkdir_cmd_bblk, sizeof(mkdir_cmd_bblk), "mkdir -p \"%s\"", g_bblk_objects_dir);
    (void)system(mkdir_cmd_bblk);

    char mkdir_cmd_mobj[PATH_MAX + 10];
    snprintf(mkdir_cmd_mobj, sizeof(mkdir_cmd_mobj), "mkdir -p \"%s\"", g_mobj_objects_dir);
    (void)system(mkdir_cmd_mobj);

    // --- NEW: Get parent/anchor data ---
    char parent_commit_hash[HASH_STR_LEN];
    char parent_tree_hash[HASH_STR_LEN];
    char anchor_hash[HASH_STR_LEN];
    get_parent_commit_data(node_path, parent_commit_hash, parent_tree_hash, anchor_hash);
    // --- END NEW ---

    char history_file_path[PATH_MAX];
    snprintf(history_file_path, sizeof(history_file_path), "%s/.log/history.json", node_path);
    
    char error_buf[256];
    ctz_json_value* history_json = ctz_json_load_file(history_file_path, error_buf, sizeof(error_buf));
    if (!history_json) {
        log_msg("Warning: Could not load history.json. Per-file author metadata will be 'unknown'.");
    }

    log_msg("Hashing node for subsection '%s'...", g_current_subsection);
    strncpy(g_node_root_path, node_path, sizeof(g_node_root_path)-1);
 
    char root_tree_hash[HASH_STR_LEN];
    if (build_tree_recursive(node_path, parent_tree_hash[0] ? parent_tree_hash : NULL, history_json, root_tree_hash) == NULL) {
        log_msg("Error: Failed to build root tree.");
        g_node_root_path[0] = '\0'; free_ignore_list();
        if (history_json) ctz_json_free(history_json);
        return; 
    }

    if (history_json) {
        ctz_json_free(history_json);
    }

    log_msg("Creating commit object...");

    char commit_content[PATH_MAX * 2];
    long long now = (long long)time(NULL);
    int content_len;

    // --- NEW: Create T-COMMIT or S-COMMIT ---
    if (strcmp(g_current_subsection, "master") == 0) {
        // --- Create T-COMMIT ---
        log_msg("Creating T-COMMIT for Trunk (master)...");
        if (parent_commit_hash[0] == '\0') { // First commit
            content_len = snprintf(commit_content, sizeof(commit_content),
                                   "type: T-COMMIT\ntree %s\nauthor %s <%d@exodus> %lld +0000\ncommitter %s <%d@exodus> %lld +0000\n\n%s\n",
                                   root_tree_hash, username, (int)user_id, now, username, (int)user_id, now, version_tag);
        } else {
            content_len = snprintf(commit_content, sizeof(commit_content),
                                   "type: T-COMMIT\ntree %s\nparent %s\nauthor %s <%d@exodus> %lld +0000\ncommitter %s <%d@exodus> %lld +0000\n\n%s\n",
                                   root_tree_hash, parent_commit_hash, username, (int)user_id, now, username, (int)user_id, now, version_tag);
        }
    } else {
        // --- Create S-COMMIT ---
         log_msg("Creating S-COMMIT for subsection '%s'...", g_current_subsection);
        if (anchor_hash[0] == '\0') {
            log_msg("Error: Cannot create S-COMMIT. Invalid anchor data (TRUNK_HEAD is empty?).");
            g_node_root_path[0] = '\0'; free_ignore_list(); return;
        }

        char* parent_s_commit_hash = NULL;
        // Check if the parent commit was an S-Commit or the T-Commit anchor
        if (parent_commit_hash[0] != '\0') {
            size_t parent_commit_size;
            char* parent_content = read_object(parent_commit_hash, &parent_commit_size);
            if (parent_content && strstr(parent_content, "type: S-COMMIT")) {
                parent_s_commit_hash = parent_commit_hash;
            }
            if (parent_content) free(parent_content);
        }


        if (parent_s_commit_hash) { // Continuing an existing S-Commit chain
             content_len = snprintf(commit_content, sizeof(commit_content),
                                   "type: S-COMMIT\ntree %s\nparent %s\nanchor %s\nauthor %s <%d@exodus> %lld +0000\ncommitter %s <%d@exodus> %lld +0000\n\n%s\n",
                                   root_tree_hash, parent_s_commit_hash, anchor_hash, 
                                   username, (int)user_id, now, username, (int)user_id, now, version_tag);
        } else { // First S-Commit on this branch
             content_len = snprintf(commit_content, sizeof(commit_content),
                                   "type: S-COMMIT\ntree %s\nanchor %s\nauthor %s <%d@exodus> %lld +0000\ncommitter %s <%d@exodus> %lld +0000\n\n%s\n",
                                   root_tree_hash, anchor_hash, 
                                   username, (int)user_id, now, username, (int)user_id, now, version_tag);
        }
    }
    // --- END NEW ---

    char new_commit_hash[HASH_STR_LEN];
    get_buffer_hash(commit_content, content_len, new_commit_hash);
    if (write_blob_object(new_commit_hash, commit_content, content_len) != 0) { // Commits are BLOBs
        log_msg("Error: Failed to write commit object.");
        g_node_root_path[0] = '\0'; free_ignore_list(); return;
    }

    log_msg("Updating references for '%s'...", g_current_subsection);
    write_string_to_file(active_head_file, new_commit_hash); // Writes to the correct HEAD file
    
    log_msg("Clearing node activity log (history.json)...");
    snprintf(history_file_path, sizeof(history_file_path), "%s/.log/history.json", node_path);
    FILE* f = fopen(history_file_path, "w");
    if (f) {
        fprintf(f, "[]\n"); 
        fclose(f);
    } else {
        log_msg("Warning: Could not clear history.json at %s", history_file_path);
    }

    generate_versions_json(node_path);

    log_msg("Snapshot commit complete.");
    g_node_root_path[0] = '\0';
    free_ignore_list();
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (strstr(fpath, "/.log") != NULL || strcmp(fpath, g_unlink_root_path) == 0) {
        return 0;
    }
    int rv = remove(fpath);
    if (rv) log_msg("Failed to remove '%s': %s", fpath, strerror(errno));
    return rv;
}

static int unpack_tree_recursive(const char* tree_hash, const char* current_dest_path) {
    size_t tree_size;
    char* tree_content = read_object(tree_hash, &tree_size);
    if (!tree_content) {
        log_msg("Failed to read tree object: %s", tree_hash);
        return -1;
    }

    char* line = tree_content;
    char* next_line;
    while (line < tree_content + tree_size) {
        next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0';
        else if (strlen(line) == 0) break;

        mode_t mode;
        char type;
        char hash[HASH_STR_LEN];
        double entropy;
        char name[NAME_MAX + 1] = {0};
        char author_buf[128];

        if (sscanf(line, "%o %c %64s E:%lf U:%127[^\t]\t%[^\n]", 
                   &mode, &type, hash, &entropy, author_buf, name) != 6) {
            if (sscanf(line, "%o %c %64s E:%lf\t%[^\n]", 
                       &mode, &type, hash, &entropy, name) != 5) {
                if (next_line) line = next_line + 1; else break;
                continue;
            }
        }

        char entry_dest_path[PATH_MAX];
        snprintf(entry_dest_path, sizeof(entry_dest_path), "%s/%s", current_dest_path, name);
        const char* relative_path_entry = entry_dest_path + strlen(g_node_root_path);
        if (*relative_path_entry == '/') relative_path_entry++;

        switch (type) {
            case 'T':
                log_msg("Creating dir: %s", relative_path_entry);
                if (mkdir(entry_dest_path, mode | 0700) != 0 && errno != EEXIST) {
                    log_msg("Failed to create dir '%s': %s", entry_dest_path, strerror(errno));
                } else {
                    if (unpack_tree_recursive(hash, entry_dest_path) != 0) {
                        free(tree_content); return -1;
                    }
                    if (chmod(entry_dest_path, mode) != 0) {
                         log_msg("Failed to chmod dir '%s': %s", entry_dest_path, strerror(errno));
                    }
                }
                break;
            case 'B':
                log_msg("Restoring file: %s", relative_path_entry);
                size_t blob_size;
                char* blob_content = read_object(hash, &blob_size);
                if (!blob_content) break;
                FILE* f_blob = fopen(entry_dest_path, "wb");
                if (!f_blob) { free(blob_content); break; }
                fwrite(blob_content, 1, blob_size, f_blob);
                fclose(f_blob);
                free(blob_content);
                chmod(entry_dest_path, mode);
                break;
             case 'L':
                log_msg("Restoring link: %s", relative_path_entry);
                size_t target_size;
                char* target_content = read_object(hash, &target_size);
                if (!target_content) break;
                // read_object adds a null-terminator, but let's be safe
                char* target_path = malloc(target_size + 1);
                if (!target_path) { free(target_content); break; }
                memcpy(target_path, target_content, target_size);
                target_path[target_size] = '\0';
                free(target_content);
                symlink(target_path, entry_dest_path); // Don't check return, might fail
                free(target_path);
                break;
                
            // --- NEW: Add case for Manifests ---
            case 'M':
                log_msg("Restoring manifest: %s", relative_path_entry);
                ManifestData* manifest = read_mobj_object(hash);
                if (!manifest) {
                    log_msg("Error: Failed to read manifest object %s", hash);
                    break;
                }
                if (reconstruct_file_from_manifest(manifest, entry_dest_path) != 0) {
                    log_msg("Error: Failed to reconstruct file from manifest %s", hash);
                    // Error is already logged by the helper
                }
                free_manifest_data(manifest);
                break;
            // --- END NEW ---
        }
        if (next_line) line = next_line + 1; else break;
    }
    free(tree_content);
    return 0;
}

static int unpack_file_entry(const char* hash, char type, mode_t mode, const char* dest_path) {
    switch (type) {
        case 'B': {
            size_t blob_size;
            char* blob_content = read_object(hash, &blob_size);
            if (!blob_content) return -1;
            FILE* f_blob = fopen(dest_path, "wb");
            if (!f_blob) { free(blob_content); return -1; }
            fwrite(blob_content, 1, blob_size, f_blob);
            fclose(f_blob);
            free(blob_content);
            chmod(dest_path, mode);
            break;
        }
        case 'L': {
            size_t target_size;
            char* target_content = read_object(hash, &target_size);
            if (!target_content) return -1;
            target_content[target_size] = '\0'; // read_object should do this, but for safety
            symlink(target_content, dest_path);
            free(target_content);
            break;
        }
        case 'M': {
            ManifestData* manifest = read_mobj_object(hash);
            if (!manifest) {
                log_msg("Error: Failed to read manifest object %s", hash);
                return -1;
            }
            if (reconstruct_file_from_manifest(manifest, dest_path) != 0) {
                log_msg("Error: Failed to reconstruct file from manifest %s", hash);
                free_manifest_data(manifest);
                return -1;
            }
            free_manifest_data(manifest);
            break;
        }
        default:
            log_msg("Error: Unknown type '%c' for unpack: %s", type, dest_path);
            return -1;
    }
    return 0;
}

static int apply_tree_diff(const char* old_tree_hash, const char* new_tree_hash, const char* base_path) {
    // 1. Parse both trees into component lists
    // Note: parse_tree_for_merge is a bit of a misnomer, but it works perfectly
    // 'o' = old, 't' = target/new
    MergeEntry* list_o = parse_tree_for_merge(old_tree_hash, 'o');
    MergeEntry* list_t = parse_tree_for_merge(new_tree_hash, 't');

    // 2. Create one master list of all file/dir names
    MergeEntry* master_list = NULL;
    populate_master_list(&master_list, list_o, 'o');
    populate_master_list(&master_list, list_t, 't');

    free_merge_list(list_o);
    free_merge_list(list_t);

    // 3. Iterate the master list and apply changes
    for (MergeEntry* me = master_list; me; me = me->next) {
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, me->name);
        
        const char* relative_path_entry = full_path + strlen(g_node_root_path);
        if (*relative_path_entry == '/') relative_path_entry++;

        int existed_in_old = (me->type_o != 0);
        int exists_in_new = (me->type_t != 0);

        if (existed_in_old && !exists_in_new) {
            // --- 1. DELETED ---
            log_msg("Deleting: %s", relative_path_entry);
            if (me->type_o == 'T') {
                // We must recursively delete the directory's contents first
                // A full implementation would use nftw, but for this,
                // we'll just recurse with a NULL new_tree_hash.
                apply_tree_diff(me->hash_o, NULL, full_path);
                if (rmdir(full_path) != 0) {
                     log_msg("Warning: rmdir failed for '%s': %s", full_path, strerror(errno));
                }
            } else {
                if (remove(full_path) != 0) {
                     log_msg("Warning: remove failed for '%s': %s", full_path, strerror(errno));
                }
            }
        } else if (!existed_in_old && exists_in_new) {
            // --- 2. NEW ---
            log_msg("Creating: %s", relative_path_entry);
            if (me->type_t == 'T') {
                if (mkdir(full_path, me->mode_t | 0700) != 0 && errno != EEXIST) {
                    log_msg("Failed to create dir '%s': %s", full_path, strerror(errno));
                } else {
                    apply_tree_diff(NULL, me->hash_t, full_path); // Recurse to populate
                    chmod(full_path, me->mode_t);
                }
            } else {
                unpack_file_entry(me->hash_t, me->type_t, me->mode_t, full_path);
            }
        } else if (existed_in_old && exists_in_new) {
            // --- 3. POTENTIALLY MODIFIED ---
            if (strcmp(me->hash_o, me->hash_t) == 0) {
                // Hashes are identical. DO NOTHING.
                continue;
            }

            // Hashes differ. This is an UPDATE.
            log_msg("Updating: %s", relative_path_entry);

            // Handle type changes (e.g., file to dir) by deleting old first
            if (me->type_o != me->type_t) {
                if (me->type_o == 'T') {
                    apply_tree_diff(me->hash_o, NULL, full_path); // Recurse-delete
                    rmdir(full_path);
                } else {
                    remove(full_path);
                }
            }
            
            // Now, create the new entry
            if (me->type_t == 'T') {
                if (me->type_o != 'T') { // If it wasn't a T before, mkdir
                    mkdir(full_path, me->mode_t | 0700);
                }
                apply_tree_diff(me->hash_o, me->hash_t, full_path); // Recurse
                chmod(full_path, me->mode_t);
            } else {
                unpack_file_entry(me->hash_t, me->type_t, me->mode_t, full_path);
            }
        }
    }

    free_merge_list(master_list);
    return 0;
}

static void execute_rebuild_job(const char* node_name, const char* node_path, const char* version_tag, const char* old_commit_hash_from_ipc) {
    (void)node_name;
    
    char old_commit_hash[HASH_STR_LEN] = {0};
    char old_tree_hash[HASH_STR_LEN] = {0};
    char new_commit_hash[HASH_STR_LEN] = {0};
    char new_tree_hash[HASH_STR_LEN] = {0};

    // --- 1. Get the CURRENT tree hash (if it exists) ---
    if (old_commit_hash_from_ipc && old_commit_hash_from_ipc[0] != '\0') {
        strncpy(old_commit_hash, old_commit_hash_from_ipc, HASH_STR_LEN - 1);
        log_msg("Using explicit old commit %s as base for rebuild.", old_commit_hash);

        // Now find its tree
        size_t commit_size;
        char* commit_content = read_object(old_commit_hash, &commit_size);
        if (commit_content) {
            char* ptr = strstr(commit_content, "tree ");
            if (ptr) sscanf(ptr, "tree %64s", old_tree_hash);
            free(commit_content);
        }
    } else {
        // --- Fallback (Old, Flawed Logic) ---
        // This is kept so old clients don't break, but it will fail on switch.
        log_msg("Warning: No old commit provided to rebuild. Diff may be incorrect.");
        char active_head_file[PATH_MAX];
        get_active_head_file(node_path, active_head_file, sizeof(active_head_file)); // Gets TARGET head

        if (read_string_from_file(active_head_file, old_commit_hash, sizeof(old_commit_hash)) == 0) {
            size_t commit_size;
            char* commit_content = read_object(old_commit_hash, &commit_size);
            if (commit_content) {
                char* ptr = strstr(commit_content, "tree ");
                if (ptr) sscanf(ptr, "tree %64s", old_tree_hash);
                free(commit_content);
            }
        }
    }
    
    // --- 2. Find the TARGET commit and tree hash ---
    // This part is correct, it uses g_current_subsection (the target)
    if (find_commit_hash_by_tag(node_path, version_tag, new_commit_hash) != 0) {
        if (strcmp(version_tag, "LATEST_HEAD") == 0) {
            log_msg("Subsection '%s' is empty. Rebuilding to an empty state.", g_current_subsection);
            new_commit_hash[0] = '\0'; // Target is "nothing"
        } else {
            log_msg("Error: Failed to find commit for tag '%s' in subsection '%s'", version_tag, g_current_subsection);
            return;
        }
    }
    
    if (new_commit_hash[0] != '\0') {
        size_t commit_size;
        char* commit_content = read_object(new_commit_hash, &commit_size);
        if (!commit_content) {
            log_msg("Error: Failed to read target commit object: %s", new_commit_hash);
            return;
        }
        char* ptr = strstr(commit_content, "tree ");
        if (!ptr || sscanf(ptr, "tree %64s", new_tree_hash) != 1) {
            log_msg("Error: Corrupt commit object '%s'.", new_commit_hash);
            free(commit_content); return;
        }
        free(commit_content);
    }
    
    // This check is now meaningful.
    if (old_tree_hash[0] != '\0' && new_tree_hash[0] != '\0' && 
        strcmp(old_tree_hash, new_tree_hash) == 0) 
    {
        log_msg("Old tree and new tree are identical. Nothing to do.");
        // We still update the HEAD file to point to the new commit
        char active_head_file[PATH_MAX];
        get_active_head_file(node_path, active_head_file, sizeof(active_head_file));
        write_string_to_file(active_head_file, new_commit_hash);
        return;
    }

    log_msg("Applying changes to restore version '%s' (commit %s)...", 
            version_tag, new_commit_hash[0] ? new_commit_hash : "NULL");

    // --- 3. Apply the diff ---
    strncpy(g_node_root_path, node_path, sizeof(g_node_root_path)-1);
    
    apply_tree_diff(old_tree_hash[0] ? old_tree_hash : NULL, 
                    new_tree_hash[0] ? new_tree_hash : NULL, 
                    node_path);
    
    g_node_root_path[0] = '\0';
    
    // --- 4. Update the active HEAD file to point to the new commit ---
    char active_head_file[PATH_MAX];
    get_active_head_file(node_path, active_head_file, sizeof(active_head_file));
    write_string_to_file(active_head_file, new_commit_hash); 
    
    log_msg("Rebuild complete.");
}

static void execute_checkout_job(const char* node_name, const char* node_path, const char* version_tag, const char* file_path) {
    (void)node_name;
    char root_tree_hash[HASH_STR_LEN];
    char object_hash[HASH_STR_LEN];
    mode_t file_mode;
    double file_entropy;
    char object_type = 0; // 'B', 'L', or 'M'

    //Find commit and tree hash
    char commit_hash[HASH_STR_LEN] = {0};
    if (find_commit_hash_by_tag(node_path, version_tag, commit_hash) != 0) {
        log_msg("Error: Could not find version tag '%s' in subsection '%s'.", version_tag, g_current_subsection);
        return;
    }
    size_t commit_size;
    char* commit_content = read_object(commit_hash, &commit_size);
    if (!commit_content) {
        log_msg("Error: Failed to read commit object: %s", commit_hash);
        return;
    }
    char* ptr = strstr(commit_content, "tree ");
    if (!ptr || sscanf(ptr, "tree %64s", root_tree_hash) != 1) {
        log_msg("Error: Corrupt commit object '%s'.", commit_hash);
        free(commit_content); return;
    }
    free(commit_content);


    //use new find_file_in_tree signature
    if (find_file_in_tree(root_tree_hash, file_path, object_hash, &file_mode, &file_entropy, &object_type) != 0) {
        log_msg("Error: File '%s' not found in version '%s'.", file_path, version_tag);
        return;
    }

    //Prepare destination path
    char dest_path[PATH_MAX];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", node_path, file_path);

    char* dir_copy = strdup(dest_path);
    if (dir_copy) {
        char mkdir_cmd[PATH_MAX + 10];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", dirname(dir_copy));
        (void)system(mkdir_cmd);
        free(dir_copy);
    }

    //Handle 'M' type
    if (object_type == 'M') {
        //Reconstruct from Manifest
        log_msg("Restoring manifest: %s", file_path);
        ManifestData* manifest = read_mobj_object(object_hash);
        if (!manifest) {
            log_msg("Error: Failed to read manifest object %s", object_hash);
            return;
        }
        if (reconstruct_file_from_manifest(manifest, dest_path) != 0) {
            log_msg("Error: Failed to reconstruct file from manifest %s", object_hash);
        } else {
             log_msg("Successfully restored '%s' to version '%s'.", file_path, version_tag);
        }
        free_manifest_data(manifest);
        
    } else if (object_type == 'B' || object_type == 'L') {
        // --- Handle Blob or Link (Old logic) ---
        size_t blob_size;
        char* blob_content = read_object(object_hash, &blob_size);
        if (!blob_content) {
            log_msg("Error: Failed to read blob object %s for file %s", object_hash, file_path);
            return;
        }

        if (object_type == 'B') {
            FILE* f = fopen(dest_path, "wb");
            if (!f) {
                log_msg("Error: Failed to open destination file '%s': %s", dest_path, strerror(errno));
                free(blob_content);
                return;
            }
            fwrite(blob_content, 1, blob_size, f);
            fclose(f);
            chmod(dest_path, file_mode);
        } else { // object_type == 'L'
            blob_content[blob_size] = '\0'; // Ensure null-terminated
            symlink(blob_content, dest_path);
        }
        
        free(blob_content);
        log_msg("Successfully restored '%s' to version '%s'.", file_path, version_tag);
    } else {
        log_msg("Error: Unknown object type '%c' found for file '%s'.", object_type, file_path);
    }
}

static void execute_log_job(const char* node_path) {
    char active_head_file[PATH_MAX];
    get_active_head_file(node_path, active_head_file, sizeof(active_head_file));

    char current_commit_hash[HASH_STR_LEN];
    if (read_string_from_file(active_head_file, current_commit_hash, sizeof(current_commit_hash)) != 0 || current_commit_hash[0] == '\0') {
        log_msg("No commits found for subsection '%s'.", g_current_subsection);
        return;
    }

    char head_hash[HASH_STR_LEN];
    strncpy(head_hash, current_commit_hash, HASH_STR_LEN);

    int depth = 0;
    while (current_commit_hash[0] != '\0' && depth < 2000) { // Safety limit
        size_t commit_size;
        char* commit_content = read_object(current_commit_hash, &commit_size);
        if (!commit_content) {
            log_msg("Error: Failed to read commit object %s.", current_commit_hash);
            break;
        }

        // --- Find the commit tag/message first ---
        char commit_tag[PATH_MAX] = "[no message]";
        char* msg_start = strstr(commit_content, "\n\n");
        if (msg_start) {
            msg_start += 2; // Skip \n\n
            char* msg_end = strchr(msg_start, '\n');
            if (msg_end) {
                size_t msg_len = msg_end - msg_start;
                if (msg_len > PATH_MAX - 1) msg_len = PATH_MAX - 1;
                strncpy(commit_tag, msg_start, msg_len);
                commit_tag[msg_len] = '\0';
            } else {
                // No newline at end of message
                strncpy(commit_tag, msg_start, sizeof(commit_tag) - 1);
            }
        }

        // --- Print Formatted Log Entry ---
        if (strcmp(current_commit_hash, head_hash) == 0) {
            log_msg_diff(C_YELLOW "commit %s (HEAD -> %s)" C_RESET, current_commit_hash, g_current_subsection);
        } else {
            log_msg_diff(C_YELLOW "commit %s" C_RESET, current_commit_hash);
        }

        log_msg_diff("Commit Version: %s", commit_tag);

        char* author_line = strstr(commit_content, "author ");
        if (author_line) {
            char* date_start = strstr(author_line, "> ");
            if(date_start) {
                // Print Author: <name> <email>
                log_msg_diff("Author: %.*s>", (int)(date_start - author_line - 7 + 1), author_line + 7);
                
                // Print Date: <timestamp> (<formatted_date>)
                date_start += 2; // Skip "> "
                char* date_end = strstr(date_start, " +");
                if(date_end) *date_end = '\0';
                long long ts = atoll(date_start);
                char time_buf[100];
                strftime(time_buf, sizeof(time_buf), "%a %b %d %T %Y", localtime((time_t*)&ts));
                log_msg_diff("Date:   %lld (%s)", ts, time_buf); 
            }
        }

        log_msg_diff("\n    %s\n", commit_tag);
        //End print

        //Find the *next* parent
        char* parent_line = strstr(commit_content, "parent ");
        if (parent_line) {
            sscanf(parent_line, "parent %64s", current_commit_hash);
        } else {
            current_commit_hash[0] = '\0'; //end of history
        }

        free(commit_content);
        depth++;
    }
}

static void execute_diff_job(const char* node_name, const char* node_path, const char* v1_tag, const char* v2_tag) {
    (void)node_name;
    char tree1_hash[HASH_STR_LEN];
    char tree2_hash[HASH_STR_LEN];
    char v2_committer[256] = "[unknown]";

    //Find commit 1 and tree 1
    char commit1_hash[HASH_STR_LEN] = {0};
    if (find_commit_hash_by_tag(node_path, v1_tag, commit1_hash) != 0) {
        log_msg("Error: Could not find version tag '%s' in subsection '%s'.", v1_tag, g_current_subsection);
        return;
    }
    size_t commit_size;
    char* commit_content = read_object(commit1_hash, &commit_size);
    if (!commit_content) {
        log_msg("Error: Failed to read commit object: %s", commit1_hash);
        return;
    }
    char* ptr = strstr(commit_content, "tree ");
    if (!ptr || sscanf(ptr, "tree %64s", tree1_hash) != 1) {
        log_msg("Error: Corrupt commit object '%s'.", commit1_hash);
        free(commit_content); return;
    }
    free(commit_content);

    
    //Find commit 2 and tree 2
    char commit2_hash[HASH_STR_LEN] = {0};
    if (find_commit_hash_by_tag(node_path, v2_tag, commit2_hash) != 0) {
        log_msg("Error: Could not find version tag '%s' in subsection '%s'.", v2_tag, g_current_subsection);
        return;
    }
    commit_content = read_object(commit2_hash, &commit_size);
    if (!commit_content) {
        log_msg("Error: Failed to read commit object: %s", commit2_hash);
        return;
    }
    ptr = strstr(commit_content, "tree ");
    if (!ptr || sscanf(ptr, "tree %64s", tree2_hash) != 1) {
        log_msg("Error: Corrupt commit object '%s'.", commit2_hash);
        free(commit_content); return;
    }
    // --- END NEW ---
    
    ptr = strstr(commit_content, "\ncommitter ");
    if (!ptr) ptr = strstr(commit_content, "committer ");
    
    if (ptr) {
        if (*ptr == '\n') ptr += 11; else ptr += 10;
        char* angle_bracket = strchr(ptr, '<');
        if (angle_bracket) {
            size_t name_len = angle_bracket - ptr;
            if (name_len > 0 && name_len < sizeof(v2_committer) - 1) {
                while (name_len > 0 && (ptr[name_len - 1] == ' ' || ptr[name_len - 1] == '\t')) {
                    name_len--;
                }
                strncpy(v2_committer, ptr, name_len);
                v2_committer[name_len] = '\0';
            }
        }
    }
    free(commit_content);

    log_msg("Diffing subsection '%s': (%s) ... (%s)", g_current_subsection, v1_tag, v2_tag);
    log_msg_diff("---");
    log_msg_diff("%sCommitter: %s%s", C_CYAN, v2_committer, C_RESET);
    log_msg_diff("%s--- a/%s%s\n%s+++ b/%s%s", C_RED, v1_tag, C_RESET, C_GREEN, v2_tag, C_RESET);
    diff_trees(tree1_hash, tree2_hash, "");
}

int main(int argc, char *argv[]) {
    log_msg("exodus_snapshot starting...");

    CortezIPCData* data_head = cortez_ipc_receive(argc, argv);
    if (!data_head) {
        log_msg("Failed to receive IPC data. Tool must be run by 'exodus' client.");
        return 1;
    }

    const char* command = NULL;
    const char* node_name = NULL;
    const char* node_path = NULL;
    const char* subsection_name = NULL;
    const char* arg1 = NULL;
    const char* arg2 = NULL;
    
    CortezIPCData* current = data_head;
    if (current && current->type == CORTEZ_TYPE_STRING) { command = current->data.string_val; current = current->next; }
    if (current && current->type == CORTEZ_TYPE_STRING) { node_name = current->data.string_val; current = current->next; }
    if (current && current->type == CORTEZ_TYPE_STRING) { node_path = current->data.string_val; current = current->next; }
    if (current && current->type == CORTEZ_TYPE_STRING) { subsection_name = current->data.string_val; current = current->next; }
    if (current && current->type == CORTEZ_TYPE_STRING) { arg1 = current->data.string_val; current = current->next; }
    if (current && current->type == CORTEZ_TYPE_STRING) { arg2 = current->data.string_val; current = current->next; }

    if (!command || !node_name || !node_path || !subsection_name) {
        log_msg("Received malformed IPC data. Missing core arguments.");
        cortez_ipc_free_data(data_head);
        return 1;
    }

    // Set globals
    strncpy(g_current_subsection, subsection_name, sizeof(g_current_subsection) - 1);
    snprintf(g_objects_dir, sizeof(g_objects_dir), "%s/.log/objects", node_path);

    snprintf(g_bblk_objects_dir, sizeof(g_bblk_objects_dir), "%s/.log/objects/b", node_path);
    snprintf(g_mobj_objects_dir, sizeof(g_mobj_objects_dir), "%s/.log/objects/m", node_path);

    // Get user info once for commit/promote
    uid_t user_id = 0;
    char username[128] = "unknown";
    const char* sudo_user = getenv("SUDO_USER");
    if (sudo_user) {
        strncpy(username, sudo_user, sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0';
        const char* sudo_uid_str = getenv("SUDO_UID");
        if (sudo_uid_str) user_id = (uid_t)atol(sudo_uid_str);
    } else {
        user_id = getuid();
        get_username_from_uid_embedded(user_id, username, sizeof(username));
    }


    if (strcmp(command, "add-subs") == 0) {
        if (!arg1) {
            log_msg("Received malformed IPC data for 'add-subs'.");
        } else {
            log_msg("Command: %s, Node: %s, New Sub: %s", command, node_name, arg1);
            execute_add_subs_job(node_path, arg1); // arg1 is new_subsection_name
        }
    } else if (strcmp(command, "promote") == 0) {
        if (!arg1 || !arg2) {
             log_msg("Received malformed IPC data for 'promote'.");
        } else {
            log_msg("Command: %s, Node: %s, Sub: %s, Message: %s, Flag: %s", command, node_name, subsection_name, arg1, arg2);
            execute_promote_job(node_path, subsection_name, arg1, user_id, username, arg2);
        }
    } else if (strcmp(command, "commit") == 0) {
        if (!arg1) {
            log_msg("Received malformed IPC data for 'commit'.");
        } else {
            log_msg("Command: %s, Node: %s, Sub: %s, Tag: %s", command, node_name, g_current_subsection, arg1);
            execute_commit_job(node_name, node_path, arg1, user_id, username); // arg1 is version_tag
        }
    } else if (strcmp(command, "rebuild") == 0) {
        if (!arg1) {
            log_msg("Received malformed IPC data for 'rebuild'.");
        } else {
            log_msg("Command: %s, Node: %s, Sub: %s, Tag: %s", command, node_name, g_current_subsection, arg1);
            execute_rebuild_job(node_name, node_path, arg1, arg2); // arg1 is version_tag
        }
    } else if (strcmp(command, "diff") == 0) {
        if (!arg1 || !arg2) {
            log_msg("Received malformed IPC data for 'diff'.");
        } else {
            log_msg("Command: %s, Node: %s, Sub: %s, v1: %s, v2: %s", command, node_name, g_current_subsection, arg1, arg2);
            execute_diff_job(node_name, node_path, arg1, arg2); // arg1=v1, arg2=v2
        }
    } else if (strcmp(command, "checkout") == 0) {
        if (!arg1 || !arg2) {
            log_msg("Received malformed IPC data for 'checkout'.");
        } else {
            log_msg("Command: %s, Node: %s, Sub: %s, Version: %s, File: %s", command, node_name, g_current_subsection, arg1, arg2);
            execute_checkout_job(node_name, node_path, arg1, arg2); // arg1=version, arg2=file
        }
    }else if (strcmp(command, "log") == 0) {
    log_msg("Command: %s, Node: %s, Sub: %s", command, node_name, g_current_subsection);
    execute_log_job(node_path);
    } else {
        log_msg("Unknown command: %s", command ? command : "NULL");
    }

    cortez_ipc_free_data(data_head);
    log_msg("exodus_snapshot finished.");
    return 0;
}