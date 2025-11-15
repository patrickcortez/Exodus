/*
 * COMPILE TO STATIC LIBRARY:
 * gcc -c ctz-json.c -o ctz-json.o -O2 -Wall
 * ar rcs libctzjson.a ctz-json.o
 */

#include "ctz-json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>


#define CTZ_CONTEXT_INIT_ERROR_BUFFER(ctx, buffer, size) do { (ctx)->error_buffer = buffer; (ctx)->error_buffer_size = size; if (size > 0) buffer[0] = '\0'; } while(0)
#define CTZ_SET_ERROR(ctx, ...) do { if ((ctx)->error_buffer) snprintf((ctx)->error_buffer, (ctx)->error_buffer_size, __VA_ARGS__); } while(0)
#define CTZ_EXPECT(ctx, ch) do { assert(*(ctx)->json == (ch)); (ctx)->json++; } while(0)

//Forward Declerations
int ctz_json_compare(const ctz_json_value* a, const ctz_json_value* b);
ctz_json_value* ctz_json_duplicate(const ctz_json_value* value, int deep);


typedef struct {
    const char* json;
    char* error_buffer;
    size_t error_buffer_size;
} ctz_context;

static void ctz_parse_whitespace(ctz_context* c) {
    const char *p = c->json;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    c->json = p;
}

static ctz_json_value* ctz_parse_value(ctz_context* c);

static ctz_json_value* ctz_new_value(ctz_json_type type) {
    ctz_json_value* v = (ctz_json_value*)malloc(sizeof(ctz_json_value));
    if (!v) return NULL;
    v->type = type;
    return v;
}

static ctz_json_value* ctz_parse_literal(ctz_context* c, const char* literal, ctz_json_type type) {
    size_t len = strlen(literal);
    if (strncmp(c->json, literal, len) == 0) {
        c->json += len;
        return ctz_new_value(type);
    }
    CTZ_SET_ERROR(c, "Invalid literal");
    return NULL;
}

static ctz_json_value* ctz_parse_number(ctz_context* c) {
    const char* p = c->json;
    if (*p == '-') p++;
    if (*p == '0') {
        p++;
        if (*p >= '1' && *p <= '9') {
            CTZ_SET_ERROR(c, "Invalid number format: leading zero");
            return NULL;
        }
    } else if (*p >= '1' && *p <= '9') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    } else {
        CTZ_SET_ERROR(c, "Invalid number format");
        return NULL;
    }
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) {
            CTZ_SET_ERROR(c, "Invalid number format: digit expected after '.'");
            return NULL;
        }
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) {
            CTZ_SET_ERROR(c, "Invalid number format: digit expected after 'e'/'E'");
            return NULL;
        }
        while (isdigit((unsigned char)*p)) p++;
    }

    errno = 0;
    char* end;
    double val = strtod(c->json, &end);
    if (errno == ERANGE && (val == HUGE_VAL || val == -HUGE_VAL)) {
        CTZ_SET_ERROR(c, "Number out of range");
        return NULL;
    }
    if (p != end) {
        CTZ_SET_ERROR(c, "Invalid number format");
        return NULL;
    }

    c->json = p;
    ctz_json_value* v = ctz_new_value(CTZ_JSON_NUMBER);
    if (!v) return NULL;
    v->u.number = val;
    return v;
}

static const char* ctz_parse_hex4(const char *p, unsigned* u) {
    *u = 0;
    for (int i = 0; i < 4; i++) {
        char ch = *p++;
        *u <<= 4;
        if      (ch >= '0' && ch <= '9') *u |= ch - '0';
        else if (ch >= 'a' && ch <= 'f') *u |= ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') *u |= ch - 'A' + 10;
        else return NULL;
    }
    return p;
}

static void ctz_encode_utf8(char** buffer, unsigned u) {
    if (u <= 0x7F) {
        **buffer = u & 0xFF;
        (*buffer)++;
    } else if (u <= 0x7FF) {
        **buffer = 0xC0 | ((u >> 6) & 0xFF); (*buffer)++;
        **buffer = 0x80 | ( u       & 0x3F); (*buffer)++;
    } else if (u <= 0xFFFF) {
        **buffer = 0xE0 | ((u >> 12) & 0xFF); (*buffer)++;
        **buffer = 0x80 | ((u >> 6)  & 0x3F); (*buffer)++;
        **buffer = 0x80 | ( u        & 0x3F); (*buffer)++;
    } else {
        assert(u <= 0x10FFFF);
        **buffer = 0xF0 | ((u >> 18) & 0xFF); (*buffer)++;
        **buffer = 0x80 | ((u >> 12) & 0x3F); (*buffer)++;
        **buffer = 0x80 | ((u >> 6)  & 0x3F); (*buffer)++;
        **buffer = 0x80 | ( u        & 0x3F); (*buffer)++;
    }
}

