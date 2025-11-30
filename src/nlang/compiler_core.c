#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "nlang_defs.h"
#include <setjmp.h>

jmp_buf compile_env;
char error_msg[256];

void compiler_error(const char* msg, int line) {
    snprintf(error_msg, sizeof(error_msg), "Error line %d: %s", line, msg);
    longjmp(compile_env, 1);
}


#define MAX_SRC_SIZE 1024 * 1024
#define MAX_TOKENS 10000
#define MAX_STRINGS 100
#define MAX_VARS 100
#define MAX_CODE 10000

// --- Lexer ---

typedef enum {
    TOK_EOF,
    TOK_ID,
    TOK_NUM,
    TOK_STRING,
    TOK_VAR,
    TOK_FUNC,
    TOK_IF,
    TOK_ELIF,
    TOK_ELSE,
    TOK_OUT,
    TOK_IN,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_COMMA, TOK_SEMICOLON,
    TOK_ASSIGN,
    TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV,
    TOK_EQ, TOK_NEQ, TOK_GT, TOK_LT, TOK_GTE, TOK_LTE, TOK_NOT
} TokenType;

typedef struct {
    TokenType type;
    char text[64];
    int line;
} Token;

static Token tokens[MAX_TOKENS];
static int token_count = 0;

static void add_token(TokenType type, const char* text, int line) {
    tokens[token_count].type = type;
    strncpy(tokens[token_count].text, text, 63);
    tokens[token_count].line = line;
    token_count++;
}

void lex(const char* src) {
    int i = 0;
    int line = 1;
    while (src[i]) {
        if (isspace(src[i])) {
            if (src[i] == '\n') line++;
            i++;
            continue;
        }
        
        // Comments
        if (src[i] == ':') {
            while (src[i] && src[i] != '\n') i++;
            continue;
        }

        // Operators
        if (src[i] == '=' && src[i+1] == '=') { add_token(TOK_EQ, "==", line); i+=2; continue; }
        if (src[i] == '!' && src[i+1] == '=') { add_token(TOK_NEQ, "!=", line); i+=2; continue; }
        if (src[i] == '>' && src[i+1] == '=') { add_token(TOK_GTE, ">=", line); i+=2; continue; }
        if (src[i] == '<' && src[i+1] == '=') { add_token(TOK_LTE, "<=", line); i+=2; continue; }
        
        if (src[i] == '+') { add_token(TOK_PLUS, "+", line); i++; continue; }
        if (src[i] == '-') { add_token(TOK_MINUS, "-", line); i++; continue; }
        if (src[i] == '*') { add_token(TOK_MUL, "*", line); i++; continue; }
        if (src[i] == '/') { add_token(TOK_DIV, "/", line); i++; continue; }
        if (src[i] == '=') { add_token(TOK_ASSIGN, "=", line); i++; continue; }
        if (src[i] == '>') { add_token(TOK_GT, ">", line); i++; continue; }
        if (src[i] == '<') { add_token(TOK_LT, "<", line); i++; continue; }
        if (src[i] == '!') { add_token(TOK_NOT, "!", line); i++; continue; }
        
        if (src[i] == '(') { add_token(TOK_LPAREN, "(", line); i++; continue; }
        if (src[i] == ')') { add_token(TOK_RPAREN, ")", line); i++; continue; }
        if (src[i] == '{') { add_token(TOK_LBRACE, "{", line); i++; continue; }
        if (src[i] == '}') { add_token(TOK_RBRACE, "}", line); i++; continue; }
        if (src[i] == ';') { add_token(TOK_SEMICOLON, ";", line); i++; continue; }
        if (src[i] == ',') { add_token(TOK_COMMA, ",", line); i++; continue; }

        // Numbers
        if (isdigit(src[i])) {
            char buf[32];
            int j = 0;
            while (isdigit(src[i])) buf[j++] = src[i++];
            buf[j] = '\0';
            add_token(TOK_NUM, buf, line);
            continue;
        }

        // Strings
        if (src[i] == '"') {
            i++;
            char buf[128];
            int j = 0;
            while (src[i] && src[i] != '"') buf[j++] = src[i++];
            buf[j] = '\0';
            if (src[i] == '"') i++;
            add_token(TOK_STRING, buf, line);
            continue;
        }

        // Identifiers / Keywords
        if (isalpha(src[i])) {
            char buf[64];
            int j = 0;
            while (isalnum(src[i]) || src[i] == '_') buf[j++] = src[i++];
            buf[j] = '\0';
            
            if (strcmp(buf, "var") == 0) add_token(TOK_VAR, buf, line);
            else if (strcmp(buf, "func") == 0) add_token(TOK_FUNC, buf, line);
            else if (strcmp(buf, "if") == 0) add_token(TOK_IF, buf, line);
            else if (strcmp(buf, "elif") == 0) add_token(TOK_ELIF, buf, line);
            else if (strcmp(buf, "else") == 0) add_token(TOK_ELSE, buf, line);
            else if (strcmp(buf, "out") == 0) add_token(TOK_OUT, buf, line);
            else if (strcmp(buf, "in") == 0) add_token(TOK_IN, buf, line);
            else add_token(TOK_ID, buf, line);
            continue;
        }
        
        char buf[64]; snprintf(buf, sizeof(buf), "Unexpected character '%c'", src[i]);
        compiler_error(buf, line);
    }
    add_token(TOK_EOF, "", line);
}

