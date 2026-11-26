#pragma once
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include "lexer.h"
#include "util.h"

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
    {",", 1, 2},

    {"=", 4, 3},
    {"+=", 4, 3},{"-=", 4, 3},
    {"*=", 4, 3},{"/=", 4, 3},
    {"%=", 4, 3},
    {"<<=", 4, 3},{">>=", 4, 3},
    {"&=", 4, 3},{"^=", 4, 3},{"|=", 4, 3},

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

// Minimum binding powers of the restricted expression levels
#define BP_ASSIGN 3     // assignment-expression (no comma operator)
#define BP_CONST  5     // constant-expression (no assignment)

// Dotted operators without a @prec annotation
#define DOTOP_LEFT_BP  26
#define DOTOP_RIGHT_BP 27

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
} ParserSnapshot;

// One visible ordinary identifier: either a type name or something hiding one
typedef struct {
    const char* start;
    size_t length;
    int is_type;
} ParserName;

#define PERROR_MAX 16
#define PSNAPSHOT_MAX 64
typedef struct Parser {
    Lexer lexer;
    Token token;
    ParseErr err[PERROR_MAX];

    ParserSnapshot snapshot[PSNAPSHOT_MAX]; int snapshot_idx;

    const char* filename;       // for diagnostics
    int header_mode;            // input is preprocessed C: skip compiler noise, recover per declaration
    int preprocessed;           // constant expression already expanded by the C preprocessor
    int prescan;                // declarations only: skip bodies, initializers and other expressions
    int strict_types;           // > 0: type names must be known (used while speculating)
    int switch_depth;           // > 0: inside a switch body

    // A failure while speculating unwinds to the innermost attempt instead of exiting
    jmp_buf* spec_jmp[PSNAPSHOT_MAX]; int spec_depth;

    // Names declared so far, innermost last. A scope is a length to truncate back to.
    ParserName* names; size_t names_len; size_t names_cap;

    // Names declared elsewhere (other files of the namespace, imports, C headers)
    void* env;
    int (*lookup_type)(void* env, const Token* segments, int count);
    int (*lookup_dotop)(void* env, Token name, int* left_bp, int* right_bp);
    // is this name (or `.member`) declared somewhere with generic parameters?
    int (*lookup_generic)(void* env, const Token* segments, int count, int member);

    int const_args;             // constant generic arguments parsed so far

    // When set, collects the expressions that may go through the C preprocessor
    // as constants (generic arguments, array sizes, enumerator values), so a
    // file's constants can be preprocessed together (load.h)
    List* constants;

    // Syntax errors end the external declaration they are in; parsing goes on
    // with the next one. Without a recovery point an error exits.
    jmp_buf* recover;
    int errors;
    int silent;                 // skipping after an error: do not report again
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
    E_RETURN,

    // Expressions
    E_MEMBER,           // left . name / left -> name (token is the operator), generics = type arguments
    E_PATH,             // a::b::c, blockstmt = E_IDENT segments, generics = type arguments
    E_CAST,             // (type) left
    E_SIZEOF_TYPE,      // sizeof(type) / _Alignof(type), token is the keyword
    E_COMPOUND,         // (type) left, left = E_INIT_LIST
    E_INIT_LIST,        // blockstmt = initializers or E_INIT_ITEM
    E_INIT_ITEM,        // left = E_DESIG chain, right = initializer
    E_DESIG,            // [left] or .name, right = next designator
    E_TYPEARG,          // a type passed as a call argument (macros such as va_arg)
    E_RAW,              // raw C block
    E_LIST,             // blockstmt = items

    // Statements
    E_INCLUDE,
    E_STATIC_ASSERT,    // left = condition, right = message
    E_LABEL,            // name: right
    E_GOTO,             // left = E_IDENT label
    E_DEFAULT,          // default: right

    // Declarations
    E_DECLGROUP,        // type = shared specifiers, blockstmt = E_DECL, flags = storage / pub
    E_DECL,             // name, type, left = initializer, generics, aux = packed @prec
    E_FUNCDEF,          // name, type, generics, right = body block
    E_PARAM,            // name (may be empty), type
    E_FIELD,            // name (may be empty), type, left = bit width, right = specifiers
    E_ENUMERATOR,       // name, left = value
    E_GTYPEDEF,         // typedef<...>: name, generics, type, left = specifiers
    E_GENERIC_PARAM,    // name, blockstmt = trait bounds (E_TY_NAME); type = declared type of a constant parameter
    E_TRAIT,            // name, generics, blockstmt = E_DECL prototypes
    E_IMPL,             // generics, left = trait (E_TY_NAME) or NULL, type = target, blockstmt = E_FUNCDEF
    E_IMPORT,           // blockstmt = E_IDENT path, name = local binding
    E_FILE,             // blockstmt = external declarations

    // Type syntax
    E_TY_BASE,          // builtin type, aux = B_* mask
    E_TY_NAME,          // blockstmt = E_IDENT path, left = E_LIST type arguments
    E_TY_PTR,           // left = pointee, flags = qualifiers
    E_TY_ARRAY,         // left = element, right = size
    E_TY_FUNC,          // left = return type, blockstmt = E_PARAM
    E_TY_STRUCT,        // name = tag, blockstmt = members (struct or union)
    E_TY_ENUM,          // name = tag, blockstmt = E_ENUMERATOR
    E_CT_TYPE,          // @type(left), or @Type without left (comptime.h)
    E_TY_HOLE,          // parenthesized declarator: left = the type wrapped around it
    E_ALIGNAS,          // type or left, right = next

    // Compile-time code (comptime.h)
    E_EMIT,             // @emit left
    E_GENERATOR,        // a generator started at file scope: left = the call, generics = the parameters after `for`
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
    "E_RETURN",
    "E_MEMBER", "E_PATH", "E_CAST", "E_SIZEOF_TYPE", "E_COMPOUND",
    "E_INIT_LIST", "E_INIT_ITEM", "E_DESIG", "E_TYPEARG", "E_RAW", "E_LIST",
    "E_INCLUDE", "E_STATIC_ASSERT", "E_LABEL", "E_GOTO", "E_DEFAULT",
    "E_DECLGROUP", "E_DECL", "E_FUNCDEF", "E_PARAM", "E_FIELD", "E_ENUMERATOR",
    "E_GTYPEDEF", "E_GENERIC_PARAM", "E_TRAIT", "E_IMPL", "E_IMPORT", "E_FILE",
    "E_TY_BASE", "E_TY_NAME", "E_TY_PTR", "E_TY_ARRAY", "E_TY_FUNC",
    "E_TY_STRUCT", "E_TY_ENUM", "E_CT_TYPE", "E_TY_HOLE", "E_ALIGNAS",
    "E_EMIT", "E_GENERATOR"
};

// Expr.flags
enum {
    F_TYPEDEF    = 1 << 0,
    F_EXTERN     = 1 << 1,
    F_STATIC     = 1 << 2,
    F_AUTO       = 1 << 3,
    F_REGISTER   = 1 << 4,
    F_THREAD     = 1 << 5,
    F_INLINE     = 1 << 6,
    F_NORETURN   = 1 << 7,
    F_PUB        = 1 << 8,
    F_CONST      = 1 << 9,
    F_VOLATILE   = 1 << 10,
    F_RESTRICT   = 1 << 11,
    F_ATOMIC     = 1 << 12,
    F_PAREN      = 1 << 13,     // expression was written in parentheses
    F_HAS_BODY   = 1 << 14,     // struct / union / enum with a member list
    F_UNION      = 1 << 15,
    F_VARIADIC   = 1 << 16,     // parameter list ends in ...
    F_VOIDPARAMS = 1 << 17,     // parameter list is (void)
    F_ARR_STATIC = 1 << 18,     // [static n]
    F_ARR_STAR   = 1 << 19,     // [*]
    F_COMPTIME   = 1 << 20,     // @type: a compile-time variable (comptime.h)
};
#define F_STORAGE (F_TYPEDEF | F_EXTERN | F_STATIC | F_AUTO | F_REGISTER | F_THREAD | F_INLINE | F_NORETURN)
#define F_QUALS   (F_CONST | F_VOLATILE | F_RESTRICT | F_ATOMIC)

// E_TY_BASE.aux
enum {
    B_VOID = 1 << 0, B_CHAR = 1 << 1, B_SHORT = 1 << 2, B_INT = 1 << 3,
    B_LONG = 1 << 4, B_LONGLONG = 1 << 5, B_FLOAT = 1 << 6, B_DOUBLE = 1 << 7,
    B_SIGNED = 1 << 8, B_UNSIGNED = 1 << 9, B_BOOL = 1 << 10, B_COMPLEX = 1 << 11,
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

    Token name;             // declared / accessed name, when the kind has one
    int flags;              // F_*
    int aux;                // kind specific number
    struct Expr* type;      // type syntax, when the kind has one
    struct Expr* generics;  // E_LIST of generic parameters or type arguments
    void* sema;             // filled in by the compiler
    Token source;           // original expression, before lowering or macro expansion
    const char* expanded;   // cached preprocessing of source, independent of generic bindings

    // Comments and blank lines of the source, kept for the generated C.
    // Only items of a list (statements, members, declarations) carry them.
    const char* lead;  size_t lead_len;     // before the item
    const char* trail; size_t trail_len;    // comments finishing the item's last line
    const char* inner; size_t inner_len;    // before the closing brace of a block / member list
} Expr;




static inline Parser parser_new(const char* source) {
    Lexer lex = lexer_new(source);
    Token token = lexer_next(&lex);
    return (Parser) {
        .lexer = lex,
        .token = token
    };
}

// A parser for preprocessed C (headers, macro bodies): it skips compiler noise,
// recovers per declaration, and reads Typed C's own keywords as names
static inline Parser parser_new_header(const char* source) {
    Lexer lex = lexer_new(source);
    lex.c_only = 1;
    Token token = lexer_next(&lex);
    return (Parser) {
        .lexer = lex,
        .token = token,
        .header_mode = 1
    };
}

static inline const char* token_describe(Token tok, char* buffer, size_t size) {
    if (tok.kind == TK_EOF) return "end of file";
    size_t n = tok.length;
    const char* nl = (const char*)memchr(tok.start, '\n', n);
    if (nl) n = (size_t)(nl - tok.start);
    if (n > 40) n = 40;
    snprintf(buffer, size, "'%.*s%s'", (int)n, tok.start, n < tok.length ? "..." : "");
    return buffer;
}

// Report a syntax error. While speculating this unwinds to the attempt instead.
static inline _Noreturn void parser_fail(Parser* p, Token tok, const char* fmt, ...) {
    if (p->spec_depth > 0)
        longjmp(*p->spec_jmp[--p->spec_depth], 1);
    if (!p->silent) {
        fprintf(stderr, "%s:%zu:%zu: error: ", p->filename ? p->filename : "<input>", tok.row, tok.col);
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fputc('\n', stderr);
        p->errors++;
    }
    if (p->recover) longjmp(*p->recover, 1);
    exit(1);
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
    char buffer[64];
    Token tok = p->token;
    switch (e) {
        case PERR_OPEN_CBRACK:
            parser_fail(p, tok, "expected '}' before %s", token_describe(tok, buffer, sizeof buffer));
        case PERR_REC_FAIL:
            parser_fail(p, tok, "could not parse the construct ending at %s", token_describe(tok, buffer, sizeof buffer));
        default:
            parser_fail(p, tok, "unexpected %s", token_describe(tok, buffer, sizeof buffer));
    }
}


static inline void print_token(const char* label, Token tok, int depth) {
    if (tok.start && tok.length)
        printf("%*s%s: %.*s\n", depth * 2, "", label, (int)tok.length, tok.start);
}

static inline void print_expr(Expr* expr, int depth) {
    assert(expr);
    Token tok = expr->token;
    size_t shown = tok.start ? tok.length : 0;
    const char* nl = shown ? (const char*)memchr(tok.start, '\n', shown) : NULL;
    if (nl) shown = (size_t)(nl - tok.start);
    printf("%*s%s \"%.*s\" [%s]\n",
           depth * 2, "", ExprKind_map[expr->kind],
           (int)shown, shown ? tok.start : "",
           TokenKind_name_map[tok.kind]);

    print_token("Name", expr->name, depth + 1);
    if (expr->flags) printf("%*sFlags: 0x%x\n", (depth + 1) * 2, "", expr->flags);
    if (expr->aux)   printf("%*sAux: %d\n", (depth + 1) * 2, "", expr->aux);

    if (expr->generics) {
        printf("%*sGenerics:\n", (depth + 1) * 2, "");
        print_expr(expr->generics, depth + 2);
    }
    if (expr->type) {
        printf("%*sType:\n", (depth + 1) * 2, "");
        print_expr(expr->type, depth + 2);
    }

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

    // a field's `right` points back at specifiers already printed through `type`
    if (expr->left) {
        printf("%*sLeft:\n", (depth + 1) * 2, "");
        print_expr(expr->left, depth + 2);
    }
    if (expr->right && expr->kind != E_FIELD) {
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

// Append to the node's statement / child list
static inline void expr_push(Expr* list, Expr* item) {
    if (list->blockstmt_size >= list->alloc_blockstmt_size) {
        size_t capacity = list->alloc_blockstmt_size ? list->alloc_blockstmt_size * 2 : 8;
        Expr** items = (Expr**)realloc(list->blockstmt, capacity * sizeof(*items));
        assert(items);
        list->blockstmt = items;
        list->alloc_blockstmt_size = capacity;
    }
    list->blockstmt[list->blockstmt_size++] = item;
}

// Only for nodes that own everything below them (declaration nodes share their
// specifiers between declarators and are never freed).
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
    assert(p->snapshot_idx < PSNAPSHOT_MAX);
    p->snapshot[p->snapshot_idx] = (ParserSnapshot){p->lexer, p->token};
    p->snapshot_idx++;
}

static inline void parser_pop_snapshot(Parser* p) {
    assert(p->snapshot_idx > 0);
    ParserSnapshot snapshot = p->snapshot[--p->snapshot_idx];

    p->lexer = snapshot.lexer;
    p->token = snapshot.token;
}

static inline void parser_discard_snapshot(Parser* p) {
    assert(p->snapshot_idx > 0);
    --p->snapshot_idx;
}

// How much of the blank space after an item still belongs to the item: comments
// that finish on the line the item ends on.
static inline size_t trivia_trailing(const char* s, size_t n) {
    size_t i = 0, end = 0;
    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            return i;
        }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) {
                if (s[j] == '\n') return end;   // spans lines: it introduces what follows
                j++;
            }
            i = end = j + 2 < n ? j + 2 : n;
            continue;
        }
        return end;
    }
}

// Hand the comments in front of `start` to the items around them: `prev` gets
// what finishes its line, `item` (or the list's closing brace, through
// `container`) the rest.
static inline void parser_take_trivia(Parser* p, Expr* prev, Token start, Expr* item, Expr* container) {
    if (p->header_mode || p->prescan) return;
    const char* s = start.start - start.lead;
    size_t n = start.lead;
    size_t k = prev ? trivia_trailing(s, n) : 0;
    if (prev && k) {
        prev->trail = s;
        prev->trail_len = k;
    }
    if (item) {
        item->lead = s + k;
        item->lead_len = n - k;
    } else if (container) {
        container->inner = s + k;
        container->inner_len = n - k;
    }
}

static inline Token token_synth(const char* text, TokenKind kind, Token at) {
    at.kind = kind;
    at.lead = 0;
    at.start = text;
    at.length = strlen(text);
    return at;
}

static inline int token_kw(Token tok, const char* text) {
    return tok.kind == TK_KEYWORD && token_is(tok, text);
}

static inline int token_punct(Token tok, const char* text) {
    return tok.kind == TK_PUNCT && token_is(tok, text);
}

static inline int token_ident(Token tok, const char* text) {
    return tok.kind == TK_IDENT && token_is(tok, text);
}

// Preprocessed system headers are full of compiler extensions that carry no
// meaning for Typed C. Drop them, or map them onto the keyword they stand for.
static inline void parser_skip_noise(Parser* p) {
    static const char* const call_like[] = {
        "__attribute__", "__attribute", "__asm", "__asm__", "asm", "__declspec"
    };
    static const char* const dropped[] = {
        "__extension__", "_Nullable", "_Nonnull", "_Null_unspecified", "_Nullable_result",
        "__nullable", "__nonnull", "__null_unspecified", "__unused", "__kindof"
    };
    static const char* const mapped[][2] = {
        {"__const", "const"}, {"__const__", "const"},
        {"__signed", "signed"}, {"__signed__", "signed"},
        {"__volatile", "volatile"}, {"__volatile__", "volatile"},
        {"__inline", "inline"}, {"__inline__", "inline"},
        {"__restrict", "restrict"}, {"__restrict__", "restrict"},
        {"__alignof", "_Alignof"}, {"__alignof__", "_Alignof"},
    };
    for (;;) {
        Token tok = p->token;
        if (tok.kind == TK_DIRECTIVE || tok.kind == TK_INCLUDE) {
            p->token = lexer_next(&p->lexer);
            continue;
        }
        if (tok.kind != TK_IDENT) return;
        int again = 0;
        for (size_t i = 0; i < sizeof(call_like) / sizeof(call_like[0]) && !again; i++) {
            if (!token_is(tok, call_like[i])) continue;
            p->token = lexer_next(&p->lexer);
            if (token_punct(p->token, "(")) {
                int depth = 0;
                do {
                    if (token_punct(p->token, "(")) depth++;
                    else if (token_punct(p->token, ")")) depth--;
                    else if (p->token.kind == TK_EOF) return;
                    p->token = lexer_next(&p->lexer);
                } while (depth > 0);
            }
            again = 1;
        }
        for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]) && !again; i++) {
            if (!token_is(tok, dropped[i])) continue;
            p->token = lexer_next(&p->lexer);
            again = 1;
        }
        if (again) continue;
        for (size_t i = 0; i < sizeof(mapped) / sizeof(mapped[0]); i++) {
            if (token_is(tok, mapped[i][0])) {
                p->token = token_synth(mapped[i][1], TK_KEYWORD, tok);
                return;
            }
        }
        return;
    }
}

static inline Token parser_advance(Parser* p) {
    p->token = lexer_next(&p->lexer);
    #ifdef PARSER_TRACE
    debug("ADVANCE", p->token);
    #endif
    if (p->header_mode) {
        parser_skip_noise(p);
    } else if (p->token.kind == TK_ERR) {
        switch (p->token.err) {
            case OPEN_ML:      parser_fail(p, p->token, "unterminated comment");
            case OPEN_LITERAL: parser_fail(p, p->token, "unterminated string or character literal");
            case OPEN_RAW:     parser_fail(p, p->token, "unterminated raw C block; expected '}#'");
            default:           parser_fail(p, p->token, "unexpected character '%.*s'",
                                           (int)p->token.length, p->token.start);
        }
    }
    return p->token;
}

static inline Token parser_peek(Parser* p) {
    return p->token;
}

// The token after the current one
static inline Token parser_peek2(Parser* p) {
    Lexer lexer = p->lexer;
    return lexer_next(&lexer);
}

static inline int parser_expect(Parser* p, const char* e) {
    if (token_is(parser_peek(p), e)){
        parser_advance(p);
        return PERR_OK;
    }
    char buffer[64];
    parser_fail(p, parser_peek(p), "expected '%s' before %s", e,
                token_describe(parser_peek(p), buffer, sizeof buffer));
}

static inline Token parser_expect_ident(Parser* p, const char* what) {
    Token tok = parser_peek(p);
    if (tok.kind != TK_IDENT) {
        char buffer[64];
        parser_fail(p, tok, "expected %s before %s", what, token_describe(tok, buffer, sizeof buffer));
    }
    parser_advance(p);
    return tok;
}

// ---------------------------------------------------------------------------
// Name tracking: C cannot be parsed without knowing which identifiers are types
// ---------------------------------------------------------------------------

static inline size_t parser_scope_push(Parser* p) {
    return p->names_len;
}

static inline void parser_scope_pop(Parser* p, size_t mark) {
    p->names_len = mark;
}

static inline void parser_declare(Parser* p, Token name, int is_type) {
    if (!name.length) return;
    if (p->names_len >= p->names_cap) {
        p->names_cap = p->names_cap ? p->names_cap * 2 : 64;
        p->names = (ParserName*)realloc(p->names, p->names_cap * sizeof(*p->names));
        assert(p->names);
    }
    p->names[p->names_len++] = (ParserName){name.start, name.length, is_type};
}

// 1: a type, 0: an object, 2: a function, -1: not declared in this file so far
static inline int parser_lookup_local(Parser* p, Token name) {
    for (size_t i = p->names_len; i-- > 0;) {
        ParserName* n = &p->names[i];
        if (n->length == name.length && memcmp(n->start, name.start, name.length) == 0)
            return n->is_type;
    }
    return -1;
}

static inline int is_type_path(Parser* p, const Token* segments, int count) {
    if (count == 1) {
        int local = parser_lookup_local(p, segments[0]);
        if (local >= 0) return local == 1;
    }
    if (p->lookup_type) return p->lookup_type(p->env, segments, count);
    return 0;
}

static inline int token_is_type_keyword(Token tok) {
    static const char* const words[] = {
        "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
        "_Bool", "_Complex", "struct", "union", "enum",
        "const", "volatile", "restrict", "_Atomic"
    };
    if (tok.kind != TK_KEYWORD) return 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (token_is(tok, words[i])) return 1;
    return 0;
}

// The syntax of compile-time code (comptime.h)
static inline void ct_name(Parser* p);
static inline int at_ct_type(Parser* p);
static inline int parse_ct_mark(Parser* p);
static inline Expr* parse_ct_spec(Parser* p, int* quals);
static inline Expr* parse_ct_block_item(Parser* p);
static inline int parse_ct_member(Parser* p, Expr* member);
static inline int parse_ct_control(Parser* p, Expr* owner, int in);
static inline Expr* parse_ct_external(Parser* p);
static inline void parse_ct_skip_body(Parser* p, Expr* def);

// Does the current token start a type name? A qualified name counts only when
// the whole path names a type, so `Color::Red` stays an expression.
static inline int at_type(Parser* p) {
    Token tok = p->token;
    if (tok.kind == TK_KEYWORD) return token_is_type_keyword(tok);
    if (token_punct(tok, "@")) return at_ct_type(p);
    if (tok.kind != TK_IDENT) return 0;

    Token segments[8]; int count = 0;
    segments[count++] = tok;
    Lexer lexer = p->lexer;
    for (;;) {
        Token sep = lexer_next(&lexer);
        if (!token_punct(sep, "::")) break;
        Token next = lexer_next(&lexer);
        if (token_punct(next, "@")) next = lexer_next(&lexer);      // namespace::@Name
        if (next.kind != TK_IDENT || count >= 8) return 0;
        segments[count++] = next;
    }
    return is_type_path(p, segments, count);
}

static inline int at_declaration(Parser* p) {
    static const char* const words[] = {
        "typedef", "extern", "static", "auto", "register", "_Thread_local",
        "inline", "_Noreturn", "_Alignas", "_Static_assert"
    };
    Token tok = p->token;
    if (tok.kind == TK_KEYWORD)
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
            if (token_is(tok, words[i])) return 1;
    return at_type(p);
}

// Is the token after the current '(' the start of a type name?
static inline int paren_starts_type(Parser* p) {
    parser_push_snapshot(p);
    parser_advance(p);
    int result = at_type(p);
    parser_pop_snapshot(p);
    return result;
}

// ---------------------------------------------------------------------------
// Skipping (prescan and header recovery)
// ---------------------------------------------------------------------------

// Skip a balanced '{' ... '}' block; the current token is the '{'
static inline void parser_skip_block(Parser* p) {
    int depth = 0;
    do {
        Token tok = parser_peek(p);
        if (tok.kind == TK_EOF) parser_fail(p, tok, "expected '}' before end of file");
        if (token_punct(tok, "{")) depth++;
        else if (token_punct(tok, "}")) depth--;
        parser_advance(p);
    } while (depth > 0);
}

// Skip tokens until one of the single-character stops appears outside brackets
static inline void parser_skip_until(Parser* p, const char* stops) {
    int depth = 0;
    for (;;) {
        Token tok = parser_peek(p);
        if (tok.kind == TK_EOF) return;
        if (tok.kind == TK_PUNCT && tok.length == 1) {
            char c = tok.start[0];
            if (depth == 0 && strchr(stops, c)) return;
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') {
                if (depth == 0) return;
                depth--;
            }
        }
        parser_advance(p);
    }
}

// Header recovery: drop the rest of a declaration the parser could not read
static inline void skip_declaration(Parser* p) {
    int depth = 0;
    int after_paren = 0;
    for (;;) {
        Token tok = parser_peek(p);
        if (tok.kind == TK_EOF) return;
        if (tok.kind == TK_PUNCT && tok.length == 1) {
            char c = tok.start[0];
            if (c == '{') {
                int function_body = depth == 0 && after_paren;
                parser_skip_block(p);
                if (function_body) return;
                after_paren = 0;
                continue;
            }
            if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') { if (depth > 0) depth--; }
            else if (c == '}' && depth == 0) { parser_advance(p); return; }
            else if (c == ';' && depth == 0) { parser_advance(p); return; }
            after_paren = c == ')';
        } else {
            after_paren = 0;
        }
        parser_advance(p);
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
    P_BIF,                  // branch if [token]
    P_BIFN,                 // branch if not [token]
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
                } else {
                    char buffer[64];
                    parser_fail(p, parser_peek(p), "expected '%s' before %s", pr[i].as.text,
                                token_describe(parser_peek(p), buffer, sizeof buffer));
                }
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
            case P_END: break;
        }
    }
    return context;
}



static inline Expr* parse_if(Parser* p);
static inline void  istack_ifelse_cond(Expr* context, Expr* stack[256], size_t* idx);
static inline Expr* parse_if_cond(Parser* p);

static inline Expr* parse_for(Parser* p);
static inline Expr* parse_for_init(Parser* p);
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
static inline Expr* parse_goto(Parser* p);
static inline Expr* parse_label(Parser* p);

