#pragma once
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "lexer.h"

typedef struct {
    const char* text;
    int left_bp;
    int right_bp;
} InfixOp;

// left and right associative binding powers (higher precedendce),
// where left_bp < right_bp means left associative
// and left_bp >= right_bp right associative
// https://en.cppreference.com/c/language/operator_precedence
static const InfixOp opl[] = {
    {"=", 4, 3},
    {"+=", 4, 3},{"-=", 4, 3},
    {"*=", 4, 3},{"/=", 4, 3},
    {"%=", 4, 3},
    {"<<=", 4, 3},{">>=", 4, 3},
    {"&=", 4, 3},{"^=", 4, 3},{"!=", 4, 3},

    {"?", 5, 5},

    {"||", 6, 7}, // 67
    {"&&", 8, 9},

    // Bitwise
    {"|", 10, 11},
    {"^", 12, 13},
    {"&", 14, 15},

    // Equality
    {"==", 16, 17}, {"!=", 16, 17},

    // Comparison
    {"<", 18, 19},  {">", 18, 19},
    {"<=", 18, 19}, {">=", 18, 19},

    // Bit shifting
    {"<<", 20, 21}, {">>", 20, 21},

    // Arithmetic
    {"+", 22, 23}, {"-", 22, 23},
    {"*", 24, 25}, {"/", 24, 25}, {"%", 24, 25},
};
static const int opl_size = sizeof(opl) / sizeof(InfixOp);


// Unary operators
static const char* unary[] = {"++","--","+","-","*","&","!","~"};
static const int unary_size = sizeof(unary) / sizeof(char*);


#define debug(msg, tok) printf("%s: %.*s %s\n", msg, (int)(tok).length, (tok).start, TokenKind_name_map[(tok).kind])


typedef enum {
    PERR_OK = 0,         // default no err
    PERR_UNEXPECTED,     // unexpected token
    PERR_REC_FAIL,       // stack machine recursion failed
    PERR_OPEN_CBRACK     // open curly bracket ('}')
} ParseErr;

static const char* ParseErr_map[] = {
    "PERR_OK",
    "PERR_UNEXPECTED",
    "PERR_REC_FAIL",
    "PERR_OPEN_CBRACK"
};

typedef struct {
    Lexer lexer;
    Token token;
    ParseErr err[16];
} ParserSnapshot;


#define PERROR_MAX 16
typedef struct Parser {
    Lexer lexer;
    Token token;
    ParseErr err[PERROR_MAX];

    ParserSnapshot snapshot[PERROR_MAX]; int snapshot_idx;
} Parser;

typedef enum {
    E_ALL = 0, // TODO: Temporary value until I actually implement this properly
    E_IF,
    E_WHILE,
    E_DOWHILE,
    E_FOR,
    E_COND,
    E_IDENT,
    E_UNARY,
    E_INDEX, E_MEM, E_CALL, E_POSTFIX,
    E_SWITCH, E_CASE,
    E_BINARY,
    E_TERNARY,
    E_BLOCK,
    E_ERR,
    E_CONTINUE, E_BREAK,
    E_RETURN
} ExprKind;

static const char* ExprKind_map[] = {
    "E_ALL",
    "E_IF",
    "E_WHILE",
    "E_DOWHILE",
    "E_FOR",
    "E_COND",
    "E_IDENT",
    "E_UNARY",
    "E_INDEX", "E_MEM", "E_CALL", "E_POSTFIX",
    "E_SWITCH", "E_CASE",
    "E_BINARY",
    "E_TERNARY",
    "E_BLOCK",
    "E_ERR",
    "E_CONTINUE", "E_BREAK",
    "E_RETURN"
};

typedef struct Expr Expr;

typedef struct Condition {
    Expr* base_case; // Optional
    Expr* condition;
    Expr* increment; // Optional
    Expr* block;
} Condition;


typedef struct Expr {
    ExprKind kind;
    Token token;
    Condition* cases; // first case is default for size > 1
    size_t case_size;

    Expr** blockstmt;
    size_t blockstmt_size;
    size_t alloc_blockstmt_size;

    struct Expr* left;
    struct Expr* right;
} Expr;




static inline Parser parser_new(const char* source) {
    Lexer lex = lexer_new(source);
    Token token = lexer_next(&lex);
    return (Parser) {
        .lexer = lex,
        .token = token
    };
}

// TODO: Better error messages
static inline int parser_error(Parser* p) {
    // return last parser error
    for(size_t i = 0; i < PERROR_MAX; i++){
        if(p->err[i] != PERR_OK)
            return p->err[i];
    }
    return PERR_OK;
}

static inline void parser_error_reset(Parser* p) {
    // return last parser error
    for(size_t i = 0; i < PERROR_MAX; i++){
            p->err[i] = PERR_OK;
    }
}

static inline int set_parser_error(Parser* p, ParseErr e) {
    assert(e && "PERR_OK is not an error!");
    size_t i;
    for(i = 0; i < PERROR_MAX; i++){
        if(p->err[i] == PERR_OK){
            p->err[i] = e;
            break;
        }
    }
    assert(i != PERROR_MAX && "Cannot insert any more errors, error array full");
    #if 1
        if(parser_error(p))
            abort();
    #endif
    return parser_error(p);
}


static inline void print_expr(Expr* expr, int depth) {
    assert(expr);
    printf("%*s%s \"%.*s\" [%s]\n",
           depth * 2, "", ExprKind_map[expr->kind],
           (int)expr->token.length, expr->token.start,
           TokenKind_name_map[expr->token.kind]);

    for (size_t i = 0; i < expr->case_size; i++) {
        const Condition* condition = &expr->cases[i];
        Expr* parts[] = {
            condition->base_case, condition->condition,
            condition->increment, condition->block
        };
        const char* labels[] = {"Base case", "Condition", "Increment", "Block"};

        printf("%*sCase %zu%s:\n", (depth + 1) * 2, "", i,
               expr->case_size > 1 && i == 0 ? " (default)" : "");
        for (size_t j = 0; j < sizeof(parts) / sizeof(parts[0]); j++) {
            if (parts[j]) {
                printf("%*s%s:\n", (depth + 2) * 2, "", labels[j]);
                print_expr(parts[j], depth + 3);
            }
        }
    }

    for (size_t i = 0; i < expr->blockstmt_size; i++) {
        printf("%*sStatement %zu:\n", (depth + 1) * 2, "", i);
        print_expr(expr->blockstmt[i], depth + 2);
    }

    if (expr->left) {
        printf("%*sLeft:\n", (depth + 1) * 2, "");
        print_expr(expr->left, depth + 2);
    }
    if (expr->right) {
        printf("%*sRight:\n", (depth + 1) * 2, "");
        print_expr(expr->right, depth + 2);
    }
}

static inline Expr* expr_new(ExprKind kind, Token token, Expr* left, Expr* right) {
    Expr* e = (Expr*)calloc(1, sizeof(Expr));
    assert(e);
    e->kind = kind;
    e->token = token;
    e->left = left;
    e->right = right;
    return e;
}

static inline void free_expr(Expr* expr) {
    if (!expr) return;

    free_expr(expr->right);
    free_expr(expr->left);

    for (size_t i = 0; i < expr->case_size; i++) {
        Condition* condition = &expr->cases[i];
        free_expr(condition->base_case);
        free_expr(condition->condition);
        free_expr(condition->increment);
        free_expr(condition->block);
    }
    free(expr->cases);

    for (size_t i = 0; i < expr->blockstmt_size; i++) {
        free_expr(expr->blockstmt[i]);
    }
    free(expr->blockstmt);
    free(expr);
}