// --- Code Gen State ---

static uint8_t code[MAX_CODE];
static uint32_t code_pos = 0;

static char string_table[MAX_STRINGS][128];
static int string_count = 0;

static char var_table[MAX_VARS][64];
static int var_count = 0;

void emit(uint8_t b) {
    code[code_pos++] = b;
}

void emit32(uint32_t v) {
    emit(v & 0xFF);
    emit((v >> 8) & 0xFF);
    emit((v >> 16) & 0xFF);
    emit((v >> 24) & 0xFF);
}

// Backpatching
void emit32_at(uint32_t pos, uint32_t v) {
    code[pos] = v & 0xFF;
    code[pos+1] = (v >> 8) & 0xFF;
    code[pos+2] = (v >> 16) & 0xFF;
    code[pos+3] = (v >> 24) & 0xFF;
}

int add_string(const char* str) {
    for (int i = 0; i < string_count; ++i) {
        if (strcmp(string_table[i], str) == 0) return i;
    }
    strcpy(string_table[string_count], str);
    return string_count++;
}

int find_var(const char* name) {
    for (int i = 0; i < var_count; ++i) {
        if (strcmp(var_table[i], name) == 0) return i;
    }
    return -1;
}

int add_var(const char* name) {
    int id = find_var(name);
    if (id != -1) return id;
    strcpy(var_table[var_count], name);
    return var_count++;
}

// --- Parser ---

static int current_token = 0;

static Token* peek() { return &tokens[current_token]; }
static Token* advance() { return &tokens[current_token++]; }
static int match(TokenType type) {
    if (peek()->type == type) {
        advance();
        return 1;
    }
    return 0;
}
void expect(TokenType type, const char* msg) {
    if (!match(type)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s. Found '%s'", msg, peek()->text);
        compiler_error(buf, peek()->line);
    }
}

void parse_expression();

// --- Function Table ---
typedef struct {
    char name[32];
    uint32_t addr;
} FuncEntry;
static FuncEntry functions[64];
static int func_count = 0;

static int find_func(const char* name) {
    for (int i = 0; i < func_count; ++i) {
        if (strcmp(functions[i].name, name) == 0) return i;
    }
    return -1;
}



void parse_factor() {
    Token* t = peek();
    if (match(TOK_NUM)) {
        emit(OP_PUSH_IMM);
        emit32(atoi(t->text));
    } else if (match(TOK_STRING)) {
        int id = add_string(t->text);
        emit(OP_PUSH_STR);
        emit32(id);
    } else if (match(TOK_ID)) {
        // Check if function call
        if (tokens[current_token].type == TOK_LPAREN) {
            // Function call
            Token* func_tok = t; // t is already advanced? No, t = peek(). match(TOK_ID) advanced.
            // Wait, match(TOK_ID) advanced. So t is the ID token.
            // But peek() returns current token.
            // Let's fix logic:
            // t was peek(). match() advanced.
            // So t is valid pointer to previous token? No, peek() returns pointer to array element.
            // If we advance, array element is still there.
            
            // Look up function
            int func_idx = find_func(t->text);
            
            expect(TOK_LPAREN, "(");
            // Parse args
            if (peek()->type != TOK_RPAREN) {
                parse_expression();
                while (match(TOK_COMMA)) {
                    parse_expression();
                }
            }
            expect(TOK_RPAREN, ")");
            
            if (func_idx == -1) {
                char buf[64]; snprintf(buf, sizeof(buf), "Undefined function '%s'", t->text);
                compiler_error(buf, t->line);
            }
            emit(OP_CALL);
            emit32(functions[func_idx].addr);
            // Function returns value? We assume yes for now or void.
            // If void, we might push 0 dummy?
            // Let's assume all funcs return something or we push 0.
        } else {
            // Variable
            int id = find_var(t->text);
            if (id == -1) {
                char buf[64]; snprintf(buf, sizeof(buf), "Undefined variable '%s'", t->text);
                compiler_error(buf, t->line);
            }
            emit(OP_LOAD);
            emit32(id);
        }
    } else if (match(TOK_LPAREN)) {
        parse_expression();
        expect(TOK_RPAREN, "Expected ')'");
    } else if (match(TOK_IN)) {
        expect(TOK_LPAREN, "Expected '(' after in");
        Token* var = peek();
        expect(TOK_ID, "Expected variable name");
        advance();
        expect(TOK_RPAREN, "Expected ')'");
        
        emit(OP_INPUT);
        // Input pushes result to stack.
        // But user syntax is in(var).
        // So we store it.
        int id = add_var(var->text);
        emit(OP_STORE);
        emit32(id);
        emit(OP_PUSH_IMM); emit32(0); // Expr result
    } else {
        char buf[64]; snprintf(buf, sizeof(buf), "Unexpected token '%s'", t->text);
        compiler_error(buf, t->line);
    }
}

