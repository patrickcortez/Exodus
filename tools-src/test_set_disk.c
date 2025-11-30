#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ctz-set.h"

int main() {
    const char* db_path = "test_db.set";
    remove(db_path); // Clean start

    printf("Creating DB...\n");
    SetConfig* cfg = set_create(db_path);
    if (!cfg) {
        fprintf(stderr, "Failed to create DB\n");
        return 1;
    }

    printf("Setting values...\n");
    SetNode* root = set_get_root(cfg);
    
    // Set String
    SetNode* name = set_set_child(root, "name", SET_TYPE_STRING);
    set_node_set_string(name, "Cortez");
    
    // Set Int
    SetNode* ver = set_set_child(root, "version", SET_TYPE_INT);
    set_node_set_int(ver, 42);

    // Create Sub-Map
    SetNode* features = set_set_child(root, "features", SET_TYPE_MAP);
    SetNode* f1 = set_set_child(features, "f1", SET_TYPE_BOOL);
    set_node_set_bool(f1, 1);

    // Commit and Close
    printf("Committing...\n");
    // set_db_commit is not strictly needed if we are just closing, 
    // but we should ensure buffer pool is flushed.
    // set_free calls bpm_close which flushes.
    set_free(cfg);

    printf("Re-opening DB...\n");
    cfg = set_load(db_path);
    if (!cfg) {
        fprintf(stderr, "Failed to load DB\n");
        return 1;
    }

    root = set_get_root(cfg);
    
    printf("Verifying values...\n");
    const char* n = set_get_string(cfg, NULL, "name", "default");
    printf("Name: %s\n", n);
    if (strcmp(n, "Cortez") != 0) {
        fprintf(stderr, "FAIL: Name mismatch\n");
        return 1;
    }

    long v = set_get_int(cfg, NULL, "version", 0);
    printf("Version: %ld\n", v);
    if (v != 42) {
        fprintf(stderr, "FAIL: Version mismatch\n");
        return 1;
    }

    SetNode* feat = set_get_child(root, "features");
    if (!feat) {
        fprintf(stderr, "FAIL: Features map missing\n");
        return 1;
    }
    
    SetNode* f1_node = set_get_child(feat, "f1");
    int f1_val = set_node_bool(f1_node, 0);
    printf("Feature F1: %d\n", f1_val);
    if (f1_val != 1) {
        fprintf(stderr, "FAIL: Feature F1 mismatch\n");
        return 1;
    }

    set_free(cfg);
    printf("SUCCESS\n");
    return 0;
}