static ctz_json_value* ctz_parse_string_raw(ctz_context* c, char** str, size_t* len) {
    const char* p = c->json;
    size_t head = 0;
    unsigned u, u2;
    *len = 0;
    *str = NULL;

    while (*p != '"' && *p != '\0') {
        if ((unsigned char)*p < 0x20) {
            CTZ_SET_ERROR(c, "Invalid character in string");
            free(*str);
            return NULL;
        }
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"': case '\\': case '/': case 'b':
                case 'f': case 'n': case 'r': case 't':
                    head++;
                    p++;
                    break;
                case 'u':
                    p++;
                    if (!(p = ctz_parse_hex4(p, &u))) {
                         CTZ_SET_ERROR(c, "Invalid unicode hex");
                         free(*str);
                         return NULL;
                    }
                    if (u >= 0xD800 && u <= 0xDBFF) {
                        if (*p++ != '\\' || *p++ != 'u' || !(p = ctz_parse_hex4(p, &u2)) || u2 < 0xDC00 || u2 > 0xDFFF) {
                            CTZ_SET_ERROR(c, "Invalid unicode surrogate pair");
                            free(*str);
                            return NULL;
                        }
                        u = (((u - 0xD800) << 10) | (u2 - 0xDC00)) + 0x10000;
                    }
                    if (u <= 0x7F) head += 1;
                    else if (u <= 0x7FF) head += 2;
                    else if (u <= 0xFFFF) head += 3;
                    else head += 4;
                    break;
                default:
                    CTZ_SET_ERROR(c, "Invalid escape character");
                    free(*str);
                    return NULL;
            }
        } else {
            head++;
            p++;
        }
    }

    if (*p != '"') {
        CTZ_SET_ERROR(c, "Missing closing quote");
        free(*str);
        return NULL;
    }

    *len = head;
    *str = (char*)malloc(head + 1);
    if (!(*str)) {
        CTZ_SET_ERROR(c, "Memory allocation failure");
        return NULL;
    }
    char* q = *str;
    p = c->json;
    while (*p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"': *q++ = '"'; break;
                case '\\': *q++ = '\\'; break;
                case '/': *q++ = '/'; break;
                case 'b': *q++ = '\b'; break;
                case 'f': *q++ = '\f'; break;
                case 'n': *q++ = '\n'; break;
                case 'r': *q++ = '\r'; break;
                case 't': *q++ = '\t'; break;
                case 'u':
                    p++;
                    p = ctz_parse_hex4(p, &u) - 1;
                    if (u >= 0xD800 && u <= 0xDBFF) {
                        p += 3;
                        p = ctz_parse_hex4(p, &u2) - 1;
                        u = (((u - 0xD800) << 10) | (u2 - 0xDC00)) + 0x10000;
                    }
                    ctz_encode_utf8(&q, u);
                    break;
            }
            p++;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
    c->json = p + 1;
    return ctz_new_value(CTZ_JSON_STRING);
}


ctz_json_value* ctz_json_new_null(void) {
    return ctz_new_value(CTZ_JSON_NULL);
}

ctz_json_value* ctz_json_new_bool(int b) {
    return ctz_new_value(b ? CTZ_JSON_TRUE : CTZ_JSON_FALSE);
}

ctz_json_value* ctz_json_new_number(double n) {
    ctz_json_value* v = ctz_new_value(CTZ_JSON_NUMBER);
    if (v) v->u.number = n;
    return v;
}

ctz_json_value* ctz_json_new_string(const char* s) {
    if (!s) return NULL;
    ctz_json_value* v = ctz_new_value(CTZ_JSON_STRING);
    if (v) {
        v->u.string.len = strlen(s);
        v->u.string.s = (char*)malloc(v->u.string.len + 1);
        if (v->u.string.s) {
            memcpy(v->u.string.s, s, v->u.string.len + 1);
        } else {
            free(v);
            return NULL;
        }
    }
    return v;
}