void parse_term() {
    parse_factor();
    while (peek()->type == TOK_MUL || peek()->type == TOK_DIV) {
        TokenType op = advance()->type;
        parse_factor();
        if (op == TOK_MUL) emit(OP_MUL);
        else emit(OP_DIV);
    }
}

void parse_additive() {
    parse_term();
    while (peek()->type == TOK_PLUS || peek()->type == TOK_MINUS) {
        TokenType op = advance()->type;
        parse_term();
        if (op == TOK_PLUS) emit(OP_ADD);
        else emit(OP_SUB);
    }
}

void parse_comparison() {
    parse_additive();
    while (peek()->type == TOK_EQ || peek()->type == TOK_NEQ || 
           peek()->type == TOK_GT || peek()->type == TOK_LT ||
           peek()->type == TOK_GTE || peek()->type == TOK_LTE) {
        TokenType op = advance()->type;
        parse_additive();
        switch(op) {
            case TOK_EQ: emit(OP_EQ); break;
            case TOK_NEQ: emit(OP_NEQ); break;
            case TOK_GT: emit(OP_GT); break;
            case TOK_LT: emit(OP_LT); break;
            case TOK_GTE: emit(OP_GTE); break;
            case TOK_LTE: emit(OP_LTE); break;
            default: break;
        }
    }
}

void parse_expression() {
    parse_comparison();
}

void parse_block();

void parse_statement() {
    if (match(TOK_VAR)) {
        Token* t = peek();
        expect(TOK_ID, "Expected variable name");
        int id = add_var(t->text);
        expect(TOK_ASSIGN, "Expected '='");
        parse_expression();
        emit(OP_STORE);
        emit32(id);
        expect(TOK_SEMICOLON, "Expected ';'");
    } else if (match(TOK_OUT)) {
        expect(TOK_LPAREN, "Expected '('");
        parse_expression();
        expect(TOK_RPAREN, "Expected ')'");
        emit(OP_PRINT);
        expect(TOK_SEMICOLON, "Expected ';'");
    } else if (match(TOK_IN)) {
        expect(TOK_LPAREN, "Expected '('");
        Token* t = peek();
        expect(TOK_ID, "Expected variable name");
        int id = add_var(t->text);
        expect(TOK_RPAREN, "Expected ')'");
        emit(OP_INPUT);
        emit(OP_STORE);
        emit32(id);
        expect(TOK_SEMICOLON, "Expected ';'");
    } else if (match(TOK_IF)) {
        expect(TOK_LPAREN, "Expected '('");
        parse_expression();
        expect(TOK_RPAREN, "Expected ')'");
        
        emit(OP_JMP_FALSE);
        uint32_t patch_else = code_pos;
        emit32(0); // Placeholder
        
        parse_block();
        
        uint32_t patch_end = 0;
        if (peek()->type == TOK_ELIF || peek()->type == TOK_ELSE) {
            emit(OP_JMP);
            patch_end = code_pos;
            emit32(0); // Placeholder for jumping to end
        }
        
        // Patch the JMP_FALSE to here (start of else/elif or end)
        emit32_at(patch_else, code_pos);
        
        while (match(TOK_ELIF)) {
            expect(TOK_LPAREN, "Expected '('");
            parse_expression();
            expect(TOK_RPAREN, "Expected ')'");
            
            emit(OP_JMP_FALSE);
            uint32_t patch_next = code_pos;
            emit32(0);
            
            parse_block();
            
            emit32_at(patch_next, code_pos);
        }

        if (match(TOK_ELSE)) {
            parse_block();
        }
        
        if (patch_end != 0) {
            emit32_at(patch_end, code_pos);
        }
        
    } else if (peek()->type == TOK_ID) {
        // Assignment: x = 5;
        Token* t = advance();
        int id = find_var(t->text);
        if (id == -1) {
            char buf[64]; snprintf(buf, sizeof(buf), "Undefined variable '%s'", t->text);
            compiler_error(buf, t->line);
        }
        expect(TOK_ASSIGN, "Expected '='");
        parse_expression();
        emit(OP_STORE);
        emit32(id);
        expect(TOK_SEMICOLON, "Expected ';'");
    } else {
        // Empty statement or error
        if (match(TOK_SEMICOLON)) return;
        char buf[64]; snprintf(buf, sizeof(buf), "Unexpected token '%s'", peek()->text);
        compiler_error(buf, peek()->line);
    }
}

