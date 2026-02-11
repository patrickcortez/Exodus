#ifndef AUTOSUGGEST_H
#define AUTOSUGGEST_H

#include <stddef.h>

const char* get_last_token(const char* input);
int scan_token_for_suggestion(const char* token, char* suggestion_buf, size_t buf_size);
int is_exodus_command(const char* input);

#endif
