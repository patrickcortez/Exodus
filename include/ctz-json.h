/*
 * ctz-json.h - Ctz-JSON Single-Header Library
 *
 * This library provides a robust, compliant, and easy-to-use JSON parser.
 *
 * SOURCES:
 * - RFC 8259: The JavaScript Object Notation (JSON) Data Interchange Format.
 * (https://www.rfc-editor.org/rfc/rfc8259.html)
 * - Principles of Compiler Design, Alfred V. Aho & Jeffrey D. Ullman.
 * (Concepts for recursive descent parsing).
 */

#ifndef CTZ_JSON_H
#define CTZ_JSON_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CTZ_JSON_NULL,
    CTZ_JSON_FALSE,
    CTZ_JSON_TRUE,
    CTZ_JSON_NUMBER,
    CTZ_JSON_STRING,
    CTZ_JSON_ARRAY,
    CTZ_JSON_OBJECT
} ctz_json_type;

typedef struct ctz_json_value ctz_json_value;
typedef struct ctz_json_member ctz_json_member;

struct ctz_json_value {
    union {
        double number;
        struct { char* s; size_t len; } string;
        struct { ctz_json_value** e; size_t size; size_t capacity; } array;
        struct { ctz_json_member* m; size_t size; } object;
    } u;
    ctz_json_type type;
};

struct ctz_json_member {
    char* k;
    size_t klen;
    ctz_json_value* v;
};

ctz_json_value* ctz_json_parse(const char* json, char* error_buffer, size_t error_buffer_size);

void ctz_json_free(ctz_json_value* value);

ctz_json_type ctz_json_get_type(const ctz_json_value* value);

double ctz_json_get_number(const ctz_json_value* value);

const char* ctz_json_get_string(const ctz_json_value* value);
size_t ctz_json_get_string_length(const ctz_json_value* value);

size_t ctz_json_get_array_size(const ctz_json_value* value);
ctz_json_value* ctz_json_get_array_element(const ctz_json_value* value, size_t index);

size_t ctz_json_get_object_size(const ctz_json_value* value);
const char* ctz_json_get_object_key(const ctz_json_value* value, size_t index);
size_t ctz_json_get_object_key_length(const ctz_json_value* value, size_t index);
ctz_json_value* ctz_json_get_object_value(const ctz_json_value* value, size_t index);
ctz_json_value* ctz_json_find_object_value(const ctz_json_value* value, const char* key);

ctz_json_value* ctz_json_load_file(const char* filepath, char* error_buffer, size_t error_buffer_size);


char* ctz_json_stringify(const ctz_json_value* value, int pretty);

int ctz_json_object_set_value(ctz_json_value* object, const char* key, ctz_json_value* value_to_add);

int ctz_json_object_remove_value(ctz_json_value* object, const char* key);

ctz_json_value* ctz_json_new_null(void);
ctz_json_value* ctz_json_new_bool(int b);
ctz_json_value* ctz_json_new_number(double n);
ctz_json_value* ctz_json_new_string(const char* s);
ctz_json_value* ctz_json_new_array(void);
ctz_json_value* ctz_json_new_object(void);
int ctz_json_array_push_value(ctz_json_value* array, ctz_json_value* value_to_push);

#ifdef __cplusplus
}
#endif

#endif /* CTZ_JSON_H */
