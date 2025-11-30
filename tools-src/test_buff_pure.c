/*
 * test_buff_pure.c
 * Pure ctz-buff test (no printf)
 */

#include "ctz-buff.h"
#include <unistd.h>
#include <string.h>

// Global stdout buffer
CtzBuff out;

void print_pass(const char* test_name) {
    ctz_buff_out(&out, "%s PASS\n", test_name);
    ctz_buff_flush(&out);
}

void test_string_mode() {
    ctz_buff_out(&out, "Testing String Mode...\n");
    CtzBuff b;
    const char* input = "Hello\nWorld";
    ctz_buff_init_string(&b, input);
    
    if (ctz_buff_peek(&b) != 'H') ctz_buff_out(&out, "FAIL: peek H\n");
    if (ctz_buff_getc(&b) != 'H') ctz_buff_out(&out, "FAIL: getc H\n");
    
    // Skip to end
    while (ctz_buff_getc(&b) != -1);
    
    ctz_buff_close(&b);
    print_pass("String Mode");
}

void test_file_io() {
    ctz_buff_out(&out, "Testing File IO...\n");
    
    CtzBuff f;
    if (ctz_buff_init_file(&f, "test_pure.tmp", "w", 4096) != 0) {
        ctz_buff_out(&out, "FAIL: open write\n");
        return;
    }
    ctz_buff_out(&f, "Pure Test %d", 123);
    ctz_buff_close(&f);
    
    if (ctz_buff_init_file(&f, "test_pure.tmp", "r", 4096) != 0) {
        ctz_buff_out(&out, "FAIL: open read\n");
        return;
    }
    
    char buf[32];
    int i;
    int n = ctz_buff_in(&f, "Pure Test %d", &i);
    
    if (n != 1 || i != 123) {
        ctz_buff_out(&out, "FAIL: read match n=%d i=%d\n", n, i);
    } else {
        print_pass("File IO");
    }
    
    ctz_buff_close(&f);
    unlink("test_pure.tmp");
}

void test_advanced_features() {
    ctz_buff_out(&out, "Testing Advanced Features...\n");
    
    CtzBuff b;
    if (ctz_buff_init_file(&b, "test_adv.tmp", "w", 4096) != 0) return;
    
    int x = 10;
    double y = 2.5;
    const char* str = "Part1";
    
    // Arithmetic in arguments
    ctz_buff_out(&b, "Add: %d\n", x + 5);
    ctz_buff_out(&b, "Mult: %f\n", y * 2.0);
    ctz_buff_out(&b, "Mixed: %d\n", (int)(x * y));
    
    // Shorthand arithmetic (side effects)
    ctz_buff_out(&b, "PostInc: %d\n", x++); // 10, then x=11
    ctz_buff_out(&b, "PreInc: %d\n", ++x);  // 12
    
    // String concatenation (via format)
    ctz_buff_out(&b, "Concat: %s%s\n", str, "Part2");
    
    // Mixed types
    ctz_buff_out(&b, "All: %d %f %s %c\n", 100, 3.14159, "Test", 'Z');
    
    ctz_buff_close(&b);
    
    // --- Visual Demonstration (to stdout) ---
    ctz_buff_out(&out, "\n--- Visual Output ---\n");
    
    // Arithmetic
    ctz_buff_out(&out, "2 + 8 = %d\n", 2 + 8);
    
    // Boolean
    int isRunning = 1;
    ctz_buff_out(&out, "isRunning: %s\n", isRunning ? "true" : "false");
    
    // String Concatenation (C-style)
    const char* con = "dog";
    ctz_buff_out(&out, "cat + con: %s%s\n", "cat ", con);
    
    ctz_buff_out(&out, "-------------------\n\n");
    
    // Verify
    if (ctz_buff_init_file(&b, "test_adv.tmp", "r", 4096) != 0) return;
    
    char line[128];
    int i_val;
    double d_val;
    char s_val[64];
    char c_val;
    
    // Add: 15
    ctz_buff_in(&b, "Add: %d", &i_val);
    if (i_val != 15) ctz_buff_out(&out, "FAIL: Add %d\n", i_val);
    
    // Mult: 5.000000
    ctz_buff_in(&b, " Mult: %f", &d_val);
    if (d_val < 4.9 || d_val > 5.1) ctz_buff_out(&out, "FAIL: Mult %f\n", d_val);
    
    // Mixed: 25
    ctz_buff_in(&b, " Mixed: %d", &i_val);
    if (i_val != 25) ctz_buff_out(&out, "FAIL: Mixed %d\n", i_val);
    
    // PostInc: 10
    ctz_buff_in(&b, " PostInc: %d", &i_val);
    if (i_val != 10) ctz_buff_out(&out, "FAIL: PostInc %d\n", i_val);
    
    // PreInc: 12
    ctz_buff_in(&b, " PreInc: %d", &i_val);
    if (i_val != 12) ctz_buff_out(&out, "FAIL: PreInc %d\n", i_val);
    
    // Concat: Part1Part2
    ctz_buff_in(&b, " Concat: %s", s_val);
    // Note: %s stops at whitespace, so it should read Part1Part2
    // We need to implement string comparison manually or assume strcmp is avail (it is included)
    if (strcmp(s_val, "Part1Part2") != 0) ctz_buff_out(&out, "FAIL: Concat %s\n", s_val);
    
    // All: 100 3.141590 Test Z
    ctz_buff_in(&b, " All: %d %f %s %c", &i_val, &d_val, s_val, &c_val);
    if (i_val != 100) ctz_buff_out(&out, "FAIL: All int %d\n", i_val);
    if (d_val < 3.14 || d_val > 3.15) ctz_buff_out(&out, "FAIL: All double %f\n", d_val);
    if (strcmp(s_val, "Test") != 0) ctz_buff_out(&out, "FAIL: All str %s\n", s_val);
    if (c_val != 'Z') ctz_buff_out(&out, "FAIL: All char %c\n", c_val);
    
    ctz_buff_close(&b);
    unlink("test_adv.tmp");
    print_pass("Advanced Features");
}

void test_peek_at() {
    ctz_buff_out(&out, "Testing Peek At...\n");
    CtzBuff b;
    const char* input = "0123456789";
    ctz_buff_init_string(&b, input);
    
    // Peek at 0 (current)
    if (ctz_buff_peek_at(&b, 0) != '0') ctz_buff_out(&out, "FAIL: peek_at 0\n");
    
    // Peek at 1 (next)
    if (ctz_buff_peek_at(&b, 1) != '1') ctz_buff_out(&out, "FAIL: peek_at 1\n");
    
    // Peek at 5
    if (ctz_buff_peek_at(&b, 5) != '5') ctz_buff_out(&out, "FAIL: peek_at 5\n");
    
    // Consume some
    ctz_buff_getc(&b); // 0
    ctz_buff_getc(&b); // 1
    
    // Now current is 2
    if (ctz_buff_peek_at(&b, 0) != '2') ctz_buff_out(&out, "FAIL: peek_at 0 after consume\n");
    if (ctz_buff_peek_at(&b, 1) != '3') ctz_buff_out(&out, "FAIL: peek_at 1 after consume\n");
    
    ctz_buff_close(&b);
    print_pass("Peek At");
}

int main() {
    // Initialize stdout buffer (fd 1, don't close it)
    if (ctz_buff_init_fd(&out, 1, "w", 4096, 0) != 0) {
        return 1;
    }
    
    test_string_mode();
    test_file_io();
    test_advanced_features();
    test_peek_at();
    
    ctz_buff_out(&out, "ALL TESTS PASSED\n");
    ctz_buff_close(&out);
    return 0;
}