ctz_json_value* ctz_json_new_array(void) {
    ctz_json_value* v = ctz_new_value(CTZ_JSON_ARRAY);
    if (v) {
        v->u.array.size = 0;
        v->u.array.capacity = 0;
        v->u.array.e = NULL;
    }
    return v;
}

ctz_json_value* ctz_json_new_object(void) {
    ctz_json_value* v = ctz_new_value(CTZ_JSON_OBJECT);
    if (v) {
        v->u.object.size = 0;
        v->u.object.m = NULL;
    }
    return v;
}


int ctz_json_array_push_value(ctz_json_value* array, ctz_json_value* value_to_push) {
    if (!array || array->type != CTZ_JSON_ARRAY || !value_to_push) {
        return -1;
    }

    
    if (array->u.array.size >= array->u.array.capacity) {
        
        size_t new_capacity = array->u.array.capacity == 0 ? 8 : array->u.array.capacity * 2;
        
        
        ctz_json_value** new_e = (ctz_json_value**)realloc(array->u.array.e, new_capacity * sizeof(ctz_json_value*));
        if (!new_e) {
            
            return -1; 
        }
        
        array->u.array.e = new_e;
        array->u.array.capacity = new_capacity;
    }
    
    
    array->u.array.e[array->u.array.size++] = value_to_push;
    
    return 0; 
}


// --- Implementation of File Loader ---

ctz_json_value* ctz_json_load_file(const char* filepath, char* error_buffer, size_t error_buffer_size) {
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        if (error_buffer) snprintf(error_buffer, error_buffer_size, "Failed to open file '%s'", filepath);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(length + 1);
    if (!buffer) {
        fclose(f);
        if (error_buffer) snprintf(error_buffer, error_buffer_size, "Memory allocation failed for file buffer");
        return NULL;
    }

    if (fread(buffer, 1, length, f) != (size_t)length) {
        fclose(f);
        free(buffer);
        if (error_buffer) snprintf(error_buffer, error_buffer_size, "Failed to read file '%s'", filepath);
        return NULL;
    }
    buffer[length] = '\0';
    fclose(f);

    ctz_json_value* val = ctz_json_parse(buffer, error_buffer, error_buffer_size);
    free(buffer);
    return val;
}

// --- Implementation of Stringify ---

typedef struct {
    char* buffer;
    size_t size;
    size_t capacity;
} ctz_strbuf;

static void strbuf_init(ctz_strbuf* sb) {
    sb->buffer = (char*)malloc(64);
    sb->size = 0;
    sb->capacity = 64;
    if (sb->buffer) sb->buffer[0] = '\0';
}

static void strbuf_free(ctz_strbuf* sb) {
    free(sb->buffer);
}

static void strbuf_append(ctz_strbuf* sb, const char* str, size_t len) {
    if (sb->size + len + 1 > sb->capacity) {
        while (sb->size + len + 1 > sb->capacity) {
            sb->capacity = sb->capacity == 0 ? 64 : sb->capacity * 2;
        }
        sb->buffer = (char*)realloc(sb->buffer, sb->capacity);
    }
    memcpy(sb->buffer + sb->size, str, len);
    sb->size += len;
    sb->buffer[sb->size] = '\0';
}

static void ctz_stringify_value(const ctz_json_value* v, ctz_strbuf* sb, int pretty, int indent);

static void ctz_stringify_string(const char* s, size_t len, ctz_strbuf* sb) {
    strbuf_append(sb, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '\"': strbuf_append(sb, "\\\"", 2); break;
            case '\\': strbuf_append(sb, "\\\\", 2); break;
            case '\b': strbuf_append(sb, "\\b", 2); break;
            case '\f': strbuf_append(sb, "\\f", 2); break;
            case '\n': strbuf_append(sb, "\\n", 2); break;
            case '\r': strbuf_append(sb, "\\r", 2); break;
            case '\t': strbuf_append(sb, "\\t", 2); break;
            default:
                if (ch < 0x20) {
                    char hex[7];
                    sprintf(hex, "\\u%04X", ch);
                    strbuf_append(sb, hex, 6);
                } else {
                    strbuf_append(sb, (const char*)&ch, 1);
                }
                break;
        }
    }
    strbuf_append(sb, "\"", 1);
}

