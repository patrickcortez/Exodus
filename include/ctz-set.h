/*
 * ctz-set.h
 * A robust configuration handler for .set files.
 * Supports: Sections, Key=Value, Key:Value, Env Expansion, Types (Int, Bool, String).
 */

#ifndef CTZ_SET_H
#define CTZ_SET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SET_TYPE_NULL,
    SET_TYPE_STRING,
    SET_TYPE_INT,
    SET_TYPE_BOOL
} SetType;

typedef struct SetValue {
    SetType type;
    union {
        char* s_val;
        long i_val;
        int b_val; // 0 or 1
    } data;
} SetValue;

typedef struct SetPair {
    char* key;
    SetValue value;
    struct SetPair* next;
} SetPair;

typedef struct SetSection {
    char* name;
    SetPair* pairs;
    struct SetSection* next;
} SetSection;

typedef struct SetConfig {
    SetSection* sections;
    char* filepath;
} SetConfig;

// --- Core API ---

// Load and parse a .set file (allocates memory)
SetConfig* set_load(const char* filepath);

// Create a new empty config
SetConfig* set_create(const char* filepath);

// Save the config back to disk
int set_save(SetConfig* config);

// Free all memory
void set_free(SetConfig* config);

// --- Getters ---
// Returns the string value. If it was an INT/BOOL, it returns string representation.
const char* set_get_string(SetConfig* config, const char* section, const char* key, const char* default_val);
long set_get_int(SetConfig* config, const char* section, const char* key, long default_val);
int set_get_bool(SetConfig* config, const char* section, const char* key, int default_val);

// --- Setters ---
// Updates value if exists, creates if not. Handles Section creation automatically.
void set_set_string(SetConfig* config, const char* section, const char* key, const char* value);
void set_set_int(SetConfig* config, const char* section, const char* key, long value);
void set_set_bool(SetConfig* config, const char* section, const char* key, int value);

#ifdef __cplusplus
}
#endif

#endif // CTZ_SET_H