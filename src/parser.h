#pragma once
#import <stdlib.h>
#include <string.h>
#import <assert.h>
#import <stdio.h>
#import "lexer.h"

typedef struct {
    const char* text;
    int left_bp;
    int right_bp;
} InfixOp;

// left and right associative binding powers (higher precedendce),
// where left_bp < right_bp means left associative
// and left_bp >= right_bp right associative
static const InfixOp opl[] = {
    {"+", 10, 11}, {"-", 10, 11},
    {"*", 20, 21}, {"/", 20, 21},
};
static const int opl_size = sizeof(opl) / sizeof(InfixOp);


// Unary operators
static const char* unary[] = {"++","--","+","-","*","&","!","~"};
static const int unary_size = sizeof(unary) / sizeof(char*);


#define debug(msg, tok) printf("%s: %.*s %s\n", msg, (int)tok.length, tok.start, TokenKind_name_map[tok.kind])


typedef struct TokenWrapper {
    Token token;
    struct TokenWrapper* next;
} TokenWrapper;


typedef struct Parser {
    Lexer lexer;
    TokenWrapper* tk_w;
} Parser;

typedef enum {
    E_IF,
    E_IDENT,
    E_UNARY,
    E_INDEX,E_MEM,E_CALL,
    E_BINARY,
    E_ALL, // TODO: Temporary value until I actually implement this properly
} ExprKind;

static const char* ExprKind_map[] = {
    "E_IF",
    "E_IDENT",
    "E_UNARY",
    "E_INDEX","E_MEM","E_CALL",
    "E_BINARY",
    "E_ALL"
};


typedef struct Expr {
    ExprKind kind;
    Token token;

    struct Expr* left;
    struct Expr* right;
} Expr;

static inline Parser parser_new(const char* source) {
    Lexer lex = lexer_new(source);
    TokenWrapper* tk_w = (TokenWrapper*)calloc(1, sizeof(TokenWrapper));
    tk_w->token = lexer_next(&lex);
    return (Parser) {
        .lexer = lex,
        .tk_w = tk_w
    };
}

static inline void parser_free(Parser* parser) {
    TokenWrapper* curr = parser->tk_w;
    while (curr) {
        TokenWrapper* next = curr->next;
        free(curr);
        curr = next;
    }
}
//
// TODO: Implement checkpointing
//
// static inline Parser parser_ckpt(Parser* p) {
//     Parser c = *p;
//     TokenWrapper* tk_w = (TokenWrapper*)calloc(1, sizeof(TokenWrapper));
//     memcpy(tk_w, p->tk_w, sizeof(TokenWrapper));
//     c.tk_w = tk_w;
//     c.tk_w->next = NULL;
//     return c;
// }
//
// static inline Parser* parser_load_ckpt(Parser* p, Parser ckpt) {
//     *p = ckpt;
//     return p;
// }

static inline void print_expr(Expr* expr, int depth){
    assert(expr);
    printf("%.*s %s, %s\n", (int)expr->token.length, expr->token.start, TokenKind_name_map[expr->token.kind], ExprKind_map[expr->kind]);
    if (expr->left){
        printf("%*c",depth*4, ' ');
        print_expr(expr->left, depth+1);
    }
    if (expr->right){
        printf("%*c",depth*4,' ');
        print_expr(expr->right, depth+1);
    }
}

static inline Expr* expr_new(ExprKind kind, Token token, Expr* left, Expr* right) {
    Expr* e = (Expr*)malloc(sizeof(Expr));
    assert(e);
    e->kind = kind;
    e->token = token;
    e->left = left;
    e->right = right;
    return e;

}

static inline void free_expr(Expr* expr) {
    if (expr->right) free_expr(expr->right);
    if (expr->left) free_expr(expr->left);
    free(expr);
}

static inline Token parser_advance(Parser* p) {
    TokenWrapper* next = p->tk_w->next;
    if (!next){
        next = (TokenWrapper*)calloc(1, sizeof(TokenWrapper));
        assert(next);
        next->token = lexer_next(&p->lexer);
    }

    free(p->tk_w);
    p->tk_w = next;
    debug("ADVANCE", p->tk_w->token);
    return p->tk_w->token;
}

static inline Token parser_peek_at(Parser* p, size_t offset) {
    TokenWrapper* curr = p->tk_w;
    for (size_t i = 0; i < offset; i++) {
        if (!curr->next) {
            curr->next = (TokenWrapper*)calloc(1, sizeof(TokenWrapper));
            assert(curr->next);
            curr->next->token = lexer_next(&p->lexer);
        }
        curr = curr->next;
    }
    return curr->token;
}
static inline Token parser_peek(Parser* p) {
    return parser_peek_at(p, 0);
}

static inline int parser_expect(Parser* p, const char* e) {
    int i = token_is(parser_peek(p), e);
    parser_advance(p);
    return i;
}

#define TOK(s, k) { \
    .op = P_TOKEN, \
    .as = { .text = (s), .kind = (k) } \
}

#define REC(f) { \
    .op = P_RECURSE, \
    .as.fn = (f) \
}

#define FN(f)    { .op = P_RECURSE, .as.fn = (f) }
#define PUSH     { .op = P_PUSH }
#define POP      { .op = P_POP }
#define LEFT     { .op = P_LEFT }
#define RIGHT    { .op = P_RIGHT }
#define DISCARD  { .op = P_DISCARD }
#define END      { .op = P_END }

typedef enum {
    P_TOKEN,    // match a specific token
    P_RECURSE,  // call a function pointer
    P_PUSH,     // push current expr onto stack
    P_POP,      // pop latest expr into context
    P_LEFT,     // put expr into left of context
    P_RIGHT,    // put expr into right of context
    P_DISCARD,  // discard topmost stack expr
    P_END
} ParseOp;

typedef Expr* (*ParseFn)(Parser*);
typedef struct {
    ParseOp op;
    union {
        struct {
            const char* text;
            ExprKind kind;
        };
        ParseFn fn;
    } as;
} ParseRule;


static inline Expr* parse_rule(Parser* p, const ParseRule* pr) {
    Expr* stack[256]; int idx = 0;
    Expr* context = NULL;
    for (size_t i = 0; pr[i].op != P_END; i++) {
        switch (pr[i].op) {
            case P_TOKEN:
                if (token_is(parser_peek(p), pr[i].as.text)){
                    assert(idx < 255);
                    stack[idx++] = expr_new(pr[i].as.kind, parser_peek(p), NULL, NULL);
                    parser_advance(p);
                } else { return NULL; }
                break;
            case P_RECURSE:
                assert(idx < 255);
                stack[idx++] = pr[i].as.fn(p);
                if (!stack[idx-1])
                        return NULL;
                break;
            case P_PUSH:
                assert(idx < 255);
                stack[idx++] = context;
                context = NULL;
                break;
            case P_POP:
                assert(idx > 0);
                context = stack[--idx];
                break;
            case P_LEFT:
                assert(context && "Context empty");
                context->left = stack[--idx];
                break;
            case P_RIGHT:
                assert(context && "Context empty");
                context->right = stack[--idx];
                break;
            case P_DISCARD:
                free_expr(stack[--idx]); stack[idx] = NULL;
                break;
        }
    }
    return context;
}



static inline Expr* parse_if(Parser* p);
static inline Expr* parse_expression(Parser* p);
static inline Expr* parse_pratt(Parser* p, int min_bp);
static inline Expr* parse_prefix(Parser* p);
static inline Expr* parse_postfix(Parser* p);
static inline Expr* parse_primary(Parser* p);

static const ParseRule if_rule[] = {
    TOK("if", E_IF),
    POP,
    TOK("(", E_ALL),
    DISCARD,
    REC(parse_expression),
    LEFT,
    TOK(")", E_ALL),
    DISCARD,
    TOK("{", E_ALL),
    DISCARD,
    REC(parse_expression),
    RIGHT,
    TOK("}", E_ALL),
    DISCARD,
    END
};

static inline Expr* parse_if(Parser* p){
    return parse_rule(p, if_rule);
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
    while (token_is(tok, "[") || token_is(tok, "(")) {
        if (token_is(tok, "[")) {

            parser_advance(p);
            Expr* expr = parse_expression(p);

            if(!parser_expect(p, "]")) {
                free_expr(expr);
                assert(0 && "Unclosed bracket ']'");
            }
            left = expr_new(E_INDEX, tok, expr, left);
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
            if(!parser_expect(p, ")")) {
                free_expr(expr);
                assert(0 && "Unclosed call bracket ')'");
            }
            left = expr_new(E_CALL, tok, expr, left);
        }
        tok = parser_peek(p);
    }
    return left;
}

static inline Expr* parse_primary(Parser* p) {
    Token tok = parser_peek(p);
    // Case 1: you have a '(' -> advance the '('
    // Case 2: you have a primary expr -> advance the primary expr
    // Both cases advance the tok so just do it here
    parser_advance(p);

    if (token_is(tok, "(")) {
        Expr* expr = parse_expression(p);

        if(!parser_expect(p, ")")) {
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
        case TK_KEYWORD:
        case TK_STR:
        case TK_CHAR:
            return expr_new(E_IDENT, tok, NULL, NULL);
        default:
            assert(0 && "Unexpected token");
            return NULL;
    }
}