static void ctz_stringify_value(const ctz_json_value* v, ctz_strbuf* sb, int pretty, int indent) {
    char buffer[128];
    switch (v->type) {
        case CTZ_JSON_NULL:   strbuf_append(sb, "null", 4); break;
        case CTZ_JSON_TRUE:   strbuf_append(sb, "true", 4); break;
        case CTZ_JSON_FALSE:  strbuf_append(sb, "false", 5); break;
        case CTZ_JSON_NUMBER:
            sprintf(buffer, "%.17g", v->u.number);
            strbuf_append(sb, buffer, strlen(buffer));
            break;
        case CTZ_JSON_STRING:
            ctz_stringify_string(v->u.string.s, v->u.string.len, sb);
            break;
        case CTZ_JSON_ARRAY:
            strbuf_append(sb, "[", 1);
            if (pretty && v->u.array.size > 0) strbuf_append(sb, "\n", 1);
            for (size_t i = 0; i < v->u.array.size; i++) {
                if (pretty) sprintf(buffer, "%*s", (indent + 1) * 2, "");
                else buffer[0] = '\0';
                strbuf_append(sb, buffer, strlen(buffer));

                ctz_stringify_value(v->u.array.e[i], sb, pretty, indent + 1);
                
                if (i < v->u.array.size - 1) {
                    strbuf_append(sb, ",", 1);
                }
                if (pretty) strbuf_append(sb, "\n", 1);
            }
            if (pretty && v->u.array.size > 0) {
                 sprintf(buffer, "%*s", indent * 2, "");
                 strbuf_append(sb, buffer, strlen(buffer));
            }
            strbuf_append(sb, "]", 1);
            break;
        case CTZ_JSON_OBJECT:
            strbuf_append(sb, "{", 1);
            if (pretty && v->u.object.size > 0) strbuf_append(sb, "\n", 1);
            for (size_t i = 0; i < v->u.object.size; i++) {
                if (pretty) sprintf(buffer, "%*s", (indent + 1) * 2, "");
                else buffer[0] = '\0';
                strbuf_append(sb, buffer, strlen(buffer));

                ctz_stringify_string(v->u.object.m[i].k, v->u.object.m[i].klen, sb);
                strbuf_append(sb, pretty ? ": " : ":", pretty ? 2 : 1);
                ctz_stringify_value(v->u.object.m[i].v, sb, pretty, indent + 1);

                if (i < v->u.object.size - 1) {
                    strbuf_append(sb, ",", 1);
                }
                if (pretty) strbuf_append(sb, "\n", 1);
            }
            if (pretty && v->u.object.size > 0) {
                 sprintf(buffer, "%*s", indent * 2, "");
                 strbuf_append(sb, buffer, strlen(buffer));
            }
            strbuf_append(sb, "}", 1);
            break;
    }
}

char* ctz_json_stringify(const ctz_json_value* value, int pretty) {
    if (!value) return NULL;
    ctz_strbuf sb;
    strbuf_init(&sb);
    if (!sb.buffer) return NULL;
    ctz_stringify_value(value, &sb, pretty, 0);
    return sb.buffer;
}


// --- Implementation of Object Manipulation ---

int ctz_json_object_set_value(ctz_json_value* object, const char* key, ctz_json_value* value_to_add) {
    if (!object || object->type != CTZ_JSON_OBJECT || !key || !value_to_add) return -1;
    
    size_t key_len = strlen(key);
    // First, check if key already exists to replace it
    for (size_t i = 0; i < object->u.object.size; i++) {
        if (object->u.object.m[i].klen == key_len && memcmp(object->u.object.m[i].k, key, key_len) == 0) {
            ctz_json_free(object->u.object.m[i].v); // Free the old value
            object->u.object.m[i].v = value_to_add; // Assign the new one
            return 0;
        }
    }
    
    // If not found, add a new member
    ctz_json_member* new_m = (ctz_json_member*)realloc(object->u.object.m, (object->u.object.size + 1) * sizeof(ctz_json_member));
    if (!new_m) return -1;

    object->u.object.m = new_m;
    ctz_json_member* member = &object->u.object.m[object->u.object.size];
    
    member->klen = key_len;
    member->k = (char*)malloc(key_len + 1);
    if (!member->k) return -1;
    memcpy(member->k, key, key_len + 1);
    
    member->v = value_to_add;
    object->u.object.size++;
    
    return 0;
}

