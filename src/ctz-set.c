/*
 * ctz-set.c
 * Implementation of the .set configuration format handler.
 * COMPILE: 
 * gcc -c ctz-set.c -o ctz-set.o -Wall -O2
 * ar rcs libctzset.a ctz-set.o
 */

#define _GNU_SOURCE
#include "ctz-set.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define MAX_LINE_LEN 2048
#define MAX_VAR_LEN 64

// --- Helper: String Trimming ---
static char* trim_whitespace(char* str) {
    char* end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    *(end+1) = 0;
    return str;
}

// --- Helper: Environment Expansion ---
// Replaces ${VAR} with getenv("VAR")
static char* expand_env_vars(const char* input) {
    if (!input) return NULL;
    if (strchr(input, '$') == NULL) return strdup(input);

    char buffer[MAX_LINE_LEN * 2];
    char* cursor = buffer;
    const char* ptr = input;
    
    while (*ptr && (cursor - buffer < sizeof(buffer) - 1)) {
        if (*ptr == '$' && *(ptr+1) == '{') {
            const char* start = ptr + 2;
            const char* end = strchr(start, '}');
            if (end) {
                char var_name[MAX_VAR_LEN];
                int len = end - start;
                if (len < MAX_VAR_LEN) {
                    strncpy(var_name, start, len);
                    var_name[len] = '\0';
                    char* env_val = getenv(var_name);
                    if (env_val) {
                        int env_len = strlen(env_val);
                        if ((cursor - buffer) + env_len < sizeof(buffer)) {
                            strcpy(cursor, env_val);
                            cursor += env_len;
                        }
                    }
                    ptr = end + 1;
                    continue;
                }
            }
        }
        *cursor++ = *ptr++;
    }
    *cursor = '\0';
    return strdup(buffer);
}

// --- Helper: Memory Management ---
static void free_value(SetValue* v) {
    if (v->type == SET_TYPE_STRING && v->data.s_val) {
        free(v->data.s_val);
    }
}

void set_free(SetConfig* config) {
    if (!config) return;
    if (config->filepath) free(config->filepath);
    
    SetSection* sec = config->sections;
    while (sec) {
        SetPair* pair = sec->pairs;
        while (pair) {
            SetPair* next_pair = pair->next;
            free(pair->key);
            free_value(&pair->value);
            free(pair);
            pair = next_pair;
        }
        SetSection* next_sec = sec->next;
        free(sec->name);
        free(sec);
        sec = next_sec;
    }
    free(config);
}

// --- Core: Parsing Logic ---
static SetValue parse_value(const char* raw_val) {
    SetValue v;
    v.type = SET_TYPE_NULL;

    char* expanded = expand_env_vars(raw_val);
    if (!expanded) return v;

    // 1. Check for Quotes (String)
    if (expanded[0] == '"') {
        size_t len = strlen(expanded);
        if (len > 1 && expanded[len-1] == '"') {
            expanded[len-1] = '\0'; // Remove trailing quote
            v.type = SET_TYPE_STRING;
            v.data.s_val = strdup(expanded + 1); // Skip leading quote
        } else {
            v.type = SET_TYPE_STRING;
            v.data.s_val = strdup(expanded); // Broken quotes, treat as raw string
        }
    } 
    // 2. Check for Booleans
    else if (strcasecmp(expanded, "true") == 0 || strcasecmp(expanded, "on") == 0) {
        v.type = SET_TYPE_BOOL;
        v.data.b_val = 1;
    }
    else if (strcasecmp(expanded, "false") == 0 || strcasecmp(expanded, "off") == 0) {
        v.type = SET_TYPE_BOOL;
        v.data.b_val = 0;
    }
    // 3. Check for Integers (0 and 1 can be bools or ints, handling logic here implies strict digits)
    else {
        char* endptr;
        long val = strtol(expanded, &endptr, 10);
        if (*endptr == '\0') {
            // It's a pure number
            // Optional: Heuristic to treat 0/1 as bool? Let's keep as Int for now.
            v.type = SET_TYPE_INT;
            v.data.i_val = val;
        } else {
            // Fallback to String
            v.type = SET_TYPE_STRING;
            v.data.s_val = strdup(expanded);
        }
    }
    
    free(expanded);
    return v;
}

SetConfig* set_create(const char* filepath) {
    SetConfig* cfg = calloc(1, sizeof(SetConfig));
    if (filepath) cfg->filepath = strdup(filepath);
    return cfg;
}

SetConfig* set_load(const char* filepath) {
    FILE* f = fopen(filepath, "r");
    if (!f) return set_create(filepath); // Return empty config if file missing

    SetConfig* cfg = set_create(filepath);
    SetSection* current_section = NULL;
    // Create default global section
    current_section = calloc(1, sizeof(SetSection));
    current_section->name = strdup("global");
    cfg->sections = current_section;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        char* p = trim_whitespace(line);
        if (*p == 0 || *p == '#' || *p == ';') continue; // Skip empty/comments

        // Section Detection [section]
        if (*p == '[') {
            char* end = strchr(p, ']');
            if (end) {
                *end = '\0';
                // Create new section
                SetSection* new_sec = calloc(1, sizeof(SetSection));
                new_sec->name = strdup(p + 1);
                
                // Append to list
                SetSection* last = cfg->sections;
                while (last->next) last = last->next;
                last->next = new_sec;
                current_section = new_sec;
            }
            continue;
        }

        // Key Value Detection (Key=Value OR Key:Value)
        char* sep = strchr(p, '=');
        if (!sep) sep = strchr(p, ':');

        if (sep) {
            *sep = '\0'; // Split key and value
            char* key = trim_whitespace(p);
            char* val_str = trim_whitespace(sep + 1);

            SetPair* pair = calloc(1, sizeof(SetPair));
            pair->key = strdup(key);
            pair->value = parse_value(val_str);

            // Append to current section
            pair->next = current_section->pairs;
            current_section->pairs = pair; // Prepend is faster, order doesn't usually matter
        }
    }
    fclose(f);
    return cfg;
}