static inline void parser_push_snapshot(Parser* p) {
    assert(p->snapshot_idx < PERROR_MAX);
    p->snapshot[p->snapshot_idx] = (ParserSnapshot){p->lexer, p->token};
    memcpy(&p->snapshot[p->snapshot_idx].err, &p->err, PERROR_MAX*sizeof(ParseErr));
    p->snapshot_idx++;
}

static inline void parser_pop_snapshot(Parser* p) {
    assert(p->snapshot_idx > 0);
    ParserSnapshot snapshot = p->snapshot[--p->snapshot_idx];

    p->lexer = snapshot.lexer;
    p->token = snapshot.token;
    memcpy(p->err, &snapshot.err, 256*sizeof(ParseErr));
}

static inline void parser_discard_snapshot(Parser* p) {
    assert(p->snapshot_idx > 0);
    --p->snapshot_idx;
}

static inline Token parser_advance(Parser* p) {
    p->token = lexer_next(&p->lexer);
    #if 1
    debug("ADVANCE", p->token);
    #endif
    return p->token;
}

static inline Token parser_peek(Parser* p) {
    return p->token;
}

static inline int parser_expect(Parser* p, const char* e) {
    if (token_is(parser_peek(p), e)){
        if (!parser_error(p))
            parser_advance(p);
        return parser_error(p);
    } else {
        return set_parser_error(p, PERR_UNEXPECTED);
    }
}

#define TOK(s, k) { \
    .op = P_TOKEN, \
    .as = { .text = (s), .kind = (k) } \
}

#define REC(f) { \
    .op = P_RECURSE, \
    .as.fn = (f) \
}

#define ISTACK(f) { \
    .op = P_ISTACK, \
    .as.istack_fn = (f) \
}

#define BIF(s) { \
    .op = P_BIF, \
    .as = { .text = (s) } \
}

#define BIFN(s) { \
    .op = P_BIFN, \
    .as = { .text = (s) } \
}


#define FN(f)           { .op = P_RECURSE, .as.fn = (f) }
#define PUSH            { .op = P_PUSH }
#define POP             { .op = P_POP }
#define LEFT            { .op = P_LEFT }
#define RIGHT           { .op = P_RIGHT }
#define DISCARD         { .op = P_DISCARD }
#define END             { .op = P_END }
#define PUSH_SNAPSHOT   { .op = P_PUSH_SNAPSHOT }
#define POP_SNAPSHOT    { .op = P_POP_SNAPSHOT }
#define FSTACK          { .op = P_FSTACK }
#define FCONTEXT        { .op = P_FCONTEXT }
#define LEFT_COND       { .op = P_FCONTEXT }
#define RIGHT_COND      { .op = P_FCONTEXT }
#define EXPECT(s)       TOK(s,E_ALL), DISCARD

typedef enum {
    P_TOKEN,                // match a specific token
    P_RECURSE,              // call a function pointer
    P_PUSH,                 // push current expr onto stack
    P_POP,                  // pop latest expr into context
    P_LEFT,                 // put expr into left of context
    P_RIGHT,                // put expr into right of context
    P_DISCARD,              // discard topmost stack expr
    P_PUSH_SNAPSHOT,        // push lexer snapshot to parser
    P_POP_SNAPSHOT,         // pop lexer snapshot to parser
    P_BIF,                  // branch if token
    P_BIFN,                 // branch if not token
    P_ISTACK,               // interact with the stack directly

    P_FSTACK,               // debug print the stack
    P_FCONTEXT,             // debug print the context

    P_END
} ParseOp;

typedef Expr* (*ParseFn)(Parser*);
typedef void (*InteractStack)(Expr*, Expr*[256], size_t*);

typedef struct {
    ParseOp op;
    union {
        struct {
            const char* text;
            ExprKind kind;
        };
        ParseFn fn;
        InteractStack istack_fn;
    } as;
} ParseRule;