int ctz_json_object_remove_value(ctz_json_value* object, const char* key) {
    if (!object || object->type != CTZ_JSON_OBJECT || !key) return -1;

    size_t key_len = strlen(key);
    size_t i = 0;
    for (i = 0; i < object->u.object.size; i++) {
        if (object->u.object.m[i].klen == key_len && memcmp(object->u.object.m[i].k, key, key_len) == 0) {
            break; // Found at index i
        }
    }

    if (i == object->u.object.size) {
        return -1; // Not found
    }

    // Free the member's resources
    free(object->u.object.m[i].k);
    ctz_json_free(object->u.object.m[i].v);

    // Shift remaining elements left
    size_t num_to_move = object->u.object.size - 1 - i;
    if (num_to_move > 0) {
        memmove(&object->u.object.m[i], &object->u.object.m[i + 1], num_to_move * sizeof(ctz_json_member));
    }
    
    object->u.object.size--;
    
    if (object->u.object.size == 0) {
        free(object->u.object.m);
        object->u.object.m = NULL;
    } else {
        // Optional: shrink the allocation
        ctz_json_member* new_m = (ctz_json_member*)realloc(object->u.object.m, object->u.object.size * sizeof(ctz_json_member));
        if (new_m) object->u.object.m = new_m;
    }

    return 0;
}


static ctz_json_value* ctz_parse_string(ctz_context* c) {
    CTZ_EXPECT(c, '"');
    char* s;
    size_t len;
    ctz_json_value* v = ctz_parse_string_raw(c, &s, &len);
    if (v) {
        v->u.string.s = s;
        v->u.string.len = len;
    }
    return v;
}

static ctz_json_value* ctz_parse_array(ctz_context* c) {
    CTZ_EXPECT(c, '[');
    ctz_parse_whitespace(c);

    ctz_json_value* v = ctz_new_value(CTZ_JSON_ARRAY);
    if (!v) return NULL;
    v->u.array.size = 0;
    v->u.array.e = NULL;
    v->u.array.capacity = 0;

    if (*c->json == ']') {
        c->json++;
        return v;
    }

    size_t capacity = 0;
    for (;;) {
        if (v->u.array.size >= capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2;
            ctz_json_value** new_e = (ctz_json_value**)realloc(v->u.array.e, capacity * sizeof(ctz_json_value*));
            if (!new_e) {
                ctz_json_free(v);
                CTZ_SET_ERROR(c, "Memory allocation failure");
                return NULL;
            }
            v->u.array.e = new_e;
            v->u.array.capacity = capacity;
        }

        ctz_json_value* element = ctz_parse_value(c);
        if (!element) {
            ctz_json_free(v);
            return NULL;
        }
        v->u.array.e[v->u.array.size++] = element;

        ctz_parse_whitespace(c);
        if (*c->json == ',') {
            c->json++;
            ctz_parse_whitespace(c);
        } else if (*c->json == ']') {
            c->json++;
            return v;
        } else {
            ctz_json_free(v);
            CTZ_SET_ERROR(c, "Invalid array format");
            return NULL;
        }
    }
}

static ctz_json_value* ctz_parse_object(ctz_context* c) {
    CTZ_EXPECT(c, '{');
    ctz_parse_whitespace(c);

    ctz_json_value* v = ctz_new_value(CTZ_JSON_OBJECT);
    if (!v) return NULL;
    v->u.object.size = 0;
    v->u.object.m = NULL;

    if (*c->json == '}') {
        c->json++;
        return v;
    }

    size_t capacity = 0;
    for (;;) {
        if (v->u.object.size >= capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2;
            ctz_json_member* new_m = (ctz_json_member*)realloc(v->u.object.m, capacity * sizeof(ctz_json_member));
            if (!new_m) {
                ctz_json_free(v);
                CTZ_SET_ERROR(c, "Memory allocation failure");
                return NULL;
            }
            v->u.object.m = new_m;
        }

        if (*c->json != '"') {
            ctz_json_free(v);
            CTZ_SET_ERROR(c, "Object key must be a string");
            return NULL;
        }

        ctz_json_member* member = &v->u.object.m[v->u.object.size];
        CTZ_EXPECT(c, '"');
        if (!ctz_parse_string_raw(c, &member->k, &member->klen)) {
            ctz_json_free(v);
            return NULL;
        }

        ctz_parse_whitespace(c);
        if (*c->json != ':') {
            free(member->k);
            ctz_json_free(v);
            CTZ_SET_ERROR(c, "Missing colon after object key");
            return NULL;
        }
        c->json++;
        ctz_parse_whitespace(c);

        member->v = ctz_parse_value(c);
        if (!member->v) {
            free(member->k);
            ctz_json_free(v);
            return NULL;
        }
        v->u.object.size++;

        ctz_parse_whitespace(c);
        if (*c->json == ',') {
            c->json++;
            ctz_parse_whitespace(c);
        } else if (*c->json == '}') {
            c->json++;
            return v;
        } else {
            ctz_json_free(v);
            CTZ_SET_ERROR(c, "Invalid object format");
            return NULL;
        }
    }
}

static ctz_json_value* ctz_parse_value(ctz_context* c) {
    ctz_parse_whitespace(c);
    switch (*c->json) {
        case 'n':  return ctz_parse_literal(c, "null", CTZ_JSON_NULL);
        case 't':  return ctz_parse_literal(c, "true", CTZ_JSON_TRUE);
        case 'f':  return ctz_parse_literal(c, "false", CTZ_JSON_FALSE);
        case '"':  return ctz_parse_string(c);
        case '[':  return ctz_parse_array(c);
        case '{':  return ctz_parse_object(c);
        case '\0': CTZ_SET_ERROR(c, "Unexpected end of input"); return NULL;
        default:   return ctz_parse_number(c);
    }
}

ctz_json_value* ctz_json_parse(const char* json, char* error_buffer, size_t error_buffer_size) {
    ctz_context c;
    c.json = json;
    CTZ_CONTEXT_INIT_ERROR_BUFFER(&c, error_buffer, error_buffer_size);
    ctz_json_value* value = ctz_parse_value(&c);
    if (value) {
        ctz_parse_whitespace(&c);
        if (*c.json != '\0') {
            ctz_json_free(value);
            CTZ_SET_ERROR(&c, "Unexpected characters at end of input");
            return NULL;
        }
    }
    return value;
}

void ctz_json_free(ctz_json_value* value) {
    if (!value) return;
    switch (value->type) {
        case CTZ_JSON_STRING:
            free(value->u.string.s);
            break;
        case CTZ_JSON_ARRAY:
            for (size_t i = 0; i < value->u.array.size; i++)
                ctz_json_free(value->u.array.e[i]);
            free(value->u.array.e);
            break;
        case CTZ_JSON_OBJECT:
            for (size_t i = 0; i < value->u.object.size; i++) {
                free(value->u.object.m[i].k);
                ctz_json_free(value->u.object.m[i].v);
            }
            free(value->u.object.m);
            break;
        default:
            break;
    }
    free(value);
}

ctz_json_type ctz_json_get_type(const ctz_json_value* value) {
    return value ? value->type : CTZ_JSON_NULL;
}

double ctz_json_get_number(const ctz_json_value* value) {
    return (value && value->type == CTZ_JSON_NUMBER) ? value->u.number : 0.0;
}

const char* ctz_json_get_string(const ctz_json_value* value) {
    return (value && value->type == CTZ_JSON_STRING) ? value->u.string.s : "";
}

size_t ctz_json_get_string_length(const ctz_json_value* value) {
    return (value && value->type == CTZ_JSON_STRING) ? value->u.string.len : 0;
}

size_t ctz_json_get_array_size(const ctz_json_value* value) {
    return (value && value->type == CTZ_JSON_ARRAY) ? value->u.array.size : 0;
}

ctz_json_value* ctz_json_get_array_element(const ctz_json_value* value, size_t index) {
    if (value && value->type == CTZ_JSON_ARRAY && index < value->u.array.size)
        return value->u.array.e[index];
    return NULL;
}

size_t ctz_json_get_object_size(const ctz_json_value* value) {
    return (value && value->type == CTZ_JSON_OBJECT) ? value->u.object.size : 0;
}

const char* ctz_json_get_object_key(const ctz_json_value* value, size_t index) {
    if (value && value->type == CTZ_JSON_OBJECT && index < value->u.object.size)
        return value->u.object.m[index].k;
    return NULL;
}

size_t ctz_json_get_object_key_length(const ctz_json_value* value, size_t index) {
    if (value && value->type == CTZ_JSON_OBJECT && index < value->u.object.size)
        return value->u.object.m[index].klen;
    return 0;
}

ctz_json_value* ctz_json_get_object_value(const ctz_json_value* value, size_t index) {
    if (value && value->type == CTZ_JSON_OBJECT && index < value->u.object.size)
        return value->u.object.m[index].v;
    return NULL;
}

ctz_json_value* ctz_json_find_object_value(const ctz_json_value* value, const char* key) {
    if (!value || value->type != CTZ_JSON_OBJECT || !key) {
        return NULL;
    }
    size_t key_len = strlen(key);
    for (size_t i = 0; i < value->u.object.size; ++i) {
        if (value->u.object.m[i].klen == key_len &&
            memcmp(value->u.object.m[i].k, key, key_len) == 0) {
            return value->u.object.m[i].v;
        }
    }
    return NULL;
}

int ctz_json_compare(const ctz_json_value* a, const ctz_json_value* b) {
    if (a == b) return 0; // Same pointer
    if (!a || !b) return 1; // One is null
    if (a->type != b->type) return 1; // Different types

    switch (a->type) {
        case CTZ_JSON_NUMBER:
            return a->u.number == b->u.number ? 0 : 1;
        case CTZ_JSON_STRING:
            if (a->u.string.len != b->u.string.len) return 1;
            return memcmp(a->u.string.s, b->u.string.s, a->u.string.len);
        case CTZ_JSON_ARRAY:
            if (a->u.array.size != b->u.array.size) return 1;
            for (size_t i = 0; i < a->u.array.size; i++) {
                if (ctz_json_compare(a->u.array.e[i], b->u.array.e[i]) != 0) {
                    return 1;
                }
            }
            return 0; // All elements matched
        case CTZ_JSON_OBJECT:
            if (a->u.object.size != b->u.object.size) return 1;
            for (size_t i = 0; i < a->u.object.size; i++) {
                // Find matching key in 'b'
                ctz_json_value* b_val = ctz_json_find_object_value(b, a->u.object.m[i].k);
                if (!b_val) return 1; // Key missing in 'b'
                if (ctz_json_compare(a->u.object.m[i].v, b_val) != 0) {
                    return 1; // Values differ
                }
            }
            return 0; // All key/value pairs matched
        case CTZ_JSON_NULL:
        case CTZ_JSON_TRUE:
        case CTZ_JSON_FALSE:
        default:
            return 0; // Types are the same, so they are equal
    }
}

ctz_json_value* ctz_json_duplicate(const ctz_json_value* value, int deep) {
    if (!value) return NULL;
    
    ctz_json_value* new_val = NULL;
    switch (value->type) {
        case CTZ_JSON_NULL:   new_val = ctz_json_new_null(); break;
        case CTZ_JSON_TRUE:   new_val = ctz_json_new_bool(1); break;
        case CTZ_JSON_FALSE:  new_val = ctz_json_new_bool(0); break;
        case CTZ_JSON_NUMBER: new_val = ctz_json_new_number(value->u.number); break;
        case CTZ_JSON_STRING: new_val = ctz_json_new_string(value->u.string.s); break;
        case CTZ_JSON_ARRAY:
            new_val = ctz_json_new_array();
            if (new_val && deep) {
                for (size_t i = 0; i < value->u.array.size; i++) {
                    ctz_json_value* elem_copy = ctz_json_duplicate(value->u.array.e[i], 1);
                    if (!elem_copy) {
                        ctz_json_free(new_val);
                        return NULL; // Allocation failed
                    }
                    ctz_json_array_push_value(new_val, elem_copy);
                }
            }
            break;
        case CTZ_JSON_OBJECT:
            new_val = ctz_json_new_object();
            if (new_val && deep) {
                for (size_t i = 0; i < value->u.object.size; i++) {
                    ctz_json_value* val_copy = ctz_json_duplicate(value->u.object.m[i].v, 1);
                    if (!val_copy) {
                        ctz_json_free(new_val);
                        return NULL; // Allocation failed
                    }
                    ctz_json_object_set_value(new_val, value->u.object.m[i].k, val_copy);
                }
            }
            break;
    }
    return new_val;
}
