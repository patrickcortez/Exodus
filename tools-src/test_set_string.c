#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ctz-set.h"

int main() {
    printf("Testing set_parse and set_stringify...\n");

    const char* source = "name: \"Cortez\"\nversion: 2\nfeatures: [\"A\", \"B\"]\n";
    
    // Test set_parse
    SetConfig* cfg = set_parse(source);
    if (!cfg) {
        fprintf(stderr, "FAILED: set_parse returned NULL\n");
        return 1;
    }
    
    const char* name = set_get_string(cfg, "global", "name", NULL);
    if (!name || strcmp(name, "Cortez") != 0) {
        fprintf(stderr, "FAILED: set_parse did not parse 'name' correctly. Got: %s\n", name ? name : "NULL");
        return 1;
    }
    
    long version = set_get_int(cfg, "global", "version", 0);
    if (version != 2) {
        fprintf(stderr, "FAILED: set_parse did not parse 'version' correctly. Got: %ld\n", version);
        return 1;
    }
    
    printf("PASSED: set_parse\n");
    
    // Test set_stringify
    char* output = set_stringify(cfg);
    if (!output) {
        fprintf(stderr, "FAILED: set_stringify returned NULL\n");
        return 1;
    }
    
    printf("Stringify Output:\n%s\n", output);
    
    if (strstr(output, "name: \"Cortez\"") == NULL) {
        fprintf(stderr, "FAILED: set_stringify output missing 'name'\n");
        return 1;
    }
    
    free(output);
    set_free(cfg);
    
    printf("PASSED: set_stringify\n");
    return 0;
}