// --- Core: Getters ---
static SetPair* find_pair(SetConfig* config, const char* section, const char* key) {
    SetSection* sec = config->sections;
    while (sec) {
        // Handle NULL section request as "global" or just find first match?
        const char* target_sec = section ? section : "global";
        if (strcmp(sec->name, target_sec) == 0) {
            SetPair* pair = sec->pairs;
            while (pair) {
                if (strcmp(pair->key, key) == 0) return pair;
                pair = pair->next;
            }
            return NULL;
        }
        sec = sec->next;
    }
    return NULL;
}

const char* set_get_string(SetConfig* config, const char* section, const char* key, const char* default_val) {
    SetPair* p = find_pair(config, section, key);
    if (!p) return default_val;
    
    if (p->value.type == SET_TYPE_STRING) return p->value.data.s_val;
    if (p->value.type == SET_TYPE_INT) {
        static char buf[32]; // Not thread safe, but simple
        snprintf(buf, 32, "%ld", p->value.data.i_val);
        return buf;
    }
    if (p->value.type == SET_TYPE_BOOL) return p->value.data.b_val ? "true" : "false";
    return default_val;
}

long set_get_int(SetConfig* config, const char* section, const char* key, long default_val) {
    SetPair* p = find_pair(config, section, key);
    if (!p) return default_val;
    if (p->value.type == SET_TYPE_INT) return p->value.data.i_val;
    if (p->value.type == SET_TYPE_STRING) return atol(p->value.data.s_val);
    return default_val;
}

int set_get_bool(SetConfig* config, const char* section, const char* key, int default_val) {
    SetPair* p = find_pair(config, section, key);
    if (!p) return default_val;
    if (p->value.type == SET_TYPE_BOOL) return p->value.data.b_val;
    if (p->value.type == SET_TYPE_INT) return (p->value.data.i_val != 0);
    return default_val;
}

// --- Core: Setters ---
static SetPair* get_or_create_pair(SetConfig* config, const char* section, const char* key) {
    const char* target_sec = section ? section : "global";
    SetSection* sec = config->sections;
    SetSection* prev_sec = NULL;
    
    // Find section
    while (sec) {
        if (strcmp(sec->name, target_sec) == 0) break;
        prev_sec = sec;
        sec = sec->next;
    }
    
    // Create section if missing
    if (!sec) {
        sec = calloc(1, sizeof(SetSection));
        sec->name = strdup(target_sec);
        if (prev_sec) prev_sec->next = sec;
        else config->sections = sec;
    }

    // Find key
    SetPair* pair = sec->pairs;
    while (pair) {
        if (strcmp(pair->key, key) == 0) return pair;
        pair = pair->next;
    }

    // Create key
    pair = calloc(1, sizeof(SetPair));
    pair->key = strdup(key);
    pair->next = sec->pairs;
    sec->pairs = pair;
    return pair;
}

void set_set_string(SetConfig* config, const char* section, const char* key, const char* value) {
    SetPair* p = get_or_create_pair(config, section, key);
    free_value(&p->value);
    p->value.type = SET_TYPE_STRING;
    p->value.data.s_val = strdup(value);
}

void set_set_int(SetConfig* config, const char* section, const char* key, long value) {
    SetPair* p = get_or_create_pair(config, section, key);
    free_value(&p->value);
    p->value.type = SET_TYPE_INT;
    p->value.data.i_val = value;
}

void set_set_bool(SetConfig* config, const char* section, const char* key, int value) {
    SetPair* p = get_or_create_pair(config, section, key);
    free_value(&p->value);
    p->value.type = SET_TYPE_BOOL;
    p->value.data.b_val = value;
}

// --- Core: Saving ---
int set_save(SetConfig* config) {
    if (!config->filepath) return -1;
    FILE* f = fopen(config->filepath, "w");
    if (!f) return -1;

    SetSection* sec = config->sections;
    while (sec) {
        // Don't write [global] header if it's the implicit one, unless you want to
        if (strcmp(sec->name, "global") != 0) {
            fprintf(f, "\n[%s]\n", sec->name);
        }

        SetPair* p = sec->pairs;
        while (p) {
            if (p->value.type == SET_TYPE_STRING) {
                // Check if needs quotes (contains spaces)
                if (strchr(p->value.data.s_val, ' ')) {
                    fprintf(f, "%s = \"%s\"\n", p->key, p->value.data.s_val);
                } else {
                    fprintf(f, "%s = %s\n", p->key, p->value.data.s_val);
                }
            } else if (p->value.type == SET_TYPE_INT) {
                fprintf(f, "%s = %ld\n", p->key, p->value.data.i_val);
            } else if (p->value.type == SET_TYPE_BOOL) {
                fprintf(f, "%s = %s\n", p->key, p->value.data.b_val ? "True" : "False");
            }
            p = p->next;
        }
        sec = sec->next;
    }
    fclose(f);
    return 0;
}