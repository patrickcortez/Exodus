#ifndef NLANG_DEFS_H
#define NLANG_DEFS_H

#include <stdint.h>

// OpCodes
typedef enum {
    OP_HALT = 0,
    OP_PUSH_IMM,    // PUSH_IMM <int>
    OP_PUSH_STR,    // PUSH_STR <str_id> (index into string table)
    OP_POP,         // Discard top
    
    // Variables
    OP_LOAD,        // LOAD <var_id> (index into var table)
    OP_STORE,       // STORE <var_id>
    
    // Math
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    
    // Comparison
    OP_EQ,
    OP_NEQ,
    OP_GT,
    OP_LT,
    OP_GTE,
    OP_LTE,
    
    // Logic
    OP_NOT,
    
    // IO
    OP_PRINT,       // Pop and print
    OP_INPUT,       // Read input and push to stack
    
    // Control Flow
    OP_JMP,         // JMP <addr>
    OP_JMP_FALSE,   // JMP_FALSE <addr> (Pop and jump if 0)
    OP_CALL,        // CALL <addr>
    OP_RET          // Return
} OpCode;

// Binary Header
typedef struct {
    char magic[4]; // "NLNG"
    uint32_t version;
    uint32_t code_size;
    uint32_t string_table_size;
    uint32_t entry_point;
} NLangHeader;

#endif