static inline Expr* parse_switchcase(Parser* p);
static inline Expr* parse_case(Parser* p);
static inline Expr* parse_default(Parser* p);

static inline Expr* parse_block(Parser* p);
static inline Expr* parse_block_item(Parser* p);
static inline Expr* parse_statement(Parser* p);

static inline Expr* parse_expression(Parser* p);
static inline Expr* parse_assignment_expression(Parser* p);
static inline Expr* parse_constant_expression(Parser* p);
static inline Expr* parse_pratt(Parser* p, int min_bp);
static inline Expr* parse_prefix(Parser* p);
static inline Expr* parse_postfix(Parser* p);
static inline Expr* parse_postfix_from(Parser* p, Expr* left);
static inline Expr* parse_primary(Parser* p);
static inline Expr* parse_ident_node(Parser* p);
static inline Expr* parse_initializer(Parser* p);

static inline Expr* parse_type_name(Parser* p);
static inline Expr* parse_type_args(Parser* p);
static inline Expr* parse_decl_specifiers(Parser* p, int* storage, int allow_storage);
static inline Expr* parse_struct_specifier(Parser* p);
static inline Expr* parse_struct_keyword(Parser* p);
static inline Expr* parse_tag(Parser* p);
static inline Expr* parse_struct_members(Parser* p);
static inline void  istack_tag_specifier(Expr* context, Expr* stack[256], size_t* idx);
static inline void  check_tag_specifier(Parser* p, Expr* spec);
static inline Expr* parse_enum_specifier(Parser* p);
static inline Expr* parse_enumerators(Parser* p);
static inline Expr* parse_declaration(Parser* p, int context, int pub);
static inline Expr* parse_static_assert(Parser* p);
static inline Expr* parse_static_assert_message(Parser* p);

static inline Expr* parse_trait(Parser* p, int pub);
static inline Expr* parse_trait_name(Parser* p);
static inline Expr* parse_trait_members(Parser* p);
static inline Expr* parse_trait_member(Parser* p);
static inline Expr* parse_trait_prec(Parser* p);
static inline Expr* parse_trait_prototype(Parser* p);
static inline void  istack_trait_member(Expr* context, Expr* stack[256], size_t* idx);
static inline void  istack_trait(Expr* context, Expr* stack[256], size_t* idx);

static inline Expr* parse_impl(Parser* p);
static inline Expr* parse_impl_prescan(Parser* p);
static inline Expr* parse_method(Parser* p);
static inline Expr* parse_method_definition(Parser* p);
static inline void  istack_method(Expr* context, Expr* stack[256], size_t* idx);
static inline Expr* parse_impl_methods(Parser* p);
static inline void  istack_impl(Expr* context, Expr* stack[256], size_t* idx);

static inline Expr* parse_generic_typedef(Parser* p, int pub);
static inline Expr* parse_generic_typedef_declarator(Parser* p);
static inline void  istack_generic_typedef(Expr* context, Expr* stack[256], size_t* idx);
static inline void  check_repeated_params(Parser* p, Expr* def);

static inline Expr* parse_import(Parser* p);
static inline Expr* parse_import_path(Parser* p);
static inline Expr* parse_import_alias(Parser* p);
static inline void  istack_import(Expr* context, Expr* stack[256], size_t* idx);

static inline Expr* parse_external_declaration(Parser* p);
static inline Expr* parse_translation_unit(Parser* p);


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

// First clause of a for statement: a declaration or an optional expression.
// Either way the terminating ';' is consumed here.
static inline Expr* parse_for_init(Parser* p) {
    if (at_declaration(p))
        return parse_declaration(p, 1, 0);
    Expr* expr = parse_for_cond(p);
    parser_expect(p, ";");
    return expr;
}

static inline void istack_for_cond(Expr* context, Expr* stack[256], size_t* idx) {
    (void)idx;
    context->cases = (Condition*)calloc(1, sizeof(Condition));
    context->cases->base_case = (stack[0]->kind != E_ALL) ? stack[0] : NULL;
    context->cases->condition = (stack[1]->kind != E_ALL) ? stack[1] : NULL;
    context->cases->increment = (stack[2]->kind != E_ALL) ? stack[2] : NULL;
    context->cases->block     =                             stack[3];
    context->case_size        = 1;
}

static const ParseRule for_rule[] = {
    TOK("for", E_FOR),
    POP, // Pops the "for" token from stack into context

    // (...;...;...)
    EXPECT("("),
        REC(parse_for_init),
        // ";" consumed in parse_for_init
        REC(parse_for_cond),
        EXPECT(";"),
        REC(parse_for_cond),
    EXPECT(")"),

    REC(parse_statement),

    ISTACK(istack_for_cond),
    END
};

static inline Expr* parse_for(Parser* p){
    // a declaration in the first clause is scoped to the loop
    size_t scope = parser_scope_push(p);
    Expr* expr = parse_rule(p, for_rule);
    parser_scope_pop(p, scope);
    return expr;
}


// END For statement

// While statement

static inline Expr* parse_while_cond(Parser* p) {
    Expr* expr = parse_expression(p);
    return expr_new(E_COND, expr->token, expr, NULL);
}

static inline void istack_while_cond(Expr* context, Expr* stack[256], size_t* idx) {
    (void)idx;
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
    (void)idx;
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
//
// A switch body is any statement; `case` and `default` are labels on statements.
// E_SWITCH: cases[0] = { condition, block }
// E_CASE:   left = constant expression, right = labeled statement
// E_DEFAULT: right = labeled statement

static const ParseRule switch_rule[] = {
    TOK("switch", E_SWITCH),
    POP,
    EXPECT("("),
    REC(parse_while_cond), // TODO: create general single condition function
    EXPECT(")"),
    REC(parse_statement),
    ISTACK(istack_while_cond),
    END,
};

static const ParseRule case_rule[] = {
    TOK("case", E_CASE),
    POP,
    REC(parse_constant_expression),
    EXPECT(":"),
    REC(parse_statement),
    RIGHT,
    LEFT,
    END,
};

static const ParseRule default_rule[] = {
    TOK("default", E_DEFAULT),
    POP,
    EXPECT(":"),
    REC(parse_statement),
    RIGHT,
    END,
};

static inline Expr* parse_switchcase(Parser* p){
    p->switch_depth++;
    Expr* switch_e = parse_rule(p, switch_rule);
    p->switch_depth--;
    return switch_e;
}

static inline Expr* parse_case(Parser* p){
    if (!p->switch_depth)
        parser_fail(p, parser_peek(p), "'case' label outside of a switch statement");
    return parse_rule(p, case_rule);
}

static inline Expr* parse_default(Parser* p){
    if (!p->switch_depth)
        parser_fail(p, parser_peek(p), "'default' label outside of a switch statement");
    return parse_rule(p, default_rule);
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

// goto and labels
static const ParseRule goto_rule[] = {
    TOK("goto", E_GOTO),
    POP,
    REC(parse_ident_node),
    LEFT,
    EXPECT(";"),
    END
};
static inline Expr* parse_goto(Parser* p){
    return parse_rule(p, goto_rule);
}

static inline Expr* parse_label(Parser* p){
    Token name = parser_peek(p);
    parser_advance(p);
    parser_expect(p, ":");
    Expr* label = expr_new(E_LABEL, name, NULL, parse_statement(p));
    label->name = name;
    return label;
}
// END goto and labels

// A raw block (#{...}#) followed by one of these is an operand, not a statement
static inline int raw_continues_expression(Parser* p) {
    static const char* const infix[] = {
        "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|=",
        ".", "->", "[", "?", "==", "!=", "<", ">", "<=", ">=", "<<", ">>",
        "/", "%", "|", "^", "||", "&&", ","
    };
    Token next = parser_peek2(p);
    if (next.kind == TK_DOTOP) return 1;
    if (next.kind != TK_PUNCT) return 0;
    for (size_t i = 0; i < sizeof(infix) / sizeof(infix[0]); i++)
        if (token_is(next, infix[i])) return 1;
    return 0;
}

static inline Expr* parse_statement_inner(Parser* p);

static inline Expr* parse_statement(Parser* p) {
    Token start = parser_peek(p);
    Expr* stmt = parse_statement_inner(p);
    // a statement that is not a block item (an if / loop body) keeps its comments too
    if (stmt && !stmt->lead) parser_take_trivia(p, NULL, start, stmt, NULL);
    // ... and the comment finishing its line
    if (stmt && !stmt->trail && !p->header_mode && !p->prescan) {
        Token next = parser_peek(p);
        size_t k = trivia_trailing(next.start - next.lead, next.lead);
        if (k) {
            stmt->trail = next.start - next.lead;
            stmt->trail_len = k;
        }
    }
    return stmt;
}

static inline Expr* parse_statement_inner(Parser* p) {
    Token tok = parser_peek(p);

    if (token_punct(tok, "{"))
        return parse_block(p);

    if (tok.kind == TK_KEYWORD) {
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

        if (token_is(tok, "case"))
            return parse_case(p);

        if (token_is(tok, "default"))
            return parse_default(p);

        if (token_is(tok, "break"))
            return parse_break(p);

        if (token_is(tok, "continue"))
            return parse_continue(p);

        if (token_is(tok, "return"))
            return parse_return(p);

        if (token_is(tok, "goto"))
            return parse_goto(p);
    }

    if (tok.kind == TK_IDENT && token_punct(parser_peek2(p), ":"))
        return parse_label(p);

    if (token_punct(tok, ";")){
        parser_advance(p);
        return expr_new(E_ALL, tok, NULL, NULL);
    }

    if (tok.kind == TK_RAW && !raw_continues_expression(p)) {
        parser_advance(p);
        return expr_new(E_RAW, tok, NULL, NULL);
    }

    if (tok.kind == TK_INCLUDE) {
        parser_advance(p);
        return expr_new(E_INCLUDE, tok, NULL, NULL);
    }

    if (tok.kind == TK_DIRECTIVE)
        parser_fail(p, tok, "preprocessor directives other than #include must be inside a raw C block (#{ ... }#)");

    Expr *expr = parse_expression(p);
    if (!expr){
        set_parser_error(p, PERR_REC_FAIL);
        return NULL;
    }
    parser_expect(p, ";");

    return expr;
}

// block-item = declaration | statement | RAW_BLOCK | INCLUDE_DIRECTIVE
static inline Expr* parse_block_item(Parser* p) {
    Expr* ct = parse_ct_block_item(p);
    if (ct) return ct;
    Token tok = parser_peek(p);
    // `name:` is a label even when the name is also a type
    if (tok.kind == TK_IDENT && token_punct(parser_peek2(p), ":"))
        return parse_label(p);
    if (at_declaration(p))
        return parse_declaration(p, 1, 0);
    // `Name name` can only be a declaration, so Name was meant as a type
    if (tok.kind == TK_IDENT && parser_peek2(p).kind == TK_IDENT) {
        if (token_is(tok, "_"))
            parser_fail(p, tok, "'_' is not a type: local variables need a declared type (there is no local type inference)");
        parser_fail(p, tok, "unknown type name '%.*s'", (int)tok.length, tok.start);
    }
    return parse_statement(p);
}

static inline Expr* parse_block(Parser* p) {
    if (parser_error(p)) return NULL;
    if (!token_is(parser_peek(p), "{")) return parse_statement(p);

    Expr* block = expr_new(E_BLOCK, parser_peek(p), NULL, NULL);
    parser_advance(p);

    size_t scope = parser_scope_push(p);
    Expr* prev = NULL;
    while (!token_punct(parser_peek(p), "}")) {
        if (parser_peek(p).kind == TK_EOF)
            set_parser_error(p, PERR_OPEN_CBRACK);
        Token start = parser_peek(p);
        Expr* item = parse_block_item(p);
        parser_take_trivia(p, prev, start, item, NULL);
        expr_push(block, item);
        prev = item;
    }
    parser_take_trivia(p, prev, parser_peek(p), NULL, block);
    parser_scope_pop(p, scope);

    parser_advance(p);
    return block;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

static inline Expr* parse_expression(Parser* p) {
    return parse_pratt(p, 0);
}

static inline Expr* parse_assignment_expression(Parser* p) {
    return parse_pratt(p, BP_ASSIGN);
}

static inline Expr* parse_constant_expression(Parser* p) {
    return parse_pratt(p, BP_CONST);
}

static inline Expr* parse_pratt(Parser* p, int min_bp) {
    Token start = parser_peek(p);
    // Prefix and postfix expressions bind more tightly than any infix operator.
    Expr* left = parse_prefix(p);
    if (!left) return NULL;

    for (;;) {
        Token tok = parser_peek(p);
        InfixOp dotop = {NULL, DOTOP_LEFT_BP, DOTOP_RIGHT_BP};
        const InfixOp* op = NULL;
        if (tok.kind == TK_DOTOP) {
            // declared operators carry their own binding powers
            if (p->lookup_dotop)
                p->lookup_dotop(p->env, tok, &dotop.left_bp, &dotop.right_bp);
            op = &dotop;
        } else if (tok.kind == TK_PUNCT) {
            for (int i = 0; i < opl_size; i++) {
                if (token_is(tok, opl[i].text)) {
                    op = &opl[i];
                    break;
                }
            }
        }

        // Leave delimiters (including argument commas) for the caller.
        if (!op || op->left_bp < min_bp) break;

        if (token_is(tok, "?")) {
            parser_advance(p);                       // consume '?'

            Expr* then_expr = parse_pratt(p, 0);     // full middle expression
            parser_expect(p, ":");
            Expr* else_expr = parse_pratt(p, op->right_bp);

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
        left = expr_new(E_BINARY, tok, left, right);
    }
    if (!p->preprocessed && !p->header_mode) {
        left->source = start;
        left->source.length = (size_t)(parser_peek(p).start - start.start);
    }
    return left;
}

// The rest of `(type) { ... }` once the parenthesized type has been read
static inline Expr* parse_compound_literal(Parser* p, Token open, Expr* type) {
    Expr* literal = expr_new(E_COMPOUND, open, parse_initializer(p), NULL);
    literal->type = type;
    return literal;
}

static inline Expr* parse_prefix(Parser* p) {
    Token tok = parser_peek(p);

    if (token_kw(tok, "_Generic"))
        parser_fail(p, tok, "'_Generic' is only available inside a raw C block (#{ ... }#)");

    if (token_kw(tok, "sizeof")) {
        parser_advance(p);
        Token open = parser_peek(p);
        if (token_punct(open, "(") && paren_starts_type(p)) {
            parser_advance(p);
            Expr* type = parse_type_name(p);
            parser_expect(p, ")");
            if (token_punct(parser_peek(p), "{")) {
                // sizeof applied to a compound literal
                Expr* operand = parse_postfix_from(p, parse_compound_literal(p, open, type));
                return expr_new(E_UNARY, tok, operand, NULL);
            }
            Expr* size = expr_new(E_SIZEOF_TYPE, tok, NULL, NULL);
            size->type = type;
            return size;
        }
        return expr_new(E_UNARY, tok, parse_prefix(p), NULL);
    }

    if (token_kw(tok, "_Alignof")) {
        parser_advance(p);
        parser_expect(p, "(");
        Expr* align = expr_new(E_SIZEOF_TYPE, tok, NULL, NULL);
        align->type = parse_type_name(p);
        parser_expect(p, ")");
        return align;
    }

    // A parenthesized type name starts a cast or a compound literal
    if (token_punct(tok, "(") && paren_starts_type(p)) {
        parser_advance(p);
        Expr* type = parse_type_name(p);
        parser_expect(p, ")");
        if (token_punct(parser_peek(p), "{"))
            return parse_postfix_from(p, parse_compound_literal(p, tok, type));
        Expr* cast = expr_new(E_CAST, tok, parse_prefix(p), NULL);
        cast->type = type;
        return cast;
    }

    if (tok.kind == TK_PUNCT) {
        for (int i = 0; i < unary_size; i++) {
            if(token_is(tok, unary[i])) {
                parser_advance(p);
                return expr_new(E_UNARY, tok, parse_prefix(p), NULL);
            }
        }
    }

    return parse_postfix(p);
}

// Try to read `<type, ...>` at the current '<'. Leaves the parser untouched and
// returns NULL when the tokens do not form a list of known types, in which
// case '<' keeps its comparison meaning.
// A list holding a constant argument (`f<4>`) is only accepted when
// `allow_const` says the name before it is generic: otherwise `a < 4 > (b)`
// could never be a comparison again.
static inline Expr* try_type_args(Parser* p, int require_call, int allow_const) {
    jmp_buf attempt;
    int const_args = p->const_args;
    int snapshot = p->snapshot_idx;
    int depth = p->spec_depth;
    int strict = p->strict_types;
    size_t names = p->names_len;

    parser_push_snapshot(p);
    assert(p->spec_depth < PSNAPSHOT_MAX);
    p->spec_jmp[p->spec_depth++] = &attempt;
    if (setjmp(attempt) == 0) {
        p->strict_types++;
        Expr* args = parse_type_args(p);
        if (require_call && !token_punct(parser_peek(p), "("))
            parser_fail(p, parser_peek(p), "expected '('");
        if (p->const_args != const_args && !allow_const)
            parser_fail(p, parser_peek(p), "not a generic name");
        p->strict_types = strict;
        p->spec_depth = depth;
        parser_discard_snapshot(p);
        return args;
    }
    p->snapshot_idx = snapshot + 1;
    parser_pop_snapshot(p);
    p->spec_depth = depth;
    p->strict_types = strict;
    p->names_len = names;
    return NULL;
}

// A call argument is an expression, or a type for macros such as va_arg
static inline Expr* parse_argument(Parser* p) {
    if (at_type(p)) {
        Expr* arg = expr_new(E_TYPEARG, parser_peek(p), NULL, NULL);
        arg->type = parse_type_name(p);
        return arg;
    }
    return parse_assignment_expression(p);
}

static inline Expr* parse_postfix(Parser* p) {
    return parse_postfix_from(p, parse_primary(p));
}

static inline Expr* parse_postfix_from(Parser* p, Expr* left) {
    while (1) {
        Token tok = parser_peek(p);
        if (tok.kind != TK_PUNCT) return left;

        // Existing operand followed by ++ or --.
        if (token_is(tok, "++") || token_is(tok, "--")) {
            parser_advance(p);
            left = expr_new(E_POSTFIX, tok, left, NULL);
            continue;
        }

        if (token_is(tok, "[")) {
            parser_advance(p);
            Expr* expr = parse_expression(p);
            parser_expect(p, "]");
            left = expr_new(E_INDEX, tok, expr, left);
            continue;
        }

        if (token_is(tok, "(")) {
            parser_advance(p);

            // arguments are a chain of E_MEM nodes: left = argument, right = next
            Expr* expr = expr_new(E_MEM, parser_peek(p), NULL, NULL);
            Expr* head = expr;

            if(!token_punct(parser_peek(p), ")")) {
                head->left = parse_argument(p);
                while(token_punct(parser_peek(p), ",")) {
                    parser_advance(p);
                    Token at = parser_peek(p);
                    head->right = expr_new(E_MEM, at, parse_argument(p), NULL);
                    head = head->right;
                }
            }
            parser_expect(p, ")");
            left = expr_new(E_CALL, tok, expr, left);
            continue;
        }

        if (token_is(tok, ".") || token_is(tok, "->")) {
            parser_advance(p);
            Expr* member = expr_new(E_MEMBER, tok, left, NULL);
            if (!parse_ct_member(p, member)) member->name = parser_expect_ident(p, "a member name");
            // value.method<T>(args): type arguments need the call right behind them
            if (token_is(tok, ".") && token_punct(parser_peek(p), "<"))
                member->generics = try_type_args(p, 1,
                    p->lookup_generic && p->lookup_generic(p->env, &member->name, 1, 1));
            left = member;
            continue;
        }

        return left;
    }
}

static inline Expr* parse_ident_node(Parser* p) {
    Token tok = parser_expect_ident(p, "an identifier");
    return expr_new(E_IDENT, tok, NULL, NULL);
}

static inline Expr* parse_primary(Parser* p) {
    ct_name(p);
    Token tok = parser_peek(p);

    if (token_punct(tok, "(")) {
        parser_advance(p);
        Expr* expr = parse_expression(p);
        parser_expect(p, ")");
        // remembered so the generated C keeps the written grouping
        expr->flags |= F_PAREN;
        return expr;
    }

    switch (tok.kind) {
        case TK_IDENT: {
            parser_advance(p);
            Expr* expr = expr_new(E_IDENT, tok, NULL, NULL);
            if (token_punct(parser_peek(p), "::")) {
                // qualified name: namespace::symbol, Enum::Constant, namespace::Enum::Constant
                expr->kind = E_PATH;
                expr_push(expr, expr_new(E_IDENT, tok, NULL, NULL));
                while (token_punct(parser_peek(p), "::")) {
                    parser_advance(p);
                    ct_name(p);
                    Token segment = parser_expect_ident(p, "a name after '::'");
                    expr_push(expr, expr_new(E_IDENT, segment, NULL, NULL));
                }
            }
            // A complete list of known types after a name selects a specialization,
            // unless the name is a local object, which cannot be generic.
            if (token_punct(parser_peek(p), "<") &&
                !(expr->kind == E_IDENT && parser_lookup_local(p, tok) == 0)) {
                Token segments[8];
                int count = 0;
                if (expr->kind == E_IDENT) segments[count++] = tok;
                for (size_t i = 0; i < expr->blockstmt_size && count < 8; i++)
                    segments[count++] = expr->blockstmt[i]->token;
                expr->generics = try_type_args(p, 0, *segments[count - 1].start == '@' ||    // an '@' function
                    (p->lookup_generic && p->lookup_generic(p->env, segments, count, 0)));
            }
            return expr;
        }
        case TK_STR: {
            // adjacent string literals form one literal
            parser_advance(p);
            while (parser_peek(p).kind == TK_STR) {
                Token next = parser_peek(p);
                tok.length = (size_t)(next.start + next.length - tok.start);
                parser_advance(p);
            }
            return expr_new(E_IDENT, tok, NULL, NULL);
        }
        case TK_PP_NUM:
        case TK_NUM:
        case TK_CHAR:
            parser_advance(p);
            return expr_new(E_IDENT, tok, NULL, NULL);
        case TK_RAW:
            parser_advance(p);
            return expr_new(E_RAW, tok, NULL, NULL);
        default: {
            char buffer[64];
            parser_fail(p, tok, "expected an expression before %s",
                        token_describe(tok, buffer, sizeof buffer));
        }
    }
}

// initializer = assignment-expression | "{" initializer-list [ "," ] "}"
static inline Expr* parse_initializer(Parser* p) {
    Token tok = parser_peek(p);
    if (!token_punct(tok, "{"))
        return parse_assignment_expression(p);

    Expr* list = expr_new(E_INIT_LIST, tok, NULL, NULL);
    parser_advance(p);
    while (!token_punct(parser_peek(p), "}")) {
        Token at = parser_peek(p);
        if (token_punct(at, "[") || token_punct(at, ".")) {
            Expr* item = expr_new(E_INIT_ITEM, at, NULL, NULL);
            Expr** link = &item->left;
            while (token_punct(parser_peek(p), "[") || token_punct(parser_peek(p), ".")) {
                Token d = parser_peek(p);
                parser_advance(p);
                Expr* desig = expr_new(E_DESIG, d, NULL, NULL);
                if (token_is(d, "[")) {
                    desig->left = parse_constant_expression(p);
                    parser_expect(p, "]");
                } else if (!parse_ct_member(p, desig)) {
                    desig->name = parser_expect_ident(p, "a member name");
                }
                *link = desig;
                link = &desig->right;
            }
            parser_expect(p, "=");
            item->right = parse_initializer(p);
            expr_push(list, item);
        } else {
            expr_push(list, parse_initializer(p));
        }
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
    }
    parser_expect(p, "}");
    return list;
}

// ---------------------------------------------------------------------------
// Types and declarators
// ---------------------------------------------------------------------------

// Declarator modes
enum {
    DM_NAMED    = 0,        // a name is required
    DM_ABSTRACT = 1,        // no name allowed (type names)
    DM_MAYBE    = 2,        // either (parameters)
    DM_KIND     = 3,
    DM_GENERIC  = 1 << 2,   // name<T, ...> may introduce generic parameters
    DM_METHOD   = 1 << 3,   // the name may be a dotted operator
    DM_GTYPEDEF = 1 << 4,   // name<T, ...> repeats a generic typedef's parameters
};

typedef struct {
    Token name;
    Expr* generics;         // generic parameters, or the repeated names of a generic typedef
    int has_scope;          // generic parameters were declared: pop back to scope when done
    size_t scope;
} DeclInfo;

// Skip parentheses around a declarator's own type constructor
static inline Expr* type_skip_holes(Expr* type) {
    while (type && type->kind == E_TY_HOLE) type = type->left;
    return type;
}

// The parameter list of a declarator that declares a function, else NULL
static inline Expr* decl_function_type(Expr* type) {
    type = type_skip_holes(type);
    return type && type->kind == E_TY_FUNC ? type : NULL;
}

static inline int token_qualifier(Token tok) {
    if (tok.kind != TK_KEYWORD) return 0;
    if (token_is(tok, "const")) return F_CONST;
    if (token_is(tok, "volatile")) return F_VOLATILE;
    if (token_is(tok, "restrict")) return F_RESTRICT;
    if (token_is(tok, "_Atomic")) return F_ATOMIC;
    return 0;
}

static inline int token_storage(Token tok) {
    if (tok.kind != TK_KEYWORD) return 0;
    if (token_is(tok, "typedef")) return F_TYPEDEF;
    if (token_is(tok, "extern")) return F_EXTERN;
    if (token_is(tok, "static")) return F_STATIC;
    if (token_is(tok, "auto")) return F_AUTO;
    if (token_is(tok, "register")) return F_REGISTER;
    if (token_is(tok, "_Thread_local")) return F_THREAD;
    if (token_is(tok, "inline")) return F_INLINE;
    if (token_is(tok, "_Noreturn")) return F_NORETURN;
    return 0;
}

static inline int token_builtin(Token tok) {
    if (tok.kind != TK_KEYWORD) return 0;
    if (token_is(tok, "void")) return B_VOID;
    if (token_is(tok, "char")) return B_CHAR;
    if (token_is(tok, "short")) return B_SHORT;
    if (token_is(tok, "int")) return B_INT;
    if (token_is(tok, "long")) return B_LONG;
    if (token_is(tok, "float")) return B_FLOAT;
    if (token_is(tok, "double")) return B_DOUBLE;
    if (token_is(tok, "signed")) return B_SIGNED;
    if (token_is(tok, "unsigned")) return B_UNSIGNED;
    if (token_is(tok, "_Bool")) return B_BOOL;
    if (token_is(tok, "_Complex")) return B_COMPLEX;
    return 0;
}

// Close a `<...>` list. A `>>` closing two nested lists is split in place.
static inline void expect_close_angle(Parser* p) {
    Token tok = parser_peek(p);
    if (token_punct(tok, ">")) {
        parser_advance(p);
    } else if (token_punct(tok, ">>")) {
        p->token.start++;
        p->token.length--;
        p->token.col++;
    } else {
        char buffer[64];
        parser_fail(p, tok, "expected '>' before %s", token_describe(tok, buffer, sizeof buffer));
    }
}

// Binding power of a constant generic argument: additive and tighter, so that
// `>` and `>>` always close the list. Anything looser needs parentheses.
#define BP_GENERIC_ARG 22

// generic-argument = type-name | constant-expression (additive level)
// A bare name that is not known as a type yet (a generic parameter used before
// the list that declares it) is kept as an expression; the compiler decides
// from the parameter it is passed for.
static inline Expr* parse_generic_arg(Parser* p) {
    if (at_type(p)) return parse_type_name(p);
    if (p->prescan) {
        Expr* skipped = expr_new(E_ALL, parser_peek(p), NULL, NULL);
        for (int depth = 0;;) {
            Token tok = parser_peek(p);
            if (tok.kind == TK_EOF) break;
            if (tok.kind == TK_PUNCT) {
                if (depth == 0 && (token_is(tok, ",") || token_is(tok, ">") || token_is(tok, ">>"))) break;
                if (token_is(tok, "(") || token_is(tok, "[") || token_is(tok, "{")) depth++;
                else if (token_is(tok, ")") || token_is(tok, "]") || token_is(tok, "}")) {
                    if (depth == 0) break;
                    depth--;
                }
            }
            parser_advance(p);
        }
        return skipped;
    }

    jmp_buf attempt;
    int snapshot = p->snapshot_idx;
    int depth = p->spec_depth;
    int strict = p->strict_types;
    size_t names = p->names_len;
    parser_push_snapshot(p);
    assert(p->spec_depth < PSNAPSHOT_MAX);
    p->spec_jmp[p->spec_depth++] = &attempt;
    if (setjmp(attempt) == 0) {
        Expr* value = parse_pratt(p, BP_GENERIC_ARG);
        Token next = parser_peek(p);
        if (!token_punct(next, ",") && !token_punct(next, ">") && !token_punct(next, ">>"))
            parser_fail(p, next, "expected ',' or '>'");
        p->spec_depth = depth;
        parser_discard_snapshot(p);
        p->const_args++;
        return value;
    }
    // not an expression after all: a type built on a name declared later (`T*`)
    p->snapshot_idx = snapshot + 1;
    parser_pop_snapshot(p);
    p->spec_depth = depth;
    p->strict_types = strict;
    p->names_len = names;
    return parse_type_name(p);
}

// type-arguments = "<" generic-argument { "," generic-argument } ">"
static inline Expr* parse_type_args(Parser* p) {
    Expr* args = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    parser_expect(p, "<");
    for (;;) {
        Token start = parser_peek(p);
        Expr* arg = parse_generic_arg(p);
        if (!p->preprocessed && !p->header_mode) {
            arg->source = start;
            arg->source.length = (size_t)(parser_peek(p).start - start.start);
            // a type is never a constant
            if (p->constants && !(arg->kind >= E_TY_BASE && arg->kind <= E_TY_HOLE))
                list_push(p->constants, arg);
        }
        expr_push(args, arg);
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
    }
    expect_close_angle(p);
    return args;
}

// typedef-name = qualified-name [ type-arguments ]
static inline Expr* parse_typedef_name(Parser* p) {
    Token tok = parser_expect_ident(p, "a type name");
    Expr* name = expr_new(E_TY_NAME, tok, NULL, NULL);
    expr_push(name, expr_new(E_IDENT, tok, NULL, NULL));
    while (token_punct(parser_peek(p), "::")) {
        parser_advance(p);
        name->flags |= parse_ct_mark(p);
        Token segment = parser_expect_ident(p, "a name after '::'");
        expr_push(name, expr_new(E_IDENT, segment, NULL, NULL));
    }
    if (token_punct(parser_peek(p), "<"))
        name->left = parse_type_args(p);
    return name;
}

// generic-parameters = "<" generic-parameter { "," generic-parameter } ">"
// generic-parameter  = IDENT [ ":" bound { "+" bound } ]      (a type)
//                    | specifier-qualifier-list IDENT          (a constant)
// Type parameters become type names in the current scope, constants objects.
static inline Expr* parse_generic_params(Parser* p) {
    Expr* params = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    parser_expect(p, "<");
    for (;;) {
        // a lone name is a type parameter; anything written like a C
        // declaration (`size_t N`) is a constant parameter
        Token after = parser_peek2(p);
        int is_type = parser_peek(p).kind == TK_IDENT &&
                      (token_punct(after, ",") || token_punct(after, ">") ||
                       token_punct(after, ">>") || token_punct(after, ":"));
        Expr* const_type = is_type ? NULL : parse_decl_specifiers(p, NULL, 0);
        Token name = parser_expect_ident(p, "a generic parameter name");
        for (size_t i = 0; i < params->blockstmt_size; i++)
            if (token_same(params->blockstmt[i]->name, name))
                parser_fail(p, name, "duplicate generic parameter '%.*s'", (int)name.length, name.start);
        Expr* param = expr_new(E_GENERIC_PARAM, name, NULL, NULL);
        param->name = name;
        param->type = const_type;
        if (const_type && token_punct(parser_peek(p), ":"))
            parser_fail(p, parser_peek(p), "constant parameter '%.*s' cannot have trait bounds", (int)name.length, name.start);
        if (token_punct(parser_peek(p), ":")) {
            do {
                parser_advance(p);
                expr_push(param, parse_typedef_name(p));
            } while (token_punct(parser_peek(p), "+"));
        }
        expr_push(params, param);
        parser_declare(p, name, const_type ? 0 : 1);
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
    }
    expect_close_angle(p);
    return params;
}

static inline Expr* parse_declarator(Parser* p, Expr* base, DeclInfo* info, int mode);

// declaration-specifiers / specifier-qualifier-list. Returns the type the
// specifiers denote; storage classes and function specifiers go to *storage.
static inline Expr* parse_decl_specifiers(Parser* p, int* storage, int allow_storage) {
    Token first = parser_peek(p);
    int flags = 0, quals = 0, mask = 0;
    Expr* spec = NULL;
    Expr* align = NULL;
    Expr** align_link = &align;

    for (;;) {
        Token tok = parser_peek(p);
        if (tok.kind == TK_KEYWORD) {
            int s = token_storage(tok);
            if (s) {
                if (!allow_storage)
                    parser_fail(p, tok, "'%.*s' is not allowed here", (int)tok.length, tok.start);
                flags |= s;
                parser_advance(p);
                continue;
            }
            int q = token_qualifier(tok);
            if (q == F_ATOMIC && token_punct(parser_peek2(p), "(")) {
                // _Atomic(type) is a type specifier
                if (spec || mask) parser_fail(p, tok, "cannot combine '_Atomic(...)' with another type");
                parser_advance(p);
                parser_advance(p);
                spec = parse_type_name(p);
                parser_expect(p, ")");
                spec->flags |= F_ATOMIC;
                continue;
            }
            if (q) {
                quals |= q;
                parser_advance(p);
                continue;
            }
            if (token_is(tok, "_Alignas")) {
                parser_advance(p);
                Expr* a = expr_new(E_ALIGNAS, tok, NULL, NULL);
                if (p->prescan) {
                    parser_expect(p, "(");
                    parser_skip_until(p, ")");
                    parser_expect(p, ")");
                } else {
                    parser_expect(p, "(");
                    if (at_type(p)) a->type = parse_type_name(p);
                    else a->left = parse_constant_expression(p);
                    parser_expect(p, ")");
                }
                *align_link = a;
                align_link = &a->right;
                continue;
            }
            int b = token_builtin(tok);
            if (b) {
                if (spec) parser_fail(p, tok, "cannot combine '%.*s' with another type", (int)tok.length, tok.start);
                if (b == B_LONG && (mask & B_LONG)) b = B_LONGLONG;
                mask |= b;
                parser_advance(p);
                continue;
            }
            if (token_is(tok, "struct") || token_is(tok, "union") || token_is(tok, "enum")) {
                if (spec || mask) parser_fail(p, tok, "cannot combine '%.*s' with another type", (int)tok.length, tok.start);
                spec = token_is(tok, "enum") ? parse_enum_specifier(p) : parse_struct_specifier(p);
                continue;
            }
            break;
        }
        if (token_punct(tok, "@") && !spec && !mask) {
            spec = parse_ct_spec(p, &quals);
            continue;
        }
        // The first identifier of a declaration is its type; a later one is the
        // name being declared.
        if (tok.kind == TK_IDENT && !spec && !mask) {
            if (p->strict_types && !at_type(p))
                parser_fail(p, tok, "unknown type name '%.*s'", (int)tok.length, tok.start);
            spec = parse_typedef_name(p);
            continue;
        }
        break;
    }

    if (!spec) {
        if (!mask) {
            char buffer[64];
            parser_fail(p, parser_peek(p), "expected a type before %s",
                        token_describe(parser_peek(p), buffer, sizeof buffer));
        }
        spec = expr_new(E_TY_BASE, first, NULL, NULL);
        spec->aux = mask;
    }
    spec->flags |= quals;
    if (align) {
        if (spec->kind == E_TY_PTR || spec->kind == E_TY_ARRAY || spec->kind == E_TY_FUNC)
            parser_fail(p, align->token, "'_Alignas' cannot be combined with this type");
        spec->right = align;
    }
    if (storage) *storage = flags;
    return spec;
}

// struct-member list entry: fields share one specifier list
static inline void parse_struct_member(Parser* p, Expr* owner) {
    if (parse_ct_control(p, owner, E_TY_STRUCT)) return;
    Token tok = parser_peek(p);
    if (tok.kind == TK_RAW) {
        parser_advance(p);
        expr_push(owner, expr_new(E_RAW, tok, NULL, NULL));
        return;
    }
    if (tok.kind == TK_INCLUDE) {
        parser_advance(p);
        expr_push(owner, expr_new(E_INCLUDE, tok, NULL, NULL));
        return;
    }
    if (token_kw(tok, "_Static_assert")) {
        expr_push(owner, parse_static_assert(p));
        return;
    }

    Expr* specs = parse_decl_specifiers(p, NULL, 0);
    if (token_punct(parser_peek(p), ";")) {
        // anonymous struct / union member
        Expr* field = expr_new(E_FIELD, tok, NULL, specs);
        field->type = specs;
        expr_push(owner, field);
        parser_advance(p);
        return;
    }
    for (;;) {
        Expr* field = expr_new(E_FIELD, parser_peek(p), NULL, specs);
        int has_width = token_punct(parser_peek(p), ":");
        if (has_width) {
            field->type = specs;    // unnamed bit-field
        } else {
            DeclInfo info = {0};
            field->type = parse_declarator(p, specs, &info, DM_NAMED);
            field->name = info.name;
            has_width = token_punct(parser_peek(p), ":");
        }
        if (has_width) {
            Token colon = parser_peek(p);
            parser_advance(p);
            if (p->prescan) {
                parser_skip_until(p, ",;");
                field->left = expr_new(E_ALL, colon, NULL, NULL);
            } else {
                field->left = parse_constant_expression(p);
            }
        }
        expr_push(owner, field);
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
    }
    parser_expect(p, ";");
}

// `struct geo::Node`, `enum geo::Color`: a tag of an imported namespace. The
// namespace goes to `left`, the tag stays in `name`.
static inline void parse_tag_namespace(Parser* p, Expr* spec) {
    if (!token_punct(parser_peek(p), "::")) return;
    spec->left = expr_new(E_IDENT, spec->name, NULL, NULL);
    parser_advance(p);
    spec->name = parser_expect_ident(p, "a tag name");
}

// Struct / union specifier
//
// struct-specifier = ( "struct" | "union" ) [ tag ] [ "{" { struct-member } "}" ]
// tag              = IDENT [ "::" IDENT ]
// E_TY_STRUCT: name = tag, left = namespace of the tag, blockstmt = members,
// flags = F_UNION, F_HAS_BODY

// The keyword: the E_TY_STRUCT, with F_UNION for a union
static inline Expr* parse_struct_keyword(Parser* p) {
    Token keyword = parser_peek(p);
    parser_advance(p);
    Expr* spec = expr_new(E_TY_STRUCT, keyword, NULL, NULL);
    if (token_is(keyword, "union")) spec->flags |= F_UNION;
    ct_name(p);      // the tag can be made by @ident
    return spec;
}

// The tag of a struct, union or enum: an E_IDENT with name = the tag and
// left = its namespace, or an E_ALL placeholder when there is none
static inline Expr* parse_tag(Parser* p) {
    Token tok = parser_peek(p);
    if (tok.kind != TK_IDENT) return expr_new(E_ALL, tok, NULL, NULL);
    parser_advance(p);
    Expr* tag = expr_new(E_IDENT, tok, NULL, NULL);
    tag->name = tok;
    parse_tag_namespace(p, tag);
    return tag;
}

// The members between the braces, collected in an E_LIST, which also takes the
// comments before the closing brace
static inline Expr* parse_struct_members(Parser* p) {
    Expr* members = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    Expr* prev = NULL;
    while (!token_punct(parser_peek(p), "}")) {
        if (parser_peek(p).kind == TK_EOF) set_parser_error(p, PERR_OPEN_CBRACK);
        Token start = parser_peek(p);
        size_t first = members->blockstmt_size;
        parse_struct_member(p, members);
        parser_take_trivia(p, prev, start, members->blockstmt[first], NULL);
        prev = members->blockstmt[members->blockstmt_size - 1];
    }
    parser_take_trivia(p, prev, parser_peek(p), NULL, members);
    return members;
}

// context: the specifier; stack: tag [ body ]
// The tag is an E_IDENT, or an E_ALL when there is none; the body, when there
// is one, the E_LIST of the members. Shared by structs, unions and enums.
static inline void istack_tag_specifier(Expr* context, Expr* stack[256], size_t* idx) {
    Expr* tag = stack[0];
    if (tag->kind == E_IDENT) {
        context->name = tag->name;
        context->left = tag->left;
    }
    if (*idx == 2) {
        Expr* body = stack[1];
        context->flags               |= F_HAS_BODY;
        context->blockstmt            = body->blockstmt;
        context->blockstmt_size       = body->blockstmt_size;
        context->alloc_blockstmt_size = body->alloc_blockstmt_size;
        context->inner                = body->inner;
        context->inner_len            = body->inner_len;
    }
}

static const ParseRule struct_rule[] = {
    REC(parse_struct_keyword),
    POP,
    REC(parse_tag),

    // { ... }
    BIF("{"),
        EXPECT("{"),
        REC(parse_struct_members),
        EXPECT("}"),
        POP, // put into return context
    END,

    ISTACK(istack_tag_specifier),
    END
};

// A specifier has a tag, a body or both, and a body only with a tag of this
// namespace. Shared by structs, unions and enums.
static inline void check_tag_specifier(Parser* p, Expr* spec) {
    if ((spec->flags & F_HAS_BODY) && spec->left)
        parser_fail(p, spec->name, "a tag is defined in its own namespace: remove '%.*s::'",
                    (int)spec->left->token.length, spec->left->token.start);
    if (!(spec->flags & F_HAS_BODY) && !spec->name.length)
        parser_fail(p, parser_peek(p), "expected a tag or '{' after '%.*s'", (int)spec->token.length, spec->token.start);
}

static inline Expr* parse_struct_specifier(Parser* p) {
    Expr* spec = parse_rule(p, struct_rule);
    if (spec) check_tag_specifier(p, spec);
    return spec;
}

// END Struct / union specifier

// Enum specifier
//
// enum-specifier = "enum" [ tag ] [ "{" enumerator { "," enumerator } [ "," ] "}" ]
// enumerator     = IDENT [ "=" constant-expression ]
// E_TY_ENUM: name = tag, left = namespace of the tag, blockstmt = E_ENUMERATOR

// The enumerators between the braces, collected in an E_LIST, which also takes
// the comments before the closing brace
static inline Expr* parse_enumerators(Parser* p) {
    Expr* list = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    Expr* prev = NULL;
    for (;;) {
        Token name = parser_expect_ident(p, "an enumerator name");
        Expr* item = expr_new(E_ENUMERATOR, name, NULL, NULL);
        item->name = name;
        parser_take_trivia(p, prev, name, item, NULL);
        prev = item;
        if (token_punct(parser_peek(p), "=")) {
            Token eq = parser_peek(p);
            parser_advance(p);
            if (p->prescan) {
                parser_skip_until(p, ",");
                item->left = expr_new(E_ALL, eq, NULL, NULL);
            } else {
                item->left = parse_constant_expression(p);
                if (p->constants && item->left && item->left->source.start) list_push(p->constants, item->left);
            }
        }
        // enumerators are ordinary identifiers and hide type names
        parser_declare(p, name, 0);
        expr_push(list, item);
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
        if (token_punct(parser_peek(p), "}")) break;
    }
    parser_take_trivia(p, prev, parser_peek(p), NULL, list);
    return list;
}

static const ParseRule enum_rule[] = {
    TOK("enum", E_TY_ENUM),
    POP,
    REC(parse_tag),

    // { ... }
    BIF("{"),
        EXPECT("{"),
        REC(parse_enumerators),
        EXPECT("}"),
        POP, // put into return context
    END,

    ISTACK(istack_tag_specifier),
    END
};

static inline Expr* parse_enum_specifier(Parser* p) {
    Expr* spec = parse_rule(p, enum_rule);
    if (spec) check_tag_specifier(p, spec);
    return spec;
}

// END Enum specifier

// The parameter list of a function declarator; the '(' has been consumed
static inline void parse_parameters(Parser* p, Expr* fn) {
    if (token_punct(parser_peek(p), ")")) {
        parser_advance(p);
        return;
    }
    if (token_kw(parser_peek(p), "void") && token_punct(parser_peek2(p), ")")) {
        fn->flags |= F_VOIDPARAMS;
        parser_advance(p);
        parser_advance(p);
        return;
    }
    for (;;) {
        Token tok = parser_peek(p);
        if (token_punct(tok, "...")) {
            if (!fn->blockstmt_size)
                parser_fail(p, tok, "'...' needs a named parameter before it");
            fn->flags |= F_VARIADIC;
            parser_advance(p);
            break;
        }
        int storage = 0;
        Expr* specs = parse_decl_specifiers(p, &storage, 1);
        if (storage & ~F_REGISTER)
            parser_fail(p, tok, "storage class is not allowed on a parameter");
        DeclInfo info = {0};
        Expr* param = expr_new(E_PARAM, tok, NULL, NULL);
        param->type = parse_declarator(p, specs, &info, DM_MAYBE);
        param->name = info.name;
        param->flags = storage;
        expr_push(fn, param);
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
    }
    parser_expect(p, ")");
}

// Array and function suffixes. The first suffix is the outermost constructor:
// `a[2][3]` is an array of 2 arrays of 3.
static inline Expr* parse_declarator_suffixes(Parser* p, Expr* base) {
    Token tok = parser_peek(p);
    if (token_punct(tok, "[")) {
        parser_advance(p);
        Expr* array = expr_new(E_TY_ARRAY, tok, NULL, NULL);
        for (;;) {
            Token t = parser_peek(p);
            int q = token_qualifier(t);
            if (q) array->flags |= q;
            else if (token_kw(t, "static")) array->flags |= F_ARR_STATIC;
            else break;
            parser_advance(p);
        }
        if (token_punct(parser_peek(p), "*") && token_punct(parser_peek2(p), "]")) {
            array->flags |= F_ARR_STAR;
            parser_advance(p);
        } else if (!token_punct(parser_peek(p), "]")) {
            if (p->prescan) {
                array->right = expr_new(E_ALL, parser_peek(p), NULL, NULL);
                parser_skip_until(p, "]");
            } else {
                array->right = parse_assignment_expression(p);
                if (p->constants && array->right && array->right->source.start) list_push(p->constants, array->right);
            }
        }
        parser_expect(p, "]");
        array->left = parse_declarator_suffixes(p, base);
        return array;
    }
    if (token_punct(tok, "(")) {
        parser_advance(p);
        Expr* fn = expr_new(E_TY_FUNC, tok, NULL, NULL);
        parse_parameters(p, fn);
        fn->left = parse_declarator_suffixes(p, base);
        return fn;
    }
    return base;
}

// Is the current '(' a parenthesized declarator rather than a parameter list?
static inline int paren_is_grouping(Parser* p, int mode) {
    Token next = parser_peek2(p);
    if (token_punct(next, "*") || token_punct(next, "(") || token_punct(next, "[")) return 1;
    if (next.kind == TK_DOTOP) return 1;
    if (next.kind == TK_IDENT) {
        if ((mode & DM_KIND) == DM_NAMED) return 1;
        // in a parameter or type name, `(T)` is a parameter list when T is a type
        parser_push_snapshot(p);
        parser_advance(p);
        int is_type = at_type(p);
        parser_pop_snapshot(p);
        return !is_type;
    }
    return 0;
}

// declarator / abstract-declarator: wraps `base` in the pointers, arrays and
// functions written around the declared name.
static inline Expr* parse_declarator(Parser* p, Expr* base, DeclInfo* info, int mode) {
    while (token_punct(parser_peek(p), "*")) {
        Token star = parser_peek(p);
        parser_advance(p);
        base = expr_new(E_TY_PTR, star, base, NULL);
        for (int q; (q = token_qualifier(parser_peek(p))); parser_advance(p))
            base->flags |= q;
    }

    ct_name(p);      // an '@' function, a compile-time type, or a name made by @ident
    Token tok = parser_peek(p);
    int kind = mode & DM_KIND;
    Expr* hole = NULL;
    Expr* inner = NULL;

    if (kind != DM_ABSTRACT && (tok.kind == TK_IDENT || (tok.kind == TK_DOTOP && (mode & DM_METHOD)))) {
        info->name = tok;
        parser_advance(p);
        if (tok.kind == TK_IDENT && token_punct(parser_peek(p), "<")) {
            if (mode & DM_GENERIC) {
                info->has_scope = 1;
                info->scope = parser_scope_push(p);
                info->generics = parse_generic_params(p);
            } else if (mode & DM_GTYPEDEF) {
                Expr* names = expr_new(E_LIST, parser_peek(p), NULL, NULL);
                parser_advance(p);
                for (;;) {
                    expr_push(names, parse_ident_node(p));
                    if (!token_punct(parser_peek(p), ",")) break;
                    parser_advance(p);
                }
                expect_close_angle(p);
                info->generics = names;
            } else {
                parser_fail(p, parser_peek(p), "'%.*s' cannot declare generic parameters: only functions, methods and generic typedefs can",
                            (int)tok.length, tok.start);
            }
        }
    } else if (token_punct(tok, "(") && paren_is_grouping(p, mode)) {
        parser_advance(p);
        hole = expr_new(E_TY_HOLE, tok, NULL, NULL);
        inner = parse_declarator(p, hole, info, mode);
        parser_expect(p, ")");
    } else if (kind == DM_NAMED) {
        char buffer[64];
        if (token_punct(tok, "^"))
            parser_fail(p, tok, "block pointers are not supported");
        parser_fail(p, tok, "expected a name before %s", token_describe(tok, buffer, sizeof buffer));
    }

    base = parse_declarator_suffixes(p, base);
    if (hole) {
        hole->left = base;
        return inner;
    }
    return base;
}

// type-name = specifier-qualifier-list [ abstract-declarator ]
static inline Expr* parse_type_name(Parser* p) {
    Expr* specs = parse_decl_specifiers(p, NULL, 0);
    DeclInfo info = {0};
    return parse_declarator(p, specs, &info, DM_ABSTRACT);
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

// Static assertion
//
// static-assert = "_Static_assert" "(" constant-expression "," STRING ")" ";"
// E_STATIC_ASSERT: left = condition, right = message

// The message: a string literal
static inline Expr* parse_static_assert_message(Parser* p) {
    if (parser_peek(p).kind != TK_STR)
        parser_fail(p, parser_peek(p), "expected a string literal in '_Static_assert'");
    return parse_primary(p);
}

static const ParseRule static_assert_rule[] = {
    TOK("_Static_assert", E_STATIC_ASSERT),
    POP,

    // (...)
    EXPECT("("),
    REC(parse_constant_expression),
    EXPECT(","),
    REC(parse_static_assert_message),
    EXPECT(")"),
    EXPECT(";"),

    RIGHT,
    LEFT,
    END
};

static inline Expr* parse_static_assert(Parser* p) {
    if (!p->prescan) return parse_rule(p, static_assert_rule);
    // the prescan only collects names: the condition may use names it does not know yet
    Expr* check = expr_new(E_STATIC_ASSERT, parser_peek(p), NULL, NULL);
    parser_advance(p);
    parser_expect(p, "(");
    parser_skip_until(p, ")");
    parser_expect(p, ")");
    parser_expect(p, ";");
    return check;
}

// END Static assertion

// Finish a function definition whose declarator has been read: declare the
// parameters, then parse (or skip) the body.
static inline Expr* parse_function_definition(Parser* p, Expr* type, DeclInfo* info, int flags) {
    Expr* fn = decl_function_type(type);
    if ((fn->flags & F_VARIADIC) && !p->header_mode)
        parser_fail(p, info->name, "variadic function definitions are only available inside a raw C block (#{ ... }#)");

    Expr* def = expr_new(E_FUNCDEF, info->name, NULL, NULL);
    def->name = info->name;
    def->type = type;
    def->generics = info->generics;
    def->flags = flags;

    size_t scope = info->has_scope ? info->scope : parser_scope_push(p);
    for (size_t i = 0; i < fn->blockstmt_size; i++) {
        Expr* param = fn->blockstmt[i];
        if (!param->name.length && !p->header_mode && !(fn->flags & F_VOIDPARAMS))
            parser_fail(p, param->token, "parameter %zu of '%.*s' needs a name", i + 1,
                        (int)info->name.length, info->name.start);
        parser_declare(p, param->name, 0);
    }
    int switch_depth = p->switch_depth;
    p->switch_depth = 0;
    if (p->prescan)
        parse_ct_skip_body(p, def);
    else if (p->header_mode)
        parser_skip_block(p);
    else
        def->right = parse_block(p);
    p->switch_depth = switch_depth;
    parser_scope_pop(p, scope);
    return def;
}

// declaration (context 1: block or for clause) or file-scope declaration /
// function definition (context 0)
static inline Expr* parse_declaration(Parser* p, int context, int pub) {
    Token start = parser_peek(p);
    if (token_kw(start, "_Static_assert")) {
        if (pub) parser_fail(p, start, "'pub' is not allowed on a static assertion");
        return parse_static_assert(p);
    }

    int at_file_scope = context == 0;
    int flags = 0;
    Expr* specs = parse_decl_specifiers(p, &flags, 1);
    flags |= pub;

    Expr* group = expr_new(E_DECLGROUP, start, NULL, NULL);
    group->type = specs;
    group->flags = flags;

    if (token_punct(parser_peek(p), ";")) {
        parser_advance(p);
        return group;
    }

    for (;;) {
        DeclInfo info = {0};
        Expr* type = parse_declarator(p, specs, &info, DM_NAMED | (at_file_scope ? DM_GENERIC : 0));
        Expr* fn = decl_function_type(type);
        if (info.generics && !fn)
            parser_fail(p, info.name, "'%.*s' cannot declare generic parameters: only functions, methods and generic typedefs can",
                        (int)info.name.length, info.name.start);

        if (fn && !group->blockstmt_size && token_punct(parser_peek(p), "{")) {
            if (!at_file_scope)
                parser_fail(p, info.name, "function definitions are not allowed inside a function");
            if (flags & F_TYPEDEF)
                parser_fail(p, info.name, "a typedef cannot have a function body");
            Expr* def = parse_function_definition(p, type, &info, flags);
            parser_declare(p, info.name, 2);
            return def;
        }

        Expr* decl = expr_new(E_DECL, info.name, NULL, NULL);
        decl->name = info.name;
        decl->type = type;
        decl->generics = info.generics;
        decl->flags = flags;
        if (info.has_scope) parser_scope_pop(p, info.scope);
        // the name is visible in its own initializer
        parser_declare(p, info.name, (flags & F_TYPEDEF) ? 1 : fn ? 2 : 0);

        if (token_punct(parser_peek(p), "=")) {
            Token eq = parser_peek(p);
            parser_advance(p);
            if (flags & F_TYPEDEF) parser_fail(p, eq, "a typedef cannot have an initializer");
            if (p->prescan) {
                parser_skip_until(p, ",;");
                decl->left = expr_new(E_ALL, eq, NULL, NULL);
            } else {
                decl->left = parse_initializer(p);
            }
        }
        expr_push(group, decl);
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
    }
    parser_expect(p, ";");
    return group;
}

// A method's receiver is its first parameter and is named `self`
static inline void check_receiver(Parser* p, Expr* fn, Token name) {
    if (!fn->blockstmt_size || !token_is(fn->blockstmt[0]->name, "self"))
        parser_fail(p, name, "the first parameter of method '%.*s' must be the receiver, named 'self' with an explicit type",
                    (int)name.length, name.start);
    for (size_t i = 1; i < fn->blockstmt_size; i++)
        if (token_is(fn->blockstmt[i]->name, "self"))
            parser_fail(p, fn->blockstmt[i]->name, "'self' must be the first parameter");
    if (name.kind == TK_DOTOP && (fn->blockstmt_size != 2 || (fn->flags & F_VARIADIC)))
        parser_fail(p, name, "dotted operator '%.*s' must take exactly two parameters: the receiver and the right operand",
                    (int)name.length, name.start);
}


// Trait
//
// trait-declaration = "trait" [ generic-parameters ] IDENT "{" { trait-member } "}" [ ";" ]

// The name after `trait`
static inline Expr* parse_trait_name(Parser* p) {
    Token tok = parser_expect_ident(p, "a trait name");
    return expr_new(E_IDENT, tok, NULL, NULL);
}

// trait-member = [ "@prec" "(" INT "," INT ")" ] declaration-specifiers declarator ";"
// A method prototype; with @prec, a dotted operator and its binding powers.

// @prec(left_bp, right_bp): an E_ALL carrying the packed binding powers in aux,
// which istack_trait_member moves into the declaration
static inline Expr* parse_trait_prec(Parser* p) {
    Expr* prec = expr_new(E_ALL, parser_peek(p), NULL, NULL);
    parser_advance(p);
    if (!token_ident(parser_peek(p), "prec"))
        parser_fail(p, parser_peek(p), "unknown annotation; expected '@prec(left_bp, right_bp)'");
    parser_advance(p);
    parser_expect(p, "(");
    int bp[2];
    for (int i = 0; i < 2; i++) {
        Token num = parser_peek(p);
        char* end = NULL;
        long value = num.kind == TK_PP_NUM ? strtol(num.start, &end, 0) : -1;
        if (num.kind != TK_PP_NUM || end != num.start + num.length || value < 0 || value > 0x7fff)
            parser_fail(p, num, "'@prec' takes two integer literals");
        bp[i] = (int)value;
        parser_advance(p);
        if (i == 0) parser_expect(p, ",");
    }
    parser_expect(p, ")");
    prec->aux = (1 << 30) | (bp[0] << 15) | bp[1];
    return prec;
}

// The prototype: an E_DECL with name, type and generics
static inline Expr* parse_trait_prototype(Parser* p) {
    Expr* specs = parse_decl_specifiers(p, NULL, 0);
    DeclInfo info = {0};
    Expr* type = parse_declarator(p, specs, &info, DM_NAMED | DM_GENERIC | DM_METHOD);
    Expr* fn = decl_function_type(type);
    if (!fn)
        parser_fail(p, info.name, "trait members must be method prototypes");
    check_receiver(p, fn, info.name);
    if (info.has_scope) parser_scope_pop(p, info.scope);
    if (token_punct(parser_peek(p), "{"))
        parser_fail(p, parser_peek(p), "trait methods are prototypes only; default bodies are not allowed");

    Expr* decl = expr_new(E_DECL, info.name, NULL, NULL);
    decl->name = info.name;
    decl->type = type;
    decl->generics = info.generics;
    return decl;
}

// context: the declaration; stack: [ @prec ]
static inline void istack_trait_member(Expr* context, Expr* stack[256], size_t* idx) {
    if (*idx == 1) context->aux = stack[0]->aux;
}

static const ParseRule trait_member_rule[] = {
    // @prec(...)
    BIF("@"),
        REC(parse_trait_prec),
        POP, // put into return context
    END,

    REC(parse_trait_prototype),
    POP, // the declaration is the member
    EXPECT(";"),

    ISTACK(istack_trait_member),
    END
};

static inline Expr* parse_trait_member(Parser* p) {
    Token start = parser_peek(p);
    Expr* member = parse_rule(p, trait_member_rule);
    if (member && member->aux && member->name.kind != TK_DOTOP)
        parser_fail(p, start, "'@prec' is only allowed on dotted-operator prototypes");
    return member;
}

// The members between the braces, collected in an E_LIST
static inline Expr* parse_trait_members(Parser* p) {
    Expr* members = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    while (!token_punct(parser_peek(p), "}")) {
        if (parser_peek(p).kind == TK_EOF) set_parser_error(p, PERR_OPEN_CBRACK);
        if (token_kw(parser_peek(p), "pub"))
            parser_fail(p, parser_peek(p), "trait methods do not take 'pub'; visibility belongs to the trait");
        if (parse_ct_control(p, members, E_TRAIT)) continue;
        expr_push(members, parse_trait_member(p));
    }
    return members;
}

// stack: [ generics ] name members
// The generic parameters are an E_LIST and the name an E_IDENT, so the kind of
// stack[0] tells whether `<...>` was there.
static inline void istack_trait(Expr* context, Expr* stack[256], size_t* idx) {
    (void)idx;
    size_t i = 0;
    if (stack[i]->kind == E_LIST) context->generics = stack[i++];
    context->name = stack[i++]->token;
    Expr* members = stack[i];
    context->blockstmt            = members->blockstmt;
    context->blockstmt_size       = members->blockstmt_size;
    context->alloc_blockstmt_size = members->alloc_blockstmt_size;
}

static const ParseRule trait_rule[] = {
    TOK("trait", E_TRAIT),
    POP,

    // <...>
    BIF("<"),
        REC(parse_generic_params),
        POP, // put into return context
    END,
    REC(parse_trait_name),

    // { ... }
    EXPECT("{"),
    REC(parse_trait_members),
    EXPECT("}"),

    ISTACK(istack_trait),
    END
};

static inline Expr* parse_trait(Parser* p, int pub) {
    // the generic parameters and `Self` are names of the trait only
    size_t scope = parser_scope_push(p);
    parser_declare(p, token_synth("Self", TK_IDENT, parser_peek(p)), 1);
    Expr* trait = parse_rule(p, trait_rule);
    if (trait) {
        trait->flags = pub;
        if (token_punct(parser_peek(p), ";")) parser_advance(p);
    }
    parser_scope_pop(p, scope);
    return trait;
}

// END Trait

// Impl
//
// impl-block = "impl" [ generic-parameters ] [ trait "for" ] type "{" { [ "pub" ] method } "}" [ ";" ]
// method     = [ "pub" ] declaration-specifiers declarator block
// E_IMPL: generics, left = trait (E_TY_NAME) or NULL, type = target, blockstmt = E_FUNCDEF

// The prescan: nothing in an impl declares a name other files need to parse,
// except which names are followed by `<`: methods that may be called with
// explicit generic arguments, kept as E_IDENT in blockstmt
static inline Expr* parse_impl_prescan(Parser* p) {
    Expr* impl = expr_new(E_IMPL, parser_peek(p), NULL, NULL);
    parser_advance(p);
    while (!token_punct(parser_peek(p), "{")) {
        if (parser_peek(p).kind == TK_EOF) parser_fail(p, parser_peek(p), "expected '{' after 'impl'");
        parser_advance(p);
    }
    int depth = 0;
    do {
        Token tok = parser_peek(p);
        if (tok.kind == TK_EOF) parser_fail(p, tok, "expected '}' before end of file");
        if (token_punct(tok, "{")) depth++;
        else if (token_punct(tok, "}")) depth--;
        else if (tok.kind == TK_IDENT && depth == 1 && token_punct(parser_peek2(p), "<"))
            expr_push(impl, expr_new(E_IDENT, tok, NULL, NULL));
        parser_advance(p);
    } while (depth > 0);
    if (token_punct(parser_peek(p), ";")) parser_advance(p);
    return impl;
}

// The definition after `pub`: specifiers, a function declarator with the
// receiver first, and the body. One helper, since the specifiers' storage flags
// and the declarator's DeclInfo go on into parse_function_definition.
static inline Expr* parse_method_definition(Parser* p) {
    int flags = 0;
    Expr* specs = parse_decl_specifiers(p, &flags, 1);
    if (flags & (F_TYPEDEF | F_EXTERN))
        parser_fail(p, specs->token, "an impl block contains only method definitions");
    DeclInfo info = {0};
    Expr* type = parse_declarator(p, specs, &info, DM_NAMED | DM_GENERIC | DM_METHOD);
    Expr* fn = decl_function_type(type);
    if (!fn || !token_punct(parser_peek(p), "{"))
        parser_fail(p, info.name, "an impl block contains only method definitions");
    check_receiver(p, fn, info.name);
    return parse_function_definition(p, type, &info, flags);
}

// context: the E_FUNCDEF; stack: [ pub ]
static inline void istack_method(Expr* context, Expr* stack[256], size_t* idx) {
    (void)stack;
    if (*idx == 1) context->flags |= F_PUB;
}

static const ParseRule method_rule[] = {
    // pub
    BIF("pub"),
        TOK("pub", E_ALL),
        POP, // put into return context
    END,

    REC(parse_method_definition),
    POP, // the definition is the method
    ISTACK(istack_method),
    END
};

static inline Expr* parse_method(Parser* p) {
    return parse_rule(p, method_rule);
}

// The methods between the braces, collected in an E_LIST, which also takes
// the comments before the closing brace
static inline Expr* parse_impl_methods(Parser* p) {
    Expr* methods = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    Expr* prev = NULL;
    while (!token_punct(parser_peek(p), "}")) {
        if (parser_peek(p).kind == TK_EOF) set_parser_error(p, PERR_OPEN_CBRACK);
        Token start = parser_peek(p);
        if (parse_ct_control(p, methods, E_IMPL)) continue;
        Expr* method = parse_method(p);
        parser_take_trivia(p, prev, start, method, NULL);
        expr_push(methods, method);
        prev = method;
    }
    parser_take_trivia(p, prev, parser_peek(p), NULL, methods);
    return methods;
}

// stack: [ generics ] type [ type ] methods
// The generic parameters are an E_LIST and types E_TY_*, so the kind of
// stack[0] tells whether `<...>` was there; two types are `trait for target`.
static inline void istack_impl(Expr* context, Expr* stack[256], size_t* idx) {
    size_t i = 0;
    if (stack[i]->kind == E_LIST) context->generics = stack[i++];
    Expr* first = stack[i++];
    if (*idx - i == 2) {
        context->left = first;
        context->type = stack[i++];
    } else {
        context->type = first;
    }
    Expr* methods = stack[i];
    context->blockstmt            = methods->blockstmt;
    context->blockstmt_size       = methods->blockstmt_size;
    context->alloc_blockstmt_size = methods->alloc_blockstmt_size;
    context->inner                = methods->inner;
    context->inner_len            = methods->inner_len;
}

static const ParseRule impl_rule[] = {
    // impl<...> ... for ...
    TOK("impl", E_IMPL),
    POP,

    // <...>
    BIF("<"),
        REC(parse_generic_params),
        POP, // put into return context
    END,

    REC(parse_type_name),

    // for ...
    BIF("for"),
        EXPECT("for"),
        REC(parse_type_name),
        POP, // put into return context
    END,

    // { ... }
    EXPECT("{"),
    REC(parse_impl_methods),
    EXPECT("}"),

    ISTACK(istack_impl),
    END
};

static inline Expr* parse_impl(Parser* p) {
    if (p->prescan) return parse_impl_prescan(p);
    // the generic parameters and `Self` are names of the impl only
    size_t scope = parser_scope_push(p);
    parser_declare(p, token_synth("Self", TK_IDENT, parser_peek(p)), 1);
    Expr* impl = parse_rule(p, impl_rule);
    if (impl) {
        Expr* trait = impl->left;
        if (trait && (trait->kind != E_TY_NAME || (trait->flags & F_QUALS)))
            parser_fail(p, trait->token, "expected a trait name before 'for'");
        if (token_punct(parser_peek(p), ";")) parser_advance(p);
    }
    parser_scope_pop(p, scope);
    return impl;
}

// END Impl

// Generic typedef
//
// generic-typedef = "typedef" generic-parameters specifiers declarator(Name<T, ...>) ";"
// E_GTYPEDEF: name, generics, type, left = specifiers

// The specifiers and the declarator: an E_DECL with left = specifiers, type,
// name, and generics = the list repeated after the name
static inline Expr* parse_generic_typedef_declarator(Parser* p) {
    Expr* specs = parse_decl_specifiers(p, NULL, 0);
    if (specs->kind == E_TY_STRUCT && (specs->flags & F_HAS_BODY) && specs->name.length)
        parser_fail(p, specs->name, "a struct defined by a generic typedef must be anonymous; remove the tag '%.*s'",
                    (int)specs->name.length, specs->name.start);

    DeclInfo info = {0};
    Expr* type = parse_declarator(p, specs, &info, DM_NAMED | DM_GTYPEDEF);
    Expr* decl = expr_new(E_DECL, info.name, NULL, NULL);
    decl->left = specs;
    decl->type = type;
    decl->name = info.name;
    decl->generics = info.generics;
    return decl;
}

// stack: generics declarator
// The declarator stays in right until parse_generic_typedef has checked the
// list repeated after the name, which needs both parts.
static inline void istack_generic_typedef(Expr* context, Expr* stack[256], size_t* idx) {
    (void)idx;
    Expr* decl = stack[1];
    context->generics = stack[0];
    context->left     = decl->left;
    context->type     = decl->type;
    context->name     = decl->name;
    context->right    = decl;
}

static const ParseRule generic_typedef_rule[] = {
    TOK("typedef", E_GTYPEDEF),
    POP,

    REC(parse_generic_params),
    REC(parse_generic_typedef_declarator),
    EXPECT(";"),

    ISTACK(istack_generic_typedef),
    END
};

// The list after the name is mandatory and repeats the parameters exactly
static inline void check_repeated_params(Parser* p, Expr* def) {
    Expr* params = def->generics;
    Expr* names = def->right->generics;
    Token name = def->name;
    if (!names)
        parser_fail(p, name, "generic typedef '%.*s' must repeat its parameters after the name, as in '%.*s<%.*s%s>'",
                    (int)name.length, name.start, (int)name.length, name.start,
                    (int)params->blockstmt[0]->name.length, params->blockstmt[0]->name.start,
                    params->blockstmt_size > 1 ? ", ..." : "");
    if (names->blockstmt_size != params->blockstmt_size)
        parser_fail(p, names->token, "generic typedef '%.*s' declares %zu parameter(s) but repeats %zu",
                    (int)name.length, name.start, params->blockstmt_size, names->blockstmt_size);
    for (size_t i = 0; i < names->blockstmt_size; i++)
        if (!token_same(names->blockstmt[i]->token, params->blockstmt[i]->name))
            parser_fail(p, names->blockstmt[i]->token, "expected '%.*s' here: the list after the name repeats the generic parameters in order",
                        (int)params->blockstmt[i]->name.length, params->blockstmt[i]->name.start);
}

static inline Expr* parse_generic_typedef(Parser* p, int pub) {
    // the generic parameters are names of the typedef only; its own name is
    // declared after them, outside
    size_t scope = parser_scope_push(p);
    Expr* def = parse_rule(p, generic_typedef_rule);
    if (def) {
        def->flags = pub;
        check_repeated_params(p, def);
        def->right = NULL;
    }
    parser_scope_pop(p, scope);
    if (def) parser_declare(p, def->name, 1);
    return def;
}

// END Generic typedef

// Import
//
// import-declaration = "import" namespace-path [ "as" IDENT ] ";"
// namespace-path     = IDENT { "::" IDENT }
// E_IMPORT: blockstmt = E_IDENT path, name = local binding

// The path, `a::b::c`: an E_LIST of the E_IDENT segments
static inline Expr* parse_import_path(Parser* p) {
    Expr* path = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    for (;;) {
        expr_push(path, parse_ident_node(p));
        if (!token_punct(parser_peek(p), "::")) break;
        parser_advance(p);
    }
    return path;
}

// The name after `as`
static inline Expr* parse_import_alias(Parser* p) {
    Token tok = parser_expect_ident(p, "an alias after 'as'");
    return expr_new(E_IDENT, tok, NULL, NULL);
}

// stack: path [ alias ]
// The local binding is the alias, or else the last segment of the path.
static inline void istack_import(Expr* context, Expr* stack[256], size_t* idx) {
    Expr* path = stack[0];
    context->blockstmt            = path->blockstmt;
    context->blockstmt_size       = path->blockstmt_size;
    context->alloc_blockstmt_size = path->alloc_blockstmt_size;
    context->name = *idx == 2 ? stack[1]->token : path->blockstmt[path->blockstmt_size - 1]->token;
}

static const ParseRule import_rule[] = {
    TOK("import", E_IMPORT),
    POP,

    REC(parse_import_path),

    // as ... (`as` is only special right here)
    BIF("as"),
        EXPECT("as"),
        REC(parse_import_alias),
        POP, // put into return context
    END,
    EXPECT(";"),

    ISTACK(istack_import),
    END
};

static inline Expr* parse_import(Parser* p) {
    return parse_rule(p, import_rule);
}

// END Import

static inline Expr* parse_external_declaration(Parser* p) {
    Token tok = parser_peek(p);

    if (tok.kind == TK_RAW) {
        parser_advance(p);
        return expr_new(E_RAW, tok, NULL, NULL);
    }
    if (tok.kind == TK_INCLUDE) {
        parser_advance(p);
        return expr_new(E_INCLUDE, tok, NULL, NULL);
    }
    if (tok.kind == TK_DIRECTIVE)
        parser_fail(p, tok, "preprocessor directives other than #include must be inside a raw C block (#{ ... }#)");
    if (token_punct(tok, ";")) {
        parser_advance(p);
        return NULL;
    }
    if (token_kw(tok, "import"))
        return parse_import(p);
    Expr* ct = parse_ct_external(p);
    if (ct) return ct;

    int pub = 0;
    if (token_kw(tok, "pub")) {
        pub = F_PUB;
        parser_advance(p);
        Token next = parser_peek(p);
        if (token_kw(next, "impl"))
            parser_fail(p, tok, "'pub' cannot prefix an impl block; mark individual methods instead");
        if (token_kw(next, "import"))
            parser_fail(p, tok, "'pub' cannot prefix an import");
        if (next.kind == TK_RAW || next.kind == TK_INCLUDE)
            parser_fail(p, tok, "'pub' must be followed by a declaration");
        tok = next;
    }

    if (token_kw(tok, "trait"))
        return parse_trait(p, pub);
    if (token_kw(tok, "impl"))
        return parse_impl(p);
    if (token_kw(tok, "typedef") && token_punct(parser_peek2(p), "<"))
        return parse_generic_typedef(p, pub);
    return parse_declaration(p, 0, pub);
}

// One declaration of a preprocessed header. Anything the parser cannot read
// (compiler extensions, mostly) is skipped: it simply stays invisible to Typed C.
static inline Expr* parse_header_declaration(Parser* p) {
    jmp_buf attempt;
    int snapshot = p->snapshot_idx;
    size_t names = p->names_len;

    if (token_punct(parser_peek(p), ";")) {
        parser_advance(p);
        return NULL;
    }
    parser_push_snapshot(p);
    p->spec_depth = 0;
    p->spec_jmp[p->spec_depth++] = &attempt;
    if (setjmp(attempt) == 0) {
        Expr* decl = parse_declaration(p, 0, 0);
        p->spec_depth = 0;
        parser_discard_snapshot(p);
        return decl;
    }
    p->snapshot_idx = snapshot + 1;
    parser_pop_snapshot(p);
    p->spec_depth = 0;
    p->strict_types = 0;
    p->names_len = names;
    skip_declaration(p);
    return NULL;
}

// One external declaration of a Typed C file. After a syntax error the rest
// of the declaration is skipped, so the following ones are still checked.
// Returns NULL for a declaration that was skipped; `*stop` is set when the
// rest of the file cannot be read either.
static inline Expr* parse_recovering_declaration(Parser* p, int* stop) {
    jmp_buf attempt;
    jmp_buf* outer = p->recover;
    int snapshot = p->snapshot_idx;
    size_t names = p->names_len;
    Token first = parser_peek(p);

    parser_push_snapshot(p);
    p->recover = &attempt;
    if (setjmp(attempt) == 0) {
        Expr* decl = parse_external_declaration(p);
        parser_discard_snapshot(p);
        p->recover = outer;
        return decl;
    }
    // back to the start of the declaration, then past its end
    p->snapshot_idx = snapshot + 1;
    parser_pop_snapshot(p);
    p->spec_depth = 0;
    p->strict_types = 0;
    p->switch_depth = 0;
    p->names_len = names;
    p->silent = 1;
    if (p->errors >= 20 || setjmp(attempt) != 0) {
        // too many errors, or the text cannot even be skipped (an unterminated literal)
        *stop = 1;
    } else {
        int block_ends = token_is(first, "impl") || token_is(first, "trait");
        if (block_ends) {
            // impl / trait: the block is the whole declaration
            while (parser_peek(p).kind != TK_EOF && !token_punct(parser_peek(p), "{") &&
                   !token_punct(parser_peek(p), ";"))
                parser_advance(p);
            if (token_punct(parser_peek(p), "{")) parser_skip_block(p);
            if (token_punct(parser_peek(p), ";")) parser_advance(p);
        } else {
            skip_declaration(p);
        }
    }
    p->silent = p->prescan;
    p->recover = outer;
    return NULL;
}

// translation-unit = { external-declaration } EOF
static inline Expr* parse_translation_unit(Parser* p) {
    Expr* file = expr_new(E_FILE, parser_peek(p), NULL, NULL);
    if (p->header_mode) parser_skip_noise(p);
    else if (p->token.kind == TK_ERR) {
        // re-run the first token through the checks in parser_advance
        p->lexer = lexer_new(p->lexer.source);
        parser_advance(p);
    }
    Expr* prev = NULL;
    int stop = 0;
    // the prescan only collects names: its errors are reported by the full parse
    if (p->prescan && !p->header_mode) p->silent = 1;
    while (!stop && parser_peek(p).kind != TK_EOF) {
        Token start = parser_peek(p);
        Expr* decl = p->header_mode ? parse_header_declaration(p)
                   : parse_recovering_declaration(p, &stop);
        if (!decl) continue;
        parser_take_trivia(p, prev, start, decl, NULL);
        expr_push(file, decl);
        prev = decl;
    }
    parser_take_trivia(p, prev, parser_peek(p), NULL, file);
    return file;
}