void parse_block() {
    expect(TOK_LBRACE, "Expected '{'");
    while (peek()->type != TOK_RBRACE && peek()->type != TOK_EOF) {
        parse_statement();
    }
    expect(TOK_RBRACE, "Expected '}'");
}

void parse_program() {
    // Emit jump to main (start of code)
    emit(OP_JMP);
    uint32_t main_jump = code_pos;
    emit32(0); // Patch later
    
    while (peek()->type != TOK_EOF) {
        if (match(TOK_FUNC)) {
            Token* name = peek();
            expect(TOK_ID, "Expected function name");
            
            // Register function
            strcpy(functions[func_count].name, name->text);
            functions[func_count].addr = code_pos;
            func_count++;
            
            expect(TOK_LPAREN, "(");
            // Args definition - for now ignore args binding (simple)
            // Or bind them to vars?
            // Let's skip args logic for simplicity in this pass, just consume tokens.
            while (peek()->type != TOK_RPAREN) advance();
            expect(TOK_RPAREN, ")");
            
            parse_block();
            emit(OP_RET);
        } else {
            // Main code
            // We need to mark where main starts.
            // But we can have interleaved funcs and main code?
            // Usually main code is at bottom or top.
            // Let's assume main code is what's left.
            // But we emitted JMP at 0.
            // We should patch JMP to here?
            // If we have multiple blocks of main code, it's messy.
            // Let's assume all functions are defined before main code, or we collect main code?
            // Simple approach:
            // If we hit a statement that is NOT a func, we assume it's main code entry.
            // We patch the start jump to here.
            // But we only do it once.
            static int main_started = 0;
            if (!main_started) {
                emit32_at(main_jump, code_pos);
                main_started = 1;
            }
            parse_statement();
        }
    }
    emit(OP_HALT);
}

// API Entry Point
int nlang_compile(const char* src, uint8_t** out_buf, uint32_t* out_size) {
    // Reset state
    code_pos = 0;
    string_count = 0;
    var_count = 0;
    func_count = 0;
    token_count = 0;
    current_token = 0;
    
    if (setjmp(compile_env) != 0) {
        // Error occurred
        printf("Compilation Failed: %s\n", error_msg);
        return -1;
    }
    
    lex(src);
    parse_program();
    
    // Serialize
    uint32_t total_size = sizeof(NLangHeader) + sizeof(uint32_t); // Header + StrCount
    for(int i=0; i<string_count; ++i) total_size += 4 + strlen(string_table[i]);
    total_size += code_pos;
    
    uint8_t* buf = malloc(total_size);
    uint8_t* ptr = buf;
    
    NLangHeader header;
    strncpy(header.magic, "NLNG", 4);
    header.version = 1;
    header.code_size = code_pos;
    header.string_table_size = 0; // Unused
    header.entry_point = 0;
    
    memcpy(ptr, &header, sizeof(header)); ptr += sizeof(header);
    
    *(uint32_t*)ptr = string_count; ptr += 4;
    for(int i=0; i<string_count; ++i) {
        uint32_t slen = strlen(string_table[i]);
        *(uint32_t*)ptr = slen; ptr += 4;
        memcpy(ptr, string_table[i], slen); ptr += slen;
    }
    
    memcpy(ptr, code, code_pos);
    
    *out_buf = buf;
    *out_size = total_size;
    return 0;
}