static inline Expr* parse_rule(Parser* p, const ParseRule pr[]) {
    Expr* stack[256]; size_t idx = 0;
    Expr* context = NULL;
    for (size_t i = 0; pr[i].op != P_END; i++) {
        switch (pr[i].op) {
            case P_FCONTEXT: {
                if (context)
                    debug("CONTEXT", context->token);
                else
                    printf("Context Empty\n");
                break;
            }
            case P_FSTACK: {
                printf("STACK:\n");
                for (size_t i = 0; i < idx; i++) {
                    char msg[256];
                    if (snprintf(msg, 256, "\t%d STACK", (int)i) < 0) {assert(0 && "snprintf failed");};
                    if (!stack[i]) {
                        printf("\t%d STACK: Err Nullptr\n", (int)i);
                    } else {
                        debug(msg, stack[i]->token);
                    }
                }
                break;
            }
            case P_TOKEN: {
                assert(pr[i].as.text);
                if (token_is(parser_peek(p), pr[i].as.text)){
                    assert(idx < 255);
                    stack[idx++] = expr_new(pr[i].as.kind, parser_peek(p), NULL, NULL);
                    parser_advance(p);
                } else { set_parser_error(p, PERR_UNEXPECTED); return NULL; }
                break;
            }
            case P_RECURSE: {
                assert(idx < 255);
                stack[idx++] = pr[i].as.fn(p);
                if (!stack[idx-1]){
                        set_parser_error(p, PERR_REC_FAIL);
                        return NULL;
                }
                break;
            }
            case P_ISTACK: {
                pr[i].as.istack_fn(context, stack, &idx);
                break;
            }
            case P_PUSH: {
                assert(idx < 255);
                stack[idx++] = context;
                context = NULL;
                break;
            }
            case P_POP: {
                // popping an empty stack does nothing
                if(idx > 0)
                    context = stack[--idx];
                break;
            }
            case P_LEFT: {
                assert(context && "Context empty");
                if (idx > 0)
                    context->left = stack[--idx];
                else
                    context->left = NULL;
                break;
            }
            case P_RIGHT: {
                assert(context && "Context empty");
                if (idx > 0)
                    context->right = stack[--idx];
                else
                    context->right = NULL;
                break;
            }
            case P_DISCARD: {
                assert(idx > 0);
                free_expr(stack[--idx]); stack[idx] = NULL;
                break;
            }
            case P_PUSH_SNAPSHOT: {
                parser_push_snapshot(p);
                break;
            }
            case P_POP_SNAPSHOT: {
                parser_pop_snapshot(p);
                break;
            }
            case P_BIF:
            case P_BIFN:
            {
                assert(idx < 255);
                if ((pr[i].op == P_BIFN) ^ token_is(parser_peek(p), pr[i].as.text)){
                    stack[idx++] = parse_rule(p, pr+i+1);
                }

                int nesting = 1; int offset = 1;

                while (nesting > 0) {
                    ParseOp op = pr[i + offset].op;
                    if (op == P_END) {
                        nesting--;
                    } else if (op == P_BIF || op == P_BIFN) {
                        nesting++;
                    }
                    offset++;
                }

                i += offset - 1;

                break;
            }
        }
    }
    return context;
}



static inline Expr* parse_if(Parser* p);
static inline void  istack_ifelse_cond(Expr* context, Expr* stack[256], size_t* idx);
static inline Expr* parse_if_cond(Parser* p);

static inline Expr* parse_for(Parser* p);
static inline Expr* parse_for_cond(Parser* p);
static inline void  istack_for_cond(Expr* context, Expr* stack[256], size_t* idx);

static inline Expr* parse_while(Parser* p);
static inline Expr* parse_while_cond(Parser* p);
static inline void  istack_while_cond(Expr* context, Expr* stack[256], size_t* idx);

static inline Expr* parse_dowhile(Parser* p);
static inline void  istack_dowhile_cond(Expr* context, Expr* stack[256], size_t* idx);

static inline Expr* parse_return(Parser* p);
static inline Expr* parse_break(Parser* p);
static inline Expr* parse_continue(Parser* p);

static inline Expr* parse_switchcase(Parser* p);

static inline Expr* parse_block(Parser* p);
static inline Expr* parse_statement(Parser* p);

static inline Expr* parse_expression(Parser* p);
static inline Expr* parse_pratt(Parser* p, int min_bp);
static inline Expr* parse_prefix(Parser* p);
static inline Expr* parse_postfix(Parser* p);
static inline Expr* parse_primary(Parser* p);


// If statement

static inline Expr* parse_if_cond(Parser* p) {
    Expr* expr = parse_expression(p);
    return expr_new(E_COND, expr->token, expr, NULL);
}

static inline void istack_ifelse_cond(Expr* context, Expr* stack[256], size_t* idx) {
    int has_else = *idx == 3;
    Condition* if_cond = (Condition*)calloc(has_else ? 2 : 1,sizeof(Condition));
    if_cond[has_else ? 1 : 0] = (Condition){
        .condition = stack[0],
        .block     = stack[1]
    };
    if(has_else){
        if_cond[0] = (Condition){
            .block = stack[2],
        };
    }
    context->cases      = if_cond;
    context->case_size  = has_else ? 2 : 1;
}

static const ParseRule if_rule[] = {
    TOK("if", E_IF),
    POP,

    EXPECT("("),
    REC(parse_if_cond),
    EXPECT(")"),

    REC(parse_statement),

    BIF("else"),
        EXPECT("else"),
        REC(parse_statement),
        POP, // put into return context
    END,
    //FSTACK,
    ISTACK(istack_ifelse_cond),
    END
};


static inline Expr* parse_if(Parser* p){
    return parse_rule(p, if_rule);
}

// END if statement

// For statement

static inline Expr* parse_for_cond(Parser* p) {
    Token tok = parser_peek(p);
    if(!(token_is(tok, ";") || token_is(tok, ")"))){
        Expr* expr = parse_expression(p);
        return expr_new(E_COND, expr->token, expr, NULL);
    } else {
        // Insert placeholder if there is no actual expression
        return expr_new(E_ALL, tok, NULL, NULL);
    }

}

static inline void istack_for_cond(Expr* context, Expr* stack[256], size_t* idx) {
    context->cases = (Condition*)calloc(1, sizeof(Condition));
    context->cases->base_case = (stack[0]->kind != E_ALL) ? stack[0] : NULL;
    context->cases->condition = (stack[1]->kind != E_ALL) ? stack[1] : NULL;
    context->cases->increment = (stack[2]->kind != E_ALL) ? stack[2] : NULL;
    context->cases->block     =                             stack[3];
    context->case_size        = 1;
}

static const ParseRule for_rule[] = {
    TOK("for", E_FOR),
    POP,

    // (...;...;...)
    EXPECT("("),
        REC(parse_for_cond),
        EXPECT(";"),
        REC(parse_for_cond),
        EXPECT(";"),
        REC(parse_for_cond),
    EXPECT(")"),

    REC(parse_statement),

    ISTACK(istack_for_cond),
    END
};

static inline Expr* parse_for(Parser* p){
    return parse_rule(p, for_rule);
}


// END For statement

// While statement

static inline Expr* parse_while_cond(Parser* p) {
    Expr* expr = parse_expression(p);
    return expr_new(E_COND, expr->token, expr, NULL);
}

static inline void istack_while_cond(Expr* context, Expr* stack[256], size_t* idx) {
    context->cases = (Condition*)calloc(1, sizeof(Condition));
    context->cases->condition = stack[0];
    context->cases->block = stack[1];
    context->case_size = 1;
}

