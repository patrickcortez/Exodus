#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ctz-set.h"

int main() {
    printf("Testing set_load with text file...\n");

    // Create a temporary text file
    const char* filename = "test_config.set";
    FILE* f = fopen(filename, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "name: \"TextConfig\"\nvalue: 123\n");
    fclose(f);

    // Load using set_load
    SetConfig* cfg = set_load(filename);
    if (!cfg) {
        fprintf(stderr, "FAILED: set_load returned NULL for text file\n");
        return 1;
    }
    
    const char* name = set_get_string(cfg, "global", "name", NULL);
    if (!name || strcmp(name, "TextConfig") != 0) {
        fprintf(stderr, "FAILED: set_load did not parse 'name' correctly. Got: %s\n", name ? name : "NULL");
        const char* err = set_get_last_error(cfg);
        if (err && err[0]) fprintf(stderr, "Config Error: %s\n", err);
        return 1;
    }
    
    long val = set_get_int(cfg, "global", "value", 0);
    if (val != 123) {
        fprintf(stderr, "FAILED: set_load did not parse 'value' correctly. Got: %ld\n", val);
        return 1;
    }
    
    set_free(cfg);
    remove(filename);
    
    printf("PASSED: set_load with text file\n");
    return 0;
}