static const ParseRule while_rule[] = {
    TOK("while", E_WHILE),
    POP,

    // (...)
    EXPECT("("),
    REC(parse_while_cond),
    EXPECT(")"),

    REC(parse_statement),

    ISTACK(istack_while_cond),
    END
};

static inline Expr* parse_while(Parser* p){
    return parse_rule(p, while_rule);
}


// END While statement

// Do While statement

static inline void istack_dowhile_cond(Expr* context, Expr* stack[256], size_t* idx) {
    context->cases = (Condition*)calloc(1, sizeof(Condition));
    context->cases->block = stack[0];
    context->cases->condition = stack[1];
    context->case_size = 1;
}

static const ParseRule dowhile_rule[] = {
    TOK("do", E_DOWHILE),
    POP,
    REC(parse_statement),

    // while (...)
    EXPECT("while"),
    EXPECT("("),
    REC(parse_while_cond),
    EXPECT(")"),
    EXPECT(";"),

    ISTACK(istack_dowhile_cond),
    END
};

static inline Expr* parse_dowhile(Parser* p){
    return parse_rule(p, dowhile_rule);
}


// END Do While statement

// switch case statment

static inline void istack_switch(Expr* context, Expr* stack[256], size_t* idx){
    context->cases = (Condition*)calloc(1,sizeof(Condition));
    context->case_size = 1;
    context->cases->condition = stack[0];
}

static inline void istack_case(Expr* context, Expr* stack[256], size_t* idx){
    context->cases = (Condition*)calloc(1,sizeof(Condition));
    context->case_size = 1;
    context->cases->condition = stack[0];
    context->cases->block     = stack[1];
}


static const ParseRule switch_rule[] = {
    TOK("switch", E_SWITCH),
    POP,
    EXPECT("("),
    REC(parse_while_cond), // TODO: create general single condition function
    EXPECT(")"),
    ISTACK(istack_switch),
    END,
};

static const ParseRule case_rule[] = {
    TOK("case", E_CASE),
    POP,
    REC(parse_while_cond),
    EXPECT(":"),
    REC(parse_statement),
    ISTACK(istack_case),
    END,
};

static inline Expr* parse_switchcase(Parser* p){
    Expr* switch_e = parse_rule(p, switch_rule);
    if (parser_error(p)) {return NULL;}
    if (parser_expect(p, "{")) {return NULL;}
    Expr* case_e = parse_rule(p, case_rule);
    Expr* head = case_e;
    if(head) {
        while (
            (token_is(parser_peek(p), "case") ||
             token_is(parser_peek(p), "default"))
        ) {
            case_e->right = parse_rule(p, case_rule);
            if (!case_e->right){
                set_parser_error(p, PERR_REC_FAIL);
                free_expr(head);
                return NULL;
            }
            case_e = case_e->right;
        }
    }
    switch_e->left = head;

    if (parser_expect(p, "}")) {return NULL;}
    return switch_e;
}

// END switch case statment

// return

static const ParseRule return_rule[] = {
    TOK("return", E_RETURN),
    POP,
    BIFN(";"),
        REC(parse_expression),
        POP,
    END,
    LEFT,
    EXPECT(";"),
    END,
};

static inline Expr* parse_return(Parser* p) {
    return parse_rule(p, return_rule);
}

// END return

// break
static const ParseRule break_rule[] = {
    TOK("break", E_BREAK),
    POP,
    EXPECT(";"),
    END
};
static inline Expr* parse_break(Parser* p) {
    return parse_rule(p, break_rule);
}
// END break

// continue
static const ParseRule continue_rule[] = {
    TOK("continue", E_CONTINUE),
    POP,
    EXPECT(";"),
    END
};
static inline Expr* parse_continue(Parser* p){
    return parse_rule(p, continue_rule);
}
// END continue

static inline Expr* parse_statement(Parser* p) {
    Token tok = parser_peek(p);

    if (token_is(tok, "{"))
        return parse_block(p);

    if (token_is(tok, "if"))
        return parse_if(p);

    if (token_is(tok, "while"))
        return parse_while(p);

    if (token_is(tok, "for"))
        return parse_for(p);

    if (token_is(tok, "do"))
        return parse_dowhile(p);

    if (token_is(tok, "switch"))
        return parse_switchcase(p);

    if (token_is(tok, "break"))
        return parse_break(p);

    if (token_is(tok, "continue"))
        return parse_continue(p);

    if (token_is(tok, "return"))
        return parse_return(p);

    if (token_is(tok, ";")){
        parser_advance(p);
        return expr_new(E_ALL, tok, NULL, NULL);
    }

    Expr *expr = parse_expression(p);
    if (!expr){
        set_parser_error(p, PERR_REC_FAIL);
        return NULL;
    }
    if (
        parser_expect(p, ";")) {
        free_expr(expr);
        set_parser_error(p, PERR_UNEXPECTED);
        return NULL;
    }

    return expr;
}


static inline Expr* parse_block(Parser* p) {
    if (parser_error(p)) return NULL;
    if (!token_is(parser_peek(p), "{")) return parse_statement(p);

    Expr* block = expr_new(E_BLOCK, parser_peek(p), NULL, NULL);
    parser_advance(p);

    block->alloc_blockstmt_size = 256;
    block->blockstmt = (Expr**)calloc(block->alloc_blockstmt_size, sizeof(*block->blockstmt));
    if (!block->blockstmt) {
        set_parser_error(p, PERR_REC_FAIL);
        free_expr(block);
        return NULL;
    }

    while (!token_is(parser_peek(p), "}")) {
        if (parser_peek(p).kind == TK_EOF) {
            set_parser_error(p, PERR_OPEN_CBRACK);
            free_expr(block);
            return NULL;
        }
        if (parser_peek(p).kind == TK_ERR) {
            free_expr(block);
            return NULL;
        }

        if (block->blockstmt_size >= block->alloc_blockstmt_size) {
            size_t capacity = block->alloc_blockstmt_size + 256;
            if (capacity < block->alloc_blockstmt_size ||
                capacity > (size_t)-1 / sizeof(*block->blockstmt)) {
                set_parser_error(p, PERR_REC_FAIL);
                free_expr(block);
                return NULL;
            }
            Expr** statements = (Expr**)realloc(block->blockstmt, capacity * sizeof(*statements));
            if (!statements) {
                set_parser_error(p, PERR_REC_FAIL);
                free_expr(block);
                return NULL;
            }
            block->blockstmt = statements;
            block->alloc_blockstmt_size = capacity;
        }

        Expr* statement = parse_statement(p);
        if (!statement) {
            set_parser_error(p, PERR_REC_FAIL);
            free_expr(block);
            return NULL;
        }
        block->blockstmt[block->blockstmt_size++] = statement;
        if (parser_error(p)) {
            free_expr(block);
            return NULL;
        }

        if (token_is(parser_peek(p), "}")) break;
    }

    if (parser_expect(p, "}")) {
        set_parser_error(p, PERR_OPEN_CBRACK);
        free_expr(block);
        return NULL;
    }
    return block;
}

static inline Expr* parse_expression(Parser* p) {
    return parse_pratt(p, 0);
}

static inline Expr* parse_pratt(Parser* p, int min_bp) {
    // Prefix and postfix expressions bind more tightly than any infix operator.
    Expr* left = parse_prefix(p);
    if (!left) return NULL;

    for (;;) {
        Token tok = parser_peek(p);
        const InfixOp* op = NULL;
        for (size_t i = 0; i < opl_size; i++) {
            if (token_is(tok, opl[i].text)) {
                op = &opl[i];
                break;
            }
        }

        // Leave delimiters (including argument commas) for the caller.
        if (!op || op->left_bp < min_bp) break;

        if (token_is(tok, "?")) {
            parser_advance(p);                       // consume '?'

            Expr* then_expr = parse_pratt(p, 0);     // full middle expression
            if (parser_expect(p, ":")) {free_expr(left); free_expr(then_expr); return NULL;}
            Expr* else_expr = parse_pratt(p, op->right_bp);

            if (!then_expr || !else_expr) {
                free_expr(left);
                free_expr(then_expr);
                free_expr(else_expr);
                return NULL;
            }

            //same sort of definition as if / switch
            Condition* if_cond = (Condition*)calloc(2,sizeof(Condition));
            // if expr
            if_cond[1] = (Condition){
                .condition = left,
                .block     = then_expr
            };

            // else case (default in switch)
            if_cond[0] = (Condition){
                .block = else_expr,
            };

            left             = expr_new(E_TERNARY, tok, NULL, NULL);
            left->cases      = if_cond;
            left->case_size  = 2;

            continue;
        }

        parser_advance(p);
        Expr* right = parse_pratt(p, op->right_bp);
        if (!right) {
            free_expr(left);
            return NULL;
        }
        left = expr_new(E_BINARY, tok, left, right);
    }
    return left;
}

static inline Expr* parse_prefix(Parser* p) {
    Token tok = parser_peek(p);
    for (int i = 0; i < unary_size; i++) {
        if(token_is(tok, unary[i])) {
            parser_advance(p);
            return expr_new(E_UNARY, tok, parse_prefix(p), NULL);
        }
    }

    return parse_postfix(p);
}

static inline Expr* parse_postfix(Parser* p) {
    Expr* left = parse_primary(p);
    Token tok = parser_peek(p);
    while (1) {
        Token tok = parser_peek(p);

        // Existing operand followed by ++ or --.
        if (token_is(tok, "++") || token_is(tok, "--")) {
            parser_advance(p);
            left = expr_new(E_POSTFIX, tok, left, NULL);
            continue;
        }

        if (token_is(tok, "[")) {
            parser_advance(p);
            Expr* expr = parse_expression(p);

            if(parser_expect(p, "]")) {
                free_expr(expr);
                assert(0 && "Unclosed bracket ']'");
            }
            left = expr_new(E_INDEX, tok, expr, left);
            continue;
        }

        if (token_is(tok, "(")) {
            parser_advance(p);

            Expr* expr = expr_new(E_MEM, parser_peek(p), NULL, NULL);
            Expr* head = expr;

            if(!token_is(parser_peek(p), ")")) {
                head->left = parse_expression(p);
                while(token_is(parser_peek(p), ",")) {
                    parser_advance(p);
                    head->right = expr_new(E_MEM, parser_peek(p), parse_expression(p), NULL);
                    debug("meth", parser_peek(p));
                    head = head->right;
                }
            }
            if(parser_expect(p, ")")) {
                free_expr(expr);
                assert(0 && "Unclosed call bracket ')'");
            }
            left = expr_new(E_CALL, tok, expr, left);
            continue;
        }

        return left;
    }
}

static inline Expr* parse_primary(Parser* p) {
    Token tok = parser_peek(p);
    // Case 1: you have a '(' -> advance the '('
    // Case 2: you have a primary expr -> advance the primary expr
    // Both cases advance the tok so just do it here
    parser_advance(p);

    if (token_is(tok, "(")) {
        Expr* expr = parse_expression(p);

        if(parser_expect(p, ")")) {
            free_expr(expr);
            assert(0 && "Unclosed bracket ')'");
        }
        return expr;
    }

    switch (tok.kind) {
        case TK_IDENT:
        case TK_PP_NUM:
        case TK_NUM:
        case TK_RAW:
        case TK_STR:
        case TK_CHAR:
            return expr_new(E_IDENT, tok, NULL, NULL);
        default:
            set_parser_error(p, PERR_UNEXPECTED);
            return NULL;
    }
}
