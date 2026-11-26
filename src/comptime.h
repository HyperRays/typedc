#pragma once
#include "check.h"

// Compile-time code.
//
// Ordinary code is walked by the checker as always, and is never run. Where
// the checker meets something compile-time, it calls in here. A compile-time
// expression is evaluated, and its value is what the checker writes. A branch
// or loop on a compile-time condition is carried out, and each time it passes
// over its body, that body goes through the checker again, in the scope the
// checker has at that moment.
//
// A function whose name starts with '@' is run by walking its statements with
// the same code. There every statement is carried out and nothing is written.
//
// A compile-time value of any type is held as a running program would hold
// it: a struct is its bytes, a pointer is an address.

// ---------------------------------------------------------------------------
// Syntax
// ---------------------------------------------------------------------------

static Map ct_splices;          // where an @ident(...) starts in the source -> its arguments, as an E_LIST
static int ct_emits;            // how many '@emit' the program has

// Returns whether a name is an @ident(...), which is evaluated to the name it stands for.
static int ct_spliced(Token name) {
    return name.length > 7 && memcmp(name.start, "@ident(", 7) == 0;
}

// Called where a name can stand. If the parser is at an '@' that is directly
// followed by an identifier, the two become one token, an identifier whose
// text starts with '@'. An `@ident(...)` becomes one such token as a whole,
// and its arguments are kept for when the name is needed.
static inline void ct_name(Parser* p) {
    Token at = p->token;
    Token name = parser_peek2(p);
    if (!token_punct(at, "@") || name.kind != TK_IDENT || name.start != at.start + 1) return;
    parser_advance(p);
    p->token.start = at.start;
    p->token.length++;
    p->token.col = at.col;
    if (!token_ident(p->token, "@ident") || !token_punct(parser_peek2(p), "(")) return;
    Token first = p->token;
    Expr* args = expr_new(E_LIST, first, NULL, NULL);
    parser_advance(p);
    parser_advance(p);
    if (p->prescan) parser_skip_until(p, ")");
    while (!p->prescan) {
        expr_push(args, parse_assignment_expression(p));
        if (!token_punct(parser_peek(p), ",")) break;
        parser_advance(p);
    }
    Token close = parser_peek(p);
    if (!token_punct(close, ")")) parser_fail(p, close, "expected ')' after the arguments of '@ident'");
    map_put(&ct_splices, xstrf("%p", (void*)first.start), args);
    p->token = first;
    p->token.length = (size_t)(close.start + 1 - first.start);
}

// Returns whether the '@' the parser is at starts a type: `@int` does, and so
// do `@Name` when `Name` is a type, `@Type`, and `@type(...)`. Anything else,
// such as `@name(...)`, is a call.
static inline int at_ct_type(Parser* p) {
    Token name = parser_peek2(p);
    if (name.kind == TK_KEYWORD) return token_is_type_keyword(name);
    if (name.kind != TK_IDENT || name.start != p->token.start + 1) return 0;
    Token whole = name;         // a type that was declared with the '@' in its name
    whole.start--;
    whole.length++;
    return token_ident(name, "Type") || token_ident(name, "type") || is_type_path(p, &name, 1) || is_type_path(p, &whole, 1);
}

// Takes the '@' of `namespace::@Name`, which asks for the compile-time
// version of a type from a namespace. Returns the mark that the type gets.
static inline int parse_ct_mark(Parser* p) {
    if (!token_punct(parser_peek(p), "@")) return 0;
    parser_advance(p);
    return F_COMPTIME;
}

// type-specifier = "@" type-specifier | "@Type" | "@type" "(" expression ")"
// Called at the '@'. Returns the specifier, or NULL for the first form, whose
// type is parsed by the caller and marked as compile-time through `quals`.
static inline Expr* parse_ct_spec(Parser* p, int* quals) {
    Token whole = parser_peek2(p);
    whole.start--;
    whole.length++;
    if (whole.kind == TK_IDENT && whole.start == p->token.start && is_type_path(p, &whole, 1)) {
        // a compile-time type: its variables are compile-time variables
        ct_name(p);
        *quals |= F_COMPTIME;
        return parse_typedef_name(p);
    }
    parser_advance(p);
    Token name = parser_peek(p);
    Expr* spec = expr_new(E_CT_TYPE, name, NULL, NULL);
    if (token_ident(name, "type") && token_punct(parser_peek2(p), "(")) {
        parser_advance(p);
        parser_advance(p);
        if (p->prescan) parser_skip_until(p, ")");
        else spec->left = parse_expression(p);
        parser_expect(p, ")");
        return spec;
    }
    *quals |= F_COMPTIME;
    if (!token_ident(name, "Type") || is_type_path(p, &name, 1)) return NULL;
    parser_advance(p);
    return spec;
}

// block-item = ...existing... | import-declaration | "@emit" external-declaration
static inline Expr* parse_ct_block_item(Parser* p) {
    Token tok = parser_peek(p);
    Token name = parser_peek2(p);
    if (token_kw(tok, "import")) return parse_import(p);
    if (!token_punct(tok, "@") || !token_ident(name, "emit") || name.start != tok.start + 1) return NULL;
    parser_advance(p);
    parser_advance(p);
    ct_emits++;
    return expr_new(E_EMIT, tok, parse_external_declaration(p), NULL);
}

// Called behind the '.' or '->' of a member access or of a designator. Takes
// a `[expression]`, which names the member by a compile-time value, and
// returns whether there was one.
static inline int parse_ct_member(Parser* p, Expr* member) {
    if (!token_punct(parser_peek(p), "[")) {
        ct_name(p);      // the name can be made by @ident
        return 0;
    }
    parser_advance(p);
    Expr* field = parse_expression(p);
    parser_expect(p, "]");
    if (member->kind == E_DESIG) member->left = field, member->aux = 1;
    else member->right = field;
    return 1;
}

// Returns whether a declaration of compile-time variables stands where a
// member is expected: something with an initializer, or, where the members
// are methods, something that is not one.
static inline int at_ct_declaration(Parser* p, int in) {
    Lexer lexer = p->lexer;
    int depth = 0;
    for (Token tok = p->token;; tok = lexer_next(&lexer)) {
        if (tok.kind == TK_EOF) return 0;
        if (tok.kind != TK_PUNCT) continue;
        if (token_is(tok, "[")) depth++;
        if (token_is(tok, "]")) depth--;
        if (depth) continue;
        if (token_is(tok, "=")) return 1;
        if (token_is(tok, ";")) return in != E_TY_STRUCT;
        if (token_is(tok, "(") || token_is(tok, "{") || token_is(tok, "}") || token_is(tok, ":")) return 0;
    }
}

// X-body = X-member | "{" { X-member } "}"
static inline Expr* parse_ct_members(Parser* p, int in) {
    Expr* list = expr_new(E_LIST, parser_peek(p), NULL, NULL);
    int braces = token_punct(parser_peek(p), "{");
    if (braces) parser_advance(p);
    do {
        if (braces && token_punct(parser_peek(p), "}")) break;
        if (parser_peek(p).kind == TK_EOF) parser_fail(p, parser_peek(p), "expected '}' before end of file");
        if (in == E_TY_STRUCT) parse_struct_member(p, list);
        else if (!parse_ct_control(p, list, in)) expr_push(list, in == E_IMPL ? parse_method(p) : parse_trait_member(p));
    } while (braces);
    if (braces) parser_advance(p);
    return list;
}

// X-control = "if" "(" expression ")" X-body [ "else" X-body ]
//           | "for" "(" [ declaration | expression ";" ] [ expression ] ";" [ expression ] ")" X-body
//           | "while" "(" expression ")" X-body
// Called where a member of a struct, an impl or a trait is expected. Takes
// compile-time control, or a declaration of compile-time variables, adds it
// to the members, and returns whether there was any.
static inline int parse_ct_control(Parser* p, Expr* owner, int in) {
    Token tok = parser_peek(p);
    int kind = token_kw(tok, "if") ? E_IF : token_kw(tok, "for") ? E_FOR : token_kw(tok, "while") ? E_WHILE : 0;
    if (!kind) {
        if (p->prescan || !at_ct_declaration(p, in)) return 0;
        expr_push(owner, parse_declaration(p, 1, 0));
        return 1;
    }
    Expr* control = expr_new((ExprKind)kind, tok, NULL, NULL);
    Condition* f = (Condition*)xalloc(2 * sizeof(Condition));
    size_t scope = parser_scope_push(p);
    parser_advance(p);
    parser_expect(p, "(");
    if (p->prescan) {
        parser_skip_until(p, ")");
    } else if (kind == E_FOR) {
        if (token_punct(parser_peek(p), ";")) parser_advance(p);
        else f->base_case = parse_for_init(p);
        if (!token_punct(parser_peek(p), ";")) f->condition = parse_if_cond(p);
        parser_expect(p, ";");
        if (!token_punct(parser_peek(p), ")")) f->increment = parse_expression(p);
    } else {
        f->condition = parse_if_cond(p);
    }
    parser_expect(p, ")");
    f->block = parse_ct_members(p, in);
    control->cases = f;
    control->case_size = 1;
    if (kind == E_IF && token_kw(parser_peek(p), "else")) {
        // as in an `if` statement, the first case then holds the else branch
        parser_advance(p);
        f[1] = f[0];
        f[0] = (Condition){ .block = parse_ct_members(p, in) };
        control->case_size = 2;
    }
    parser_scope_pop(p, scope);
    expr_push(owner, control);
    return 1;
}

// generator-start          = ct-call ";"
// deferred-generator-start = "for" generic-parameters ct-call ";"
// Called where a declaration at file scope is expected. Returns the start of
// a generator, or NULL if what stands there is something else.
static inline Expr* parse_ct_external(Parser* p) {
    Token tok = parser_peek(p);
    Expr* params = NULL;
    size_t scope = parser_scope_push(p);
    if (token_kw(tok, "for")) {
        parser_advance(p);
        params = parse_generic_params(p);
    } else if (token_punct(tok, "@")) {
        if (at_ct_type(p)) return NULL;
    } else {
        // namespace::@name(...)
        Lexer lexer = p->lexer;
        Token next = lexer_next(&lexer);
        while (tok.kind == TK_IDENT && token_punct(next, "::")) tok = lexer_next(&lexer), next = lexer_next(&lexer);
        if (!token_punct(tok, "@") || tok.start == parser_peek(p).start || at_type(p)) return NULL;
    }
    Expr* start = expr_new(E_GENERATOR, parser_peek(p), NULL, NULL);
    start->generics = params;
    if (p->prescan) {
        // the first pass only notes which function is called, and from which namespace
        if (parser_peek(p).kind == TK_IDENT && token_punct(parser_peek2(p), "::")) {
            start->left = expr_new(E_IDENT, parser_peek(p), NULL, NULL);
            parser_advance(p);
            parser_advance(p);
        }
        ct_name(p);
        start->name = parser_peek(p);
        parser_skip_until(p, ";");
    } else {
        start->left = parse_expression(p);
    }
    parser_expect(p, ";");
    parser_scope_pop(p, scope);
    return start;
}

// Skips the body of a function in the first pass over a file. What the body
// of an '@' function imports and includes is added to the function, because
// the namespaces have to be loaded before anything is parsed in full.
static inline void parse_ct_skip_body(Parser* p, Expr* def) {
    int depth = 0, at = *def->name.start == '@';
    do {
        Token tok = parser_peek(p);
        if (tok.kind == TK_EOF) parser_fail(p, tok, "expected '}' before end of file");
        if (at && token_kw(tok, "import")) {
            expr_push(def, parse_import(p));
            continue;
        }
        if (at && tok.kind == TK_INCLUDE) expr_push(def, expr_new(E_INCLUDE, tok, NULL, NULL));
        if (at && token_punct(tok, "@") && token_ident(parser_peek2(p), "emit")) {
            // A type that is emitted under a plain name is a type name where the
            // function is used, which the parser has to know: the name is noted.
            Token last = tok;
            int inner = 0;
            parser_advance(p);
            parser_advance(p);
            if (!token_kw(parser_peek(p), "typedef")) continue;
            for (;; parser_advance(p)) {
                Token t = parser_peek(p);
                if (t.kind == TK_EOF || (!inner && token_punct(t, ";"))) break;
                if (token_punct(t, "{") || token_punct(t, "(") || token_punct(t, "[")) inner++;
                if (token_punct(t, "}") || token_punct(t, ")") || token_punct(t, "]")) inner--;
                if (!inner) last = t;
            }
            if (last.kind == TK_IDENT) expr_push(def, expr_new(E_IDENT, last, NULL, NULL));
            continue;
        }
        if (token_punct(tok, "{")) depth++;
        else if (token_punct(tok, "}")) depth--;
        parser_advance(p);
    } while (depth > 0);
}

// Returns whether a declaration with these specifiers declares compile-time variables.
static int ct_spec(Expr* spec) {
    return spec && (spec->flags & F_COMPTIME);
}

// ---------------------------------------------------------------------------
// Values in memory
// ---------------------------------------------------------------------------

// A number is worked on as a word, which is 64 bits wide no matter how wide
// an int is on the computer typedc runs on. A floating-point number is in a
// word as the bits of a double.
typedef int64_t Word;
typedef uint64_t UWord;

// A value while it is being worked on: a number, or the address of a struct
// or an array.
typedef struct {
    Type* type;
    Word v;
} CtValue;

// How a number is held in memory: its size in bytes, with CT_UNSIGNED,
// CT_TRUTH (a _Bool) or CT_FLOAT added. A struct or an array has no kind.
enum { CT_UNSIGNED = 16, CT_TRUTH = 32, CT_FLOAT = 64 };

#define CT_STACK (1 << 20)      // bytes for the variables of the code that is running
#define CT_NULL_REACH 4096      // addresses below this are null, or a member or element of what null points to
#define CT_CALLS 200            // calls that can be in progress at once
#define CT_PASSES 1000000       // passes of one loop that is run at compile time

// A compile-time variable that is declared at file scope
typedef struct {
    Expr* decl;
    SrcFile* file;
    Type* type;
    int state;                  // 0: it has no value yet, 1: its value is being computed, 2: it has it
    void* memory;               // where the value is
} CtGlobal;

// Where the code that is being generated or run stands
typedef struct {
    Func* running;              // the function that is being run, or NULL while the checker generates ordinary code
    Type* ret;                  // the type it returns
    const char* call;           // the '@' call that is being run, as its function and arguments
    SrcFile* use;               // where ordinary code, or a file, called into the '@' code that is running
    int runtime;                // runtime branches and loops around the statement being generated
    int unrolling;              // `runtime` at the loop being run at compile time, -1 outside one
    int deciding;               // `runtime` at the switch being decided at compile time, -1 outside one
    int in_switch;              // inside a runtime switch of that loop: `break` belongs to the switch
    int flow;                   // CT_BREAK, CT_CONTINUE or CT_RETURN on its way out
    long floor;                 // the variables below this cannot change: they belong to a caller of the '@' call
                                // that is running, or were declared outside a runtime branch or loop
} CtState;

enum { CT_BREAK = 1, CT_CONTINUE, CT_RETURN };

static CtState g_ct = { .unrolling = -1, .deciding = -1 };
static Expr* g_ct_pass;         // the statement that is handed back to gen_stmt as it is

// Variables are on a stack. Those of the function body the checker is
// generating are at its bottom, and those of each call lie above its caller's.
static Word ct_stack[CT_STACK / 8];
static long ct_top;             // bytes in use
static FuncInst* ct_owner;      // the function body whose variables are at the bottom
static List ct_kept;            // the other memory: an address, then a size, for each piece
static Map ct_funcs;            // namespace and name -> Func*, for the '@' functions
static Map ct_brings;           // an '@' function -> SrcFile* with only what its body imports and includes
static Map ct_reflected;        // what a predefined function returned for a type
static int ct_soft;             // the call that is being run was told that something is unimplemented
static long ct_c_size;          // the last size that was asked for, without compile-time members
static Map ct_globals;          // namespace and name -> CtGlobal*
static Map ct_results;          // a finished call of an '@' function, as the function and its arguments -> what it returned
static const char* ct_calls[CT_CALLS];  // the functions whose calls are in progress
static int ct_depth;
static CtValue ct_result;       // what the `return` that is on its way out returns
static int ct_dry;              // while this is set, an expression is only asked for its type: nothing is read or changed

static char* ct_at(long place) {
    return (char*)ct_stack + place;
}

// Keeps a copy of some bytes for good: the value of a variable at file
// scope, a struct that was returned, a string.
static void* ct_keep(const void* from, size_t size) {
    void* memory = memcpy(xalloc(size ? size : 1), from, size);
    list_push(&ct_kept, memory);
    list_push(&ct_kept, (void*)size);
    return memory;
}

// Returns how a value of a type is held in memory, or 0 for a struct, an
// array, and a type there are no values of.
static int ct_kind(Type* type) {
    static const int kinds[BK_COUNT] = {
        [BK_BOOL] = 1 | CT_UNSIGNED | CT_TRUTH, [BK_CHAR] = 1, [BK_SCHAR] = 1, [BK_UCHAR] = 1 | CT_UNSIGNED,
        [BK_SHORT] = sizeof(short), [BK_USHORT] = sizeof(short) | CT_UNSIGNED,
        [BK_INT] = sizeof(int), [BK_UINT] = sizeof(int) | CT_UNSIGNED,
        [BK_LONG] = sizeof(long), [BK_ULONG] = sizeof(long) | CT_UNSIGNED,
        [BK_LLONG] = sizeof(long long), [BK_ULLONG] = sizeof(long long) | CT_UNSIGNED,
        [BK_FLOAT] = sizeof(float) | CT_FLOAT, [BK_DOUBLE] = sizeof(double) | CT_FLOAT, [BK_LDOUBLE] = sizeof(double) | CT_FLOAT,
    };
    Type* k = type_canon(type);
    if (k->kind == TY_PTR) return sizeof(void*) | CT_UNSIGNED;
    if (k->kind == TY_ENUM) return sizeof(int);
    return k->kind == TY_BUILTIN ? kinds[k->builtin] : 0;
}

static double ct_double(Word bits) {
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static Word ct_bits(double d) {
    Word bits;
    memcpy(&bits, &d, sizeof bits);
    return bits;
}

// Returns the value a variable of the given kind holds after `a` is stored in it.
static Word ct_fit(Word a, int kind) {
    int bits = (kind & 15) * 8;
    if (kind & CT_FLOAT) return bits == 32 ? ct_bits((double)(float)ct_double(a)) : a;
    if (kind & CT_TRUTH) return a != 0;
    if (!bits || bits >= 64) return a;
    UWord mask = (1ULL << bits) - 1, low = (UWord)a & mask;
    return (kind & CT_UNSIGNED) || !(low >> (bits - 1)) ? (Word)low : (Word)(low | ~mask);
}

static Word ct_load(void* address, int kind) {
    uint8_t b1; uint16_t b2; uint32_t b4; uint64_t b8; float f;
    switch (kind & 15) {
        case 1: memcpy(&b1, address, 1); return ct_fit(b1, kind);
        case 2: memcpy(&b2, address, 2); return ct_fit(b2, kind);
        case 4:
            memcpy(&b4, address, 4);
            memcpy(&f, address, 4);
            return kind & CT_FLOAT ? ct_bits(f) : ct_fit(b4, kind);
        default: memcpy(&b8, address, 8); return (Word)b8;
    }
}

static void ct_store(void* address, int kind, Word a) {
    uint8_t b1 = (uint8_t)a; uint16_t b2 = (uint16_t)a; uint32_t b4 = (uint32_t)a; uint64_t b8 = (uint64_t)a;
    float f = kind & CT_FLOAT ? (float)ct_double(a) : 0;
    switch (kind & 15) {
        case 1: memcpy(address, &b1, 1); break;
        case 2: memcpy(address, &b2, 2); break;
        case 4: memcpy(address, kind & CT_FLOAT ? (void*)&f : (void*)&b4, 4); break;
        default: memcpy(address, &b8, 8); break;
    }
}

// Returns whether a type is that of a compile-time variable or member.
static int ct_member(Type* type) {
    return type && (type_canon(type)->quals & Q_COMPTIME) != 0;
}

// A type as a value is a pointer to something that has no definition.
static Type* ct_type_type(void) {
    static Type* type;
    if (type) return type;
    Type* thing = type_new(TY_STRUCT);
    thing->name = "@Type";
    thing->opaque = 1;
    thing->state = 2;
    return type = type_ptr(thing);
}

static int ct_is_type(Type* type) {
    Type* k = type_canon(type);
    return k->kind == TY_PTR && type_unq(type_canon(k->base)) == type_unq(ct_type_type()->base);
}

static int ct_bit_field;        // the member that ct_layout found last is a bit-field

// Returns the size in bytes of the values of a type, laid out as a C compiler
// lays them out. `align` receives their alignment, if it is asked for. With
// `member`, the type is a struct, and `offset` and `found` receive where the
// member of that name is in it and what its type is. Compile-time members
// lie behind the others, so that the others are where they are in C.
static long ct_layout(Ctx* c, Token at, Type* type, long* align, const char* member, long* offset, Type** found) {
    Type* k = type_canon(type);
    long unused, size = 0, widest = 1, bit = 0, c_size = 0;
    long unit_end = 0, unit_bits = 0;    // Microsoft's rule: the open storage unit of bit-fields
    if (!align) align = &unused;
    if (ct_kind(k)) return ct_c_size = *align = ct_kind(k) & 15;
    if (k->kind == TY_ARRAY && k->has_size_value) {
        size = ct_layout(c, at, k->base, align, NULL, NULL, NULL) * k->size_value;
        ct_c_size *= k->size_value;
        return size;
    }
    if (k->kind != TY_STRUCT) FAIL(c, at, "values of type '%s' cannot be held at compile time", type_show(type));
    Type* st = type_unq(k);
    struct_resolve(st);
    if (st->opaque) FAIL(c, at, "values of type '%s' cannot be held at compile time: its definition is not known", type_show(type));
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < st->nfields; i++) {
            Field* f = &st->fields[i];
            long field_align, inside = 0;
            if (f->verbatim || !f->type || ct_member(f->type) != pass) continue;
            long field_size = ct_layout(c, at, f->type, &field_align, NULL, NULL, NULL);
            long unit = field_size * 8;
            if (os_ms_bitfields && unit_bits && !st->is_union && (!f->bits || unit_bits != unit)) {
                // the open unit ends: what comes next starts after all of it
                bit = unit_end;
                unit_bits = 0;
            }
            long start = st->is_union ? 0 : (bit + field_align * 8 - 1) / (field_align * 8) * (field_align * 8);
            long width = field_size * 8;
            long place = start / 8;
            if (f->bits && os_ms_bitfields) {
                // Microsoft's rule: in the open unit if it is of this size and the
                // bit-field still fits, else in a new unit of its type
                width = strtol(f->bits, NULL, 0);
                if (st->is_union) {
                    start = 0;
                } else if (!width) {
                    // closes the open unit (one of another size is closed already)
                    start = unit_bits ? unit_end : bit;
                    unit_bits = 0;
                } else if (unit_bits && bit + width <= unit_end) {
                    start = bit;
                } else {
                    unit_end = start + unit;
                    unit_bits = unit;
                }
                place = (st->is_union || !unit_bits ? start : unit_end - unit) / 8;
            } else if (f->bits) {
                // a bit-field goes on where the last one ended, if it still fits in the same unit
                width = strtol(f->bits, NULL, 0);
                start = st->is_union ? 0 : bit;
                if (!width || start / unit != (start + width - 1) / unit)
                    start = (start + unit - 1) / unit * unit;
                place = start / unit * field_size;
            }
            if (member && f->name && strcmp(f->name, member) == 0) {
                *offset = place;
                *found = f->type;
                ct_bit_field = f->bits != NULL;
            }
            // the members of a struct without a name are members of the struct around it
            if (member && !f->name && !*found && type_canon(f->type)->kind == TY_STRUCT) {
                ct_layout(c, at, f->type, NULL, member, &inside, found);
                if (*found) *offset = place + inside;
            }
            if (!st->is_union) bit = start + width;
            long end = os_ms_bitfields && f->bits && unit_bits && !st->is_union ? unit_end : start + width;
            if (os_ms_bitfields && f->bits && st->is_union) end = unit;
            if ((end + 7) / 8 > size) size = (end + 7) / 8;
            if (field_align > widest) widest = field_align;
        }
        if (!pass) c_size = (size + widest - 1) / widest * widest;
    }
    *align = widest;
    ct_c_size = c_size;
    return (size + widest - 1) / widest * widest;
}

static long ct_size(Ctx* c, Token at, Type* type) {
    return ct_layout(c, at, type, NULL, NULL, NULL, NULL);
}

// Between two values on the stack lies a word that belongs to neither, so
// that a pointer which runs out of the value it points into is noticed.
static char ct_gaps[CT_STACK / 8];

// Makes room on the stack for a value, cleared, and returns where it is.
static long ct_room(Ctx* c, Token at, long bytes) {
    long place = ct_top, taken = bytes ? (bytes + 7) / 8 * 8 : 8;
    if (place + taken + 8 > CT_STACK) FAIL(c, at, "the compile-time values here take more than %d bytes", CT_STACK);
    ct_top += taken + 8;
    memset(ct_at(place), 0, (size_t)taken);
    memset(ct_gaps + place / 8, 0, (size_t)taken / 8);
    ct_gaps[(place + taken) / 8] = 1;
    return place;
}

// Returns whether `size` bytes at an address can be read. Only compile-time
// memory can. Nothing is read while an expression is only asked for its type.
static int ct_reads(Ctx* c, Token at, void* address, long size) {
    char* p = (char*)address;
    if (ct_dry) return 0;
    // A member or element of what a null pointer points to is a little past
    // null; no compile-time value is ever that low in memory
    if ((uintptr_t)p < CT_NULL_REACH) FAIL(c, at, "a null pointer is dereferenced at compile time");
    for (long word = (p - ct_at(0)) / 8; p >= ct_at(0) && p + size <= ct_at(CT_STACK); word++) {
        if (word * 8 >= p - ct_at(0) + (size ? size : 1)) return 1;
        if (ct_gaps[word]) FAIL(c, at, "a pointer is dereferenced at compile time outside the value it points into");
    }
    for (size_t i = 0; i < ct_kept.len; i += 2)
        if (p >= (char*)ct_kept.items[i] && p + size <= (char*)ct_kept.items[i] + (size_t)ct_kept.items[i + 1]) return 1;
    FAIL(c, at, "a pointer is dereferenced at compile time that does not point at a compile-time value");
}

// Returns whether `size` bytes at an address can be changed.
static int ct_writes(Ctx* c, Token at, void* address, long size) {
    char* p = (char*)address;
    if (!ct_reads(c, at, address, size)) return 0;
    if (p >= ct_at(g_ct.floor) && p < ct_at(CT_STACK)) return 1;
    if (p < ct_at(0) || p >= ct_at(CT_STACK))
        FAIL(c, at, "this cannot change: it keeps the value it was initialized with (it is a compile-time variable at file scope, "
                    "a string, a result, or a variable of the '@' function that emitted this code)");
    if (g_ct.running) FAIL(c, at, "an '@' call may only change variables that it created itself");
    FAIL(c, at, "a compile-time variable cannot change inside a branch or loop that is decided at runtime");
}

// Returns the type that a pointer points to, or an array is made of, or NULL
// for a type that is neither.
static Type* ct_pointee(Type* type) {
    Type* k = type_canon(type);
    return k->kind == TY_PTR || k->kind == TY_ARRAY ? k->base : NULL;
}

// Text as it stands between the quotes of a string literal
static const char* ct_quoted(const char* s) {
    Buf b = {0};
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') buf_printf(&b, "\\%c", *s);
        else if (*s == '\n') buf_puts(&b, "\\n");
        else if (*s == '\t') buf_puts(&b, "\\t");
        else if (*s == '\x01' || *s == '\x02') buf_putc(&b, *s);    // around a name that the generated C settles later
        else if ((unsigned char)*s < 32) buf_printf(&b, "\\%03o", (unsigned char)*s);
        else buf_putc(&b, *s);
    }
    return buf_str(&b);
}

// Writes the value at an address as Typed C, so that ordinary code can have
// it. With `braces` it initializes a variable, where a struct or an array is
// a list in braces. Elsewhere a struct is a compound literal, and an array
// cannot stand.
static const char* ct_text(Ctx* c, Token at, Type* type, void* address, int braces) {
    static const char* const suffix[BK_COUNT] = {
        [BK_UINT] = "U", [BK_LONG] = "L", [BK_ULONG] = "UL", [BK_LLONG] = "LL", [BK_ULLONG] = "ULL", [BK_FLOAT] = "f",
    };
    Type* named = type_unq(type);
    Type* k = type_canon(named);
    int kind = ct_kind(k);
    Word value = kind ? ct_load(address, kind) : 0;
    Buf b = {0};
    if (kind & CT_FLOAT) {
        // the shortest spelling that reads back as the same number
        const char* text = "";
        for (int digits = 6; digits <= 17; digits++) {
            text = xstrf("%.*g", digits, ct_double(value));
            if (strtod(text, NULL) == ct_double(value)) break;
        }
        return xstrf("%s%s%s", text, strpbrk(text, ".en") ? "" : ".0", suffix[k->builtin] ? suffix[k->builtin] : "");
    }
    if (k->kind == TY_PTR) {
        Type* to = type_canon(k->base);
        if (ct_is_type(k)) FAIL(c, at, "a type is a compile-time value only: it has no runtime form");
        if (!value) return "0";
        if (to->kind == TY_BUILTIN && to->builtin >= BK_CHAR && to->builtin <= BK_UCHAR && ct_reads(c, at, (void*)(intptr_t)value, 1))
            return xstrf("\"%s\"", ct_quoted((const char*)(intptr_t)value));
        FAIL(c, at, "a pointer to compile-time memory has no runtime form, so it cannot be used in ordinary code");
    }
    if (k->kind == TY_ENUM) {
        // an enumerator, if one has this value
        Type* e = type_unq(k);
        struct_resolve(e);
        long long has;
        for (int i = 0; i < e->nfields; i++)
            if (e->fields[i].name && enum_value(e, e->fields[i].name, &has) && has == value)
                return gen_enum_const(c, e, token_synth(e->fields[i].name, TK_IDENT, at)).s;
        return xstrf("(%s)%lld", type_decl(named, NULL, NULL, 0), (long long)value);
    }
    if (kind) {
        // plainly for an int or anything narrower, with a suffix where C has one, and with a cast otherwise
        int wide = k->builtin > BK_INT;
        const char* text = kind & CT_UNSIGNED ? xstrf("%llu", (unsigned long long)value) : xstrf("%lld", (long long)value);
        if (wide && named->kind == TY_BUILTIN && *text != '-') return xstrf("%s%s", text, suffix[k->builtin]);
        return wide ? xstrf("(%s)%s", type_decl(named, NULL, NULL, 0), text) : text;
    }
    buf_putc(&b, '{');
    if (k->kind == TY_ARRAY) {
        long step = ct_size(c, at, k->base);
        for (long i = 0; i < k->size_value; i++)
            buf_printf(&b, "%s%s", i ? ", " : "", ct_text(c, at, k->base, (char*)address + i * step, 1));
    } else {
        Type* st = type_unq(k);
        int written = 0;
        for (int i = 0; i < st->nfields; i++) {
            Field* f = &st->fields[i];
            long offset = 0;
            Type* found = NULL;
            if (f->verbatim || !f->type || !f->name || ct_member(f->type)) continue;
            ct_layout(c, at, k, NULL, f->name, &offset, &found);
            buf_printf(&b, "%s%s", written++ ? ", " : "", ct_text(c, at, f->type, (char*)address + offset, 1));
        }
        if (!written) buf_putc(&b, '0');
    }
    buf_putc(&b, '}');
    if (braces) return buf_str(&b);
    if (k->kind == TY_ARRAY) FAIL(c, at, "a compile-time array can only initialize a variable, or be indexed");
    return xstrf("(%s)%s", type_decl(named, NULL, NULL, 0), buf_str(&b));
}

// ---------------------------------------------------------------------------
// Evaluating expressions
// ---------------------------------------------------------------------------

static CtValue ct_eval(Ctx* c, Expr* e);
static CtValue ct_call(Ctx* c, Expr* call);
static CtValue ct_value(Ctx* c, Expr* e);
static int ct_known(Ctx* c, Expr* e);
static Expr* ct_named(Ctx* c, Expr* e);
static void ct_init(Ctx* c, char* address, Type* type, Expr* e);
static Type* ct_declared(Ctx* c, Expr* d);
static Token ct_field_member(Ctx* c, Expr* e);
static Word ct_field_kind(Type* type);
static int ct_implements(Ctx* c, Token at, Type* type, Expr* bound);
static void ct_start(SrcFile* f, Expr* start);
static void ct_function(SrcFile* f, Expr* def);
static SrcFile* ct_function_file(SrcFile* f, Func* fn);
static int ct_controlled(Expr* owner);
static List ct_controls;        // hand-written impls and traits with control among their members: the item, then its file
static void ct_emit(Ctx* c, Expr* s);
static void ct_unresolve(void);
static List ct_types;           // compile-time types that files declare: the declaration, then its file
static List ct_resolving;       // the types whose members were being resolved by something that may not get through
static int ct_quiet;            // a function body is being tried: what it misses may still be emitted

// Returns the compile-time variable that a name refers to in the checker's
// scope, or NULL if it refers to anything else.
static Local* ct_local(Ctx* c, Token name) {
    Local* l = local_find(c, L_VAR | L_ENUMCONST, tok_str(name));
    return l && l->kind == L_VAR && l->type && (type_canon(l->type)->quals & Q_COMPTIME) ? l : NULL;
}

// Returns the namespace that the name of a function or variable is looked up
// in: the one it is qualified with, or the one the code is in. `name`
// receives the name itself. A local variable of that name hides whatever a
// namespace has, so then there is no namespace to look in.
static Module* ct_space(Ctx* c, Expr* e, Token* name) {
    *name = e->kind == E_PATH ? e->blockstmt[e->blockstmt_size - 1]->token : e->token;
    if (e->kind == E_PATH) return e->blockstmt_size == 2 ? import_find(c->file, tok_str(e->blockstmt[0]->token)) : NULL;
    if (e->kind != E_IDENT || name->kind != TK_IDENT) return NULL;
    return local_find(c, L_VAR | L_ENUMCONST, tok_str(*name)) ? NULL : c->file->mod;
}

// Returns the compile-time variable at file scope that an expression names, or NULL.
static CtGlobal* ct_global(Ctx* c, Expr* e) {
    Token name;
    Module* mod = ct_space(c, e, &name);
    return mod ? (CtGlobal*)map_get(&ct_globals, xstrf("%s\x01%s", mod->name, tok_str(name))) : NULL;
}

// If an expression names an enumerator whose value typedc knows, returns the
// enum's type, and `value` receives the value. Otherwise returns NULL.
static Type* ct_enumerator(Ctx* c, Expr* e, long long* value) {
    Type* owner = NULL;
    Token name = e->token;
    if (e->kind == E_PATH) {
        if (ct_global(c, e)) return NULL;
        PathRef r = resolve_path(c, e);
        owner = r.enum_type;
        name = r.constant;
    } else if (e->kind == E_IDENT && name.kind == TK_IDENT) {
        Local* l = local_find(c, L_VAR | L_ENUMCONST, tok_str(name));
        Entity* ent = l ? NULL : scope_entity(c, tok_str(name));
        if (l && l->kind == L_ENUMCONST) owner = l->type;
        if (ent && ent->kind == ENT_ENUMCONST && !ent->twin) owner = ent->type;
    }
    return owner && enum_value(type_canon(owner), tok_str(name), value) ? owner : NULL;
}

// Returns whether a call is one of an '@' function, which is always run at
// compile time.
static int ct_at_call(Ctx* c, Expr* call) {
    Token name;
    Expr* callee = call->right;
    if (callee->kind != E_IDENT && callee->kind != E_PATH) return 0;
    ct_space(c, callee, &name);
    return name.kind == TK_IDENT && *name.start == '@';
}

// Returns whether an expression is an assignment, such as `a = b` or `a += b`.
static int ct_assigns(Expr* e) {
    Token op = e->token;
    return e->kind == E_BINARY && op.kind == TK_PUNCT && op.start[op.length - 1] == '=' &&
           !(op.length == 2 && strchr("=!<>", op.start[0]));
}

// Says how much is known about an expression in ordinary code before the
// program runs:
//   0  nothing, because it uses a runtime value
//   1  it consists of literals and enumerators only, so it is left as it was written
//   2  it uses compile-time values, so it is evaluated and replaced by its result
static int ct_known(Ctx* c, Expr* e) {
    Token op = e->token;
    int a, b = 1, d = 1;
    long long value;
    e = ct_named(c, e);
    switch (e->kind) {
        case E_COND: case E_CAST:
            return ct_known(c, e->left);
        case E_IDENT:
            if (op.kind != TK_IDENT) return op.kind == TK_CHAR || op.kind == TK_NUM || op.kind == TK_PP_NUM || op.kind == TK_STR;
            if (*op.start == '@' && !ct_spliced(op)) FAIL(c, op, "'%s' is an '@' function: it can only be called, and has no address", tok_str(op));
            if (ct_local(c, op) || ct_global(c, e)) return 2;
            if (ct_enumerator(c, e, &value)) return 1;
            // a constant generic argument is a compile-time value
            return !ct_spliced(op) && const_binding(c, tok_str(op)) && const_binding(c, tok_str(op))->has_value ? 2 : 0;
        case E_PATH:
            return ct_global(c, e) ? 2 : ct_enumerator(c, e, &value) ? 1 : 0;
        case E_CALL:
            if (!ct_at_call(c, e)) return 0;
            for (Expr* arg = e->left; arg; arg = arg->right)
                if (arg->left && !ct_known(c, arg->left))
                    FAIL(c, arg->left->token, "the arguments of an '@' call must be compile-time values or literals");
            return 2;
        case E_MEMBER:
            return ct_known(c, e->left);
        case E_INDEX:
            a = ct_known(c, e->right);
            b = ct_known(c, e->left);
            if (a == 2 && !b) FAIL(c, e->left->token, "the index into a compile-time array must be a compile-time value");
            break;
        case E_UNARY: case E_POSTFIX:
            a = ct_known(c, e->left);
            if (token_is(op, "++") || token_is(op, "--")) return a == 2 ? 2 : 0;
            if (token_kw(op, "sizeof") || token_is(op, "&") || token_is(op, "*")) return a == 2 ? 2 : 0;
            return op.length == 1 && strchr("-+~!", *op.start) ? a : 0;
        case E_TERNARY:
            a = ct_known(c, e->cases[1].condition);
            b = ct_known(c, e->cases[1].block);
            d = ct_known(c, e->cases[0].block);
            break;
        case E_BINARY:
            a = ct_known(c, e->left);
            b = ct_known(c, e->right);
            if (op.kind != TK_PUNCT) return 0;
            if (!ct_assigns(e)) break;
            if (a == 2 && !b) FAIL(c, op, "a compile-time variable can only be given compile-time values and literals");
            return a == 2 ? 2 : 0;
        default:
            return 0;
    }
    if (!a || !b || !d) return 0;
    return a == 2 || b == 2 || d == 2 ? 2 : 1;
}

// Returns the type that arithmetic on two numbers is done in.
static Type* ct_common(Type* a, Type* b) {
    Type* common = arith_type(a, b);
    return common->kind == TY_UNKNOWN ? type_builtin(BK_LLONG) : common;
}

// Returns a value as a variable of another type holds it, as an assignment
// or a cast makes it. A struct stays as it is, and an array becomes the
// address of its first element, which is what its value already is.
static Word ct_convert(Ctx* c, Token at, CtValue from, Type* to) {
    int have = ct_kind(from.type), want = ct_kind(to);
    if (ct_is_type(from.type) && want && !ct_is_type(to) && !(want & CT_TRUTH)) FAIL(c, at, "a type cannot become a value of type '%s'", type_show(to));
    if (!want || type_canon(from.type)->kind == TY_ARRAY) return from.v;
    if (!have) FAIL(c, at, "a value of type '%s' is needed here, and this is of type '%s'", type_show(to), type_show(from.type));
    if ((want & CT_FLOAT) && !(have & CT_FLOAT)) from.v = ct_bits(have & CT_UNSIGNED ? (double)(UWord)from.v : (double)from.v);
    if (!(want & CT_FLOAT) && (have & CT_FLOAT)) from.v = (Word)ct_double(from.v);
    return ct_fit(from.v, want);
}

static long long ct_char_value(Token tok) {
    const char* s = strchr(tok.start, '\'') + 1;
    if (*s != '\\') return (unsigned char)*s;
    if (s[1] == 'x') return strtol(s + 2, NULL, 16);
    if (isdigit((unsigned char)s[1])) return strtol(s + 1, NULL, 8);
    const char* found = strchr("n\nt\tr\ra\ab\bf\fv\v", s[1]);
    return found ? found[1] : s[1];
}

// Returns the characters of a string literal, as they are in memory.
static const char* ct_string(Token tok) {
    Buf b = {0};
    int inside = 0;
    for (size_t i = 0; i < tok.length; i++) {
        char ch = tok.start[i];
        if (ch == '"') { inside = !inside; continue; }      // adjacent literals are one token
        if (!inside) continue;
        if (ch == '\\' && i + 1 < tok.length) {
            const char* found = strchr("n\nt\tr\r0\0a\ab\bf\fv\v", tok.start[++i]);
            ch = found ? found[1] : tok.start[i];
        }
        buf_putc(&b, ch);
    }
    return (const char*)ct_keep(buf_str(&b), b.len + 1);
}

// Gives a compile-time variable at file scope its value. That is done the
// first time something uses the variable.
static void ct_global_value(Ctx* c, Token at, CtGlobal* g) {
    Ctx gc = ctx_for_file(g->file);
    Token name = g->decl->name;
    CtState outer = g_ct;
    long top = ct_top;
    int dry = ct_dry;
    if (g->state == 2) return;
    if (g->state == 1) FAIL(c, at, "the value of compile-time variable '%s' depends on itself", tok_str(name));
    if (!g->decl->left) FAIL(&gc, name, "compile-time variable '%s' is at file scope, so it has to be initialized", tok_str(name));
    g->state = 1;
    g->type = ct_declared(&gc, g->decl);
    // the value is computed on the stack and then kept
    g_ct = (CtState){ .unrolling = -1, .deciding = -1, .floor = top };
    ct_dry = 0;
    long size = ct_size(&gc, name, g->type);
    long place = ct_room(&gc, name, size);
    ct_init(&gc, ct_at(place), g->type, g->decl->left);
    g->memory = ct_keep(ct_at(place), (size_t)size);
    g_ct = outer;
    ct_top = top;
    ct_dry = dry;
    g->state = 2;
}

// Returns the count of a shift of a value of the given type: shifting by a
// negative count or by the type's width or more is undefined in C, and C
// compilers and processors give different results for it, so it is an error.
static int ct_shift_count(Ctx* c, Token at, Type* type, Word count) {
    int bits = (ct_kind(type) & 15) * 8;
    if (count < 0 || count >= bits)
        FAIL(c, at, "shift by %lld at compile time: '%s' has %d bits, so the count must be 0 to %d",
             (long long)count, type_show(type), bits, bits - 1);
    return (int)count;
}

// Applies a binary operator to two numbers of the same type.
static Word ct_operate(Ctx* c, Token at, const char* op, Type* type, Word a, Word b) {
    int kind = ct_kind(type), is_unsigned = kind & CT_UNSIGNED, twice = op[1] == op[0], equal = op[1] == '=';
    UWord ua = (UWord)a, ub = (UWord)b;
    double fa = ct_double(a), fb = ct_double(b);
    if (ct_dry) return 0;
    if (kind & CT_FLOAT) switch (*op) {
        case '+': return ct_bits(fa + fb);
        case '-': return ct_bits(fa - fb);
        case '*': return ct_bits(fa * fb);
        case '/': return ct_bits(fa / fb);
        case '=': return fa == fb;
        case '!': return fa != fb;
        case '<': if (!twice) return equal ? fa <= fb : fa < fb; break;
        case '>': if (!twice) return equal ? fa >= fb : fa > fb; break;
    } else switch (*op) {
        case '|': return a | b;
        case '^': return a ^ b;
        case '&': return a & b;
        case '+': return (Word)(ua + ub);
        case '-': return (Word)(ua - ub);
        case '*': return (Word)(ua * ub);
        case '=': return a == b;
        case '!': return a != b;
        case '/': case '%':
            if (!b) FAIL(c, at, "division by zero at compile time");
            if (!is_unsigned && b == -1 && a == INT64_MIN) FAIL(c, at, "a quotient does not fit at compile time");
            if (is_unsigned) return (Word)(*op == '/' ? ua / ub : ua % ub);
            return *op == '/' ? a / b : a % b;
        case '<':
            if (twice) return (Word)(ua << ct_shift_count(c, at, type, b));
            return is_unsigned ? (equal ? ua <= ub : ua < ub) : (equal ? a <= b : a < b);
        case '>':
            if (twice) {
                int count = ct_shift_count(c, at, type, b);
                return is_unsigned ? (Word)(ua >> count) : a >> count;
            }
            return is_unsigned ? (equal ? ua >= ub : ua > ub) : (equal ? a >= b : a > b);
    }
    FAIL(c, at, "operator '%s' cannot be applied to a value of type '%s' at compile time", op, type_show(type));
}

// Applies a binary operator to two values. Numbers are brought to the type
// they share. A pointer moves by whole elements, and two pointers are apart
// by so many elements.
static CtValue ct_arith(Ctx* c, Token at, const char* op, CtValue left, CtValue right) {
    Type* truth = type_builtin(BK_INT);
    Type* lp = ct_pointee(left.type);
    Type* rp = ct_pointee(right.type);
    int shift = (*op == '<' || *op == '>') && op[1] == op[0];
    int compares = !shift && strchr("=!<>", op[0]) && (op[1] == '=' || !op[1]);
    int plus = strcmp(op, "+") == 0, minus = strcmp(op, "-") == 0;
    if (lp || rp) {
        Type* pointer = type_ptr(lp ? lp : rp);
        long step = ct_size(c, at, lp ? lp : rp);
        if (compares) return (CtValue){ truth, ct_operate(c, at, op, pointer, left.v, right.v) };
        if (lp && rp && minus) return (CtValue){ g_ptrdiff_t, (left.v - right.v) / (step ? step : 1) };
        if (!(lp && !rp && (plus || minus)) && !(rp && !lp && plus))
            FAIL(c, at, "operator '%s' cannot be applied to these pointers at compile time", op);
        Word by = ct_convert(c, at, lp ? right : left, g_ptrdiff_t) * step;
        return (CtValue){ pointer, (lp ? left.v : right.v) + (plus ? by : -by) };
    }
    if (!ct_kind(left.type) || !ct_kind(right.type))
        FAIL(c, at, "operator '%s' cannot be applied to a value of type '%s' at compile time", op,
             type_show(ct_kind(left.type) ? right.type : left.type));
    Type* type = shift ? ct_common(left.type, truth) : ct_common(left.type, right.type);
    Word a = ct_convert(c, at, left, type);
    Word b = shift ? ct_convert(c, at, right, type_builtin(BK_LLONG)) : ct_convert(c, at, right, type);
    Word result = ct_operate(c, at, op, type, a, b);
    return compares ? (CtValue){ truth, result } : (CtValue){ type, ct_fit(result, ct_kind(type)) };
}

// Returns the value that is at an address: the number that is stored there,
// or for a struct or an array the address itself.
static CtValue ct_read(Ctx* c, Token at, char* address, Type* type) {
    int kind = ct_kind(type);
    type = type_unq(type);
    if (!kind) return (CtValue){ type, (Word)(intptr_t)address };
    return (CtValue){ type, ct_reads(c, at, address, kind & 15) ? ct_load(address, kind) : 0 };
}

// Stores a value at an address, as an assignment to a variable of the given
// type does, and returns what the variable then holds.
static CtValue ct_assign(Ctx* c, Token at, char* address, Type* type, CtValue value) {
    int kind = ct_kind(type);
    long size = ct_size(c, at, type);
    Word stored = ct_convert(c, at, value, type);
    if (!kind) {
        if (ct_kind(value.type)) FAIL(c, at, "a value of type '%s' is needed here, and this is of type '%s'", type_show(type), type_show(value.type));
        if (ct_writes(c, at, address, size) && ct_reads(c, at, (void*)(intptr_t)value.v, size))
            memmove(address, (void*)(intptr_t)value.v, (size_t)size);
        return (CtValue){ type, (Word)(intptr_t)address };
    }
    if (ct_writes(c, at, address, size)) ct_store(address, kind, stored);
    return (CtValue){ type, stored };
}

// Reports a variable that ordinary code changes inside a branch or loop that
// is decided at runtime, by its name.
static void ct_changing(Ctx* c, Expr* e) {
    Local* l = e->kind == E_IDENT && e->token.kind == TK_IDENT && !ct_spliced(e->token) ? ct_local(c, e->token) : NULL;
    if (l && !g_ct.running && l->at >= ct_at(0) && l->at < ct_at(g_ct.floor))
        FAIL(c, e->token, "compile-time variable '%s' cannot change inside a branch or loop that is decided at runtime", l->name);
}

// Returns whether an expression is a place in memory: a variable, a member,
// an element, or what a pointer points to.
static int ct_is_place(Expr* e) {
    while (e->kind == E_COND) e = e->left;
    return (e->kind == E_IDENT && e->token.kind == TK_IDENT) || e->kind == E_PATH || e->kind == E_MEMBER || e->kind == E_INDEX ||
           (e->kind == E_UNARY && token_is(e->token, "*"));
}

// Returns the address of a place, and `type` receives the type of what is there.
static char* ct_place(Ctx* c, Expr* e, Type** type) {
    while (e->kind == E_COND) e = e->left;
    e = ct_named(c, e);
    Token op = e->token;
    if (e->kind == E_MEMBER) {
        // the member of a struct, or of the struct a pointer points to
        Type* base;
        char* address;
        long offset = 0;
        if (token_is(op, "->")) {
            CtValue pointer = ct_eval(c, e->left);
            base = ct_pointee(pointer.type);
            address = (char*)(intptr_t)pointer.v;
        } else if (ct_is_place(e->left)) {
            address = ct_place(c, e->left, &base);
        } else {
            CtValue value = ct_eval(c, e->left);
            base = value.type;
            address = (char*)(intptr_t)value.v;
        }
        if (!base || type_canon(base)->kind != TY_STRUCT) FAIL(c, e->name, "'%s%s' needs a struct on its left", tok_str(op), tok_str(e->name));
        *type = NULL;
        ct_layout(c, e->name, base, NULL, tok_str(e->name), &offset, type);
        if (!*type) FAIL(c, e->name, "type '%s' has no member '%s'", type_show(base), tok_str(e->name));
        if (ct_bit_field) FAIL(c, e->name, "bit-field '%s' cannot be read or changed at compile time", tok_str(e->name));
        // Worked out as a number: the address may be null, as in the offsetof
        // idiom `&((T*)0)->member`. Only reading or writing there fails (ct_reads).
        return (char*)((uintptr_t)address + (uintptr_t)offset);
    }
    if (e->kind == E_INDEX) {
        // an array is where its first element is, and a pointer holds the address of one
        CtValue base = ct_eval(c, e->right);
        Type* k = type_canon(base.type);
        Word index = ct_convert(c, op, ct_eval(c, e->left), g_ptrdiff_t);
        if (!(*type = ct_pointee(base.type))) FAIL(c, op, "a value of type '%s' cannot be indexed", type_show(base.type));
        if (k->kind == TY_ARRAY && k->has_size_value && !ct_dry && (index < 0 || index >= k->size_value))
            FAIL(c, op, "index %lld is outside an array of %lld", (long long)index, (long long)k->size_value);
        // likewise worked out as a number, for any pointer
        return (char*)((uintptr_t)base.v + (uintptr_t)(index * ct_size(c, op, *type)));
    }
    if (e->kind == E_UNARY) {
        CtValue pointer = ct_eval(c, e->left);
        if (!(*type = ct_pointee(pointer.type))) FAIL(c, op, "only a pointer can be dereferenced");
        return (char*)(intptr_t)pointer.v;
    }
    Local* l = e->kind == E_IDENT ? ct_local(c, op) : NULL;
    CtGlobal* g = l ? NULL : ct_global(c, e);
    if (l) {
        *type = l->type;
        return l->at;
    }
    if (!g) FAIL(c, op, "a compile-time value is needed here, and '%s' is only known when the program runs", tok_str(op));
    ct_global_value(c, op, g);
    *type = g->type;
    return (char*)g->memory;
}

// Evaluates a compile-time expression.
static CtValue ct_eval(Ctx* c, Expr* e) {
    Token op = e->token;
    const char* text = tok_str(op);
    size_t n = strlen(text);
    Type* truth = type_builtin(BK_INT);
    Type* type;
    long long value;
    if (e->kind == E_COND) return ct_eval(c, e->left);
    if (ct_is_place(e)) {
        // An enumerator and a constant generic argument are numbers. Anything
        // else that names a place is what is in that place.
        int named = (e->kind == E_IDENT && !ct_local(c, op) && !ct_global(c, e)) || (e->kind == E_PATH && !ct_global(c, e));
        Type* owner = named ? ct_enumerator(c, e, &value) : NULL;
        Type* constant = named && e->kind == E_IDENT && !ct_spliced(op) ? const_binding(c, text) : NULL;
        if (owner) return (CtValue){ owner, value };
        if (constant && constant->has_value) return (CtValue){ constant->base, constant->value };
        char* address = ct_place(c, e, &type);
        return ct_read(c, op, address, type);
    }
    switch (e->kind) {
        case E_COND:
            return ct_eval(c, e->left);
        case E_IDENT:
            if (op.kind == TK_STR) return (CtValue){ g_char_ptr, (Word)(intptr_t)ct_string(op) };
            if (op.kind == TK_CHAR) return (CtValue){ truth, ct_char_value(op) };
            type = number_type(op);
            return (CtValue){ type, ct_kind(type) & CT_FLOAT ? ct_bits(strtod(text, NULL)) : (Word)strtoull(text, NULL, 0) };
        case E_SIZEOF_TYPE:
            return (CtValue){ g_size_t, ct_size(c, op, resolve_type(c, e->type)) };
        case E_CAST:
            type = resolve_type(c, e->type);
            return (CtValue){ type, ct_convert(c, op, ct_eval(c, e->left), type) };
        case E_COMPOUND: {
            // a value that is made where it is written, in room of its own
            type = resolve_type(c, e->type);
            char* address = ct_at(ct_room(c, op, ct_size(c, op, type)));
            ct_init(c, address, type, e->left);
            return (CtValue){ type, (Word)(intptr_t)address };
        }
        case E_CALL:
            return ct_call(c, e);
        case E_TERNARY: {
            // The result has the type the two branches share, so the branch
            // that is not taken is still asked for its type.
            CtValue condition = ct_eval(c, e->cases[1].condition);
            int yes = ct_convert(c, op, condition, type_builtin(BK_BOOL)) != 0;
            CtValue taken = ct_eval(c, e->cases[yes].block);
            ct_dry++;
            CtValue other = ct_eval(c, e->cases[!yes].block);
            ct_dry--;
            if (!ct_kind(taken.type) || !ct_kind(other.type) || ct_pointee(taken.type) || ct_pointee(other.type)) return taken;
            type = ct_common(taken.type, other.type);
            return (CtValue){ type, ct_convert(c, op, taken, type) };
        }
        case E_UNARY: case E_POSTFIX: {
            if (token_is(op, "++") || token_is(op, "--")) {
                // i++ yields the value i had before
                ct_changing(c, e->left);
                char* address = ct_place(c, e->left, &type);
                CtValue before = ct_read(c, op, address, type);
                if (!ct_kind(type)) FAIL(c, op, "'%s' cannot be applied to a value of type '%s'", text, type_show(type));
                CtValue after = ct_assign(c, op, address, type, ct_arith(c, op, *text == '+' ? "+" : "-", before, (CtValue){ truth, 1 }));
                return e->kind == E_UNARY ? after : before;
            }
            if (token_is(op, "&")) {
                char* address = ct_place(c, e->left, &type);
                return (CtValue){ type_ptr(type), (Word)(intptr_t)address };
            }
            if (token_kw(op, "sizeof")) {
                // the operand is only asked for its type
                ct_dry++;
                type = ct_eval(c, e->left).type;
                ct_dry--;
                return (CtValue){ g_size_t, ct_size(c, op, type) };
            }
            CtValue operand = ct_eval(c, e->left);
            int kind = ct_kind(operand.type);
            if (!kind) FAIL(c, op, "'%s' cannot be applied to a value of type '%s'", text, type_show(operand.type));
            if (*text == '!') return (CtValue){ truth, kind & CT_FLOAT ? ct_double(operand.v) == 0 : operand.v == 0 };
            if (ct_pointee(operand.type)) FAIL(c, op, "'%s' cannot be applied to a value of type '%s'", text, type_show(operand.type));
            type = ct_common(operand.type, truth);
            CtValue promoted = { type, ct_convert(c, op, operand, type) };
            if (*text == '+') return promoted;
            CtValue minus_one = { type, ct_kind(type) & CT_FLOAT ? ct_bits(-1) : -1 };
            return ct_arith(c, op, *text == '~' ? "^" : "*", promoted, minus_one);
        }
        case E_BINARY: {
            if (ct_assigns(e)) {
                ct_changing(c, e->left);
                char* address = ct_place(c, e->left, &type);
                if (n > 1) {
                    CtValue before = ct_read(c, op, address, type);
                    return ct_assign(c, op, address, type, ct_arith(c, op, xstrn(text, n - 1), before, ct_eval(c, e->right)));
                }
                ct_init(c, address, type, e->right);
                return ct_read(c, op, address, type);
            }
            CtValue left = ct_eval(c, e->left);
            if (*text == ',') return ct_eval(c, e->right);
            if (strcmp(text, "&&") == 0 || strcmp(text, "||") == 0) {
                // the right operand is only evaluated when the left one does not decide the result
                int decided = (ct_convert(c, op, left, type_builtin(BK_BOOL)) != 0) == (*text == '|');
                ct_dry += decided;
                Word second = ct_convert(c, op, ct_eval(c, e->right), type_builtin(BK_BOOL));
                ct_dry -= decided;
                return (CtValue){ truth, decided ? *text == '|' : second };
            }
            return ct_arith(c, op, text, left, ct_eval(c, e->right));
        }
        default:
            FAIL(c, op, "this cannot be carried out at compile time");
    }
}

// Gives the variable at an address its first value. The value is an
// expression, or a list in braces with a value for each member or element.
static void ct_init(Ctx* c, char* address, Type* type, Expr* e) {
    Type* k = type_canon(type);
    long size = ct_size(c, e->token, type);
    if (k->kind == TY_ARRAY && e->kind == E_IDENT && e->token.kind == TK_STR) {
        // an array of characters takes as much of a string as it has room for
        const char* text = ct_string(e->token);
        long length = (long)strlen(text) + 1;
        if (!ct_writes(c, e->token, address, size)) return;
        memset(address, 0, (size_t)size);
        memcpy(address, text, (size_t)(length < size ? length : size));
        return;
    }
    if (e->kind != E_INIT_LIST) {
        ct_assign(c, e->token, address, type, ct_eval(c, e));
        return;
    }
    if (k->kind != TY_STRUCT && k->kind != TY_ARRAY) FAIL(c, e->token, "a list in braces needs a struct or an array to go into");
    if (ct_writes(c, e->token, address, size)) memset(address, 0, (size_t)size);
    Type* st = type_unq(k);
    int position = 0;
    for (size_t i = 0; i < e->blockstmt_size; i++) {
        Expr* item = e->blockstmt[i];
        Expr* designator = item->kind == E_INIT_ITEM ? item->left : NULL;
        Type* into = k->base;
        long offset = 0;
        Token named = designator && designator->aux ? ct_field_member(c, designator->left) : designator ? designator->name : (Token){0};
        if (designator && (k->kind != TY_STRUCT || !named.length || designator->right))
            FAIL(c, item->token, "only '.member = value' is handled in a list in braces at compile time");
        if (k->kind == TY_ARRAY) {
            if (position >= k->size_value) FAIL(c, item->token, "this array has no element %d", position);
            offset = position++ * ct_size(c, item->token, k->base);
        } else {
            // without a name, the next member is meant
            while (!designator && position < st->nfields && (st->fields[position].verbatim || !st->fields[position].name)) position++;
            const char* member = designator ? tok_str(named) : position < st->nfields ? st->fields[position++].name : NULL;
            into = NULL;
            if (member) ct_layout(c, item->token, type, NULL, member, &offset, &into);
            if (!into) FAIL(c, item->token, "type '%s' has no member for this value", type_show(type));
        }
        ct_init(c, address + offset, into, designator ? item->right : item);
    }
}

// Returns the type of a compile-time variable that is being declared. An
// array without a size takes the size of what it is initialized with.
static Type* ct_declared(Ctx* c, Expr* d) {
    Type* type = resolve_type(c, d->type);
    Type* k = type_canon(type);
    int text = d->left && d->left->kind == E_IDENT && d->left->token.kind == TK_STR;
    if (k->kind == TY_ARRAY && !k->has_size_value && !k->size && d->left && (text || d->left->kind == E_INIT_LIST)) {
        Type* sized = type_new(TY_ARRAY);
        *sized = *k;
        sized->unq = NULL;
        sized->has_size_value = 1;
        sized->size_value = text ? (long long)strlen(ct_string(d->left->token)) + 1 : (long long)d->left->blockstmt_size;
        sized->size = xstrf("%lld", sized->size_value);
        type = sized;
    }
    else if (k->kind == TY_ARRAY && !k->has_size_value && !k->size && d->left) {
        // or the size of the array it is initialized with, which is only asked for its type here
        ct_dry++;
        Type* from = ct_eval(c, d->left).type;
        ct_dry--;
        if (type_canon(from)->kind == TY_ARRAY) type = from;
    }
    ct_size(c, d->name, type);      // reports a type that cannot be held
    return type_qual(type, Q_COMPTIME);
}

// ---------------------------------------------------------------------------
// Names and types that are compile-time values
// ---------------------------------------------------------------------------

static const char* ct_str(const char* text) {
    return (const char*)ct_keep(text, strlen(text) + 1);
}

// Returns the type of a given name from the namespace `builtin`.
static Type* ct_builtin_type(Ctx* c, Token at, const char* name) {
    Module* m = (Module*)map_get(&g_module_by_name, "builtin");
    Entity* e = m ? (Entity*)map_get(&m->names, name) : NULL;
    if (!e) FAIL(c, at, "'%s' gives values from the namespace 'builtin': add 'import builtin;'", tok_str(at));
    return entity_type(e);
}

// Reads and writes a member of a struct that is at an address.
static Word ct_get(Ctx* c, Token at, char* record, Type* type, const char* member) {
    long offset = 0;
    Type* found = NULL;
    ct_layout(c, at, type, NULL, member, &offset, &found);
    return ct_load(record + offset, ct_kind(found));
}

static void ct_set(Ctx* c, Token at, char* record, Type* type, const char* member, Word value) {
    long offset = 0;
    Type* found = NULL;
    ct_layout(c, at, type, NULL, member, &offset, &found);
    ct_store(record + offset, ct_kind(found), value);
}

// Returns the name that an @ident(...) stands for: its arguments, which are
// compile-time strings and integers, joined.
static Token ct_ident(Ctx* c, Token name) {
    Expr* args = (Expr*)map_get(&ct_splices, xstrf("%p", (void*)name.start));
    Buf b = {0};
    for (size_t i = 0; args && i < args->blockstmt_size; i++) {
        Expr* arg = args->blockstmt[i];
        if (!g_ct.running && !ct_known(c, arg)) FAIL(c, arg->token, "the arguments of '@ident' must be compile-time strings and integers");
        CtValue v = ct_value(c, arg);
        Type* to = ct_pointee(v.type);
        if (to && ct_kind(to) && (ct_kind(to) & 15) == 1 && ct_reads(c, arg->token, (void*)(intptr_t)v.v, 1)) buf_puts(&b, (const char*)(intptr_t)v.v);
        else if (ct_kind(v.type) && !to && !(ct_kind(v.type) & CT_FLOAT)) buf_printf(&b, "%lld", (long long)v.v);
        else FAIL(c, arg->token, "the arguments of '@ident' must be compile-time strings and integers");
    }
    const char* text = buf_str(&b);
    int valid = isalpha((unsigned char)*text) || *text == '_';
    for (const char* s = text; *s; s++) valid = valid && (isalnum((unsigned char)*s) || *s == '_');
    if (!valid) FAIL(c, name, "'@ident' made '%s', which is not a valid identifier", text);
    return token_synth(text, TK_IDENT, name);
}

// Returns the name of the member that the `f` of `.[f]` describes.
static Token ct_field_member(Ctx* c, Expr* e) {
    static const char* const rule = "'.[...]' needs a compile-time 'builtin::Field' that came from '@fields'";
    if (!g_ct.running && ct_known(c, e) != 2) FAIL(c, e->token, "%s", rule);
    CtValue f = ct_value(c, e);
    Type* field = ct_builtin_type(c, e->token, "Field");
    char* record = (char*)(intptr_t)f.v;
    if (type_unq(type_canon(f.type)) != type_unq(type_canon(field)) || !ct_get(c, e->token, record, field, "__of")) FAIL(c, e->token, "%s", rule);
    return token_synth((const char*)(intptr_t)ct_get(c, e->token, record, field, "name"), TK_IDENT, e->token);
}

// Returns an expression in which every `.[f]` and every @ident(...) that the
// checker would trip over is the name it stands for: the expression itself if
// it has none, and a copy otherwise.
static Expr* ct_named(Ctx* c, Expr* e) {
    Expr** inner = e->kind == E_MEMBER || e->kind == E_POSTFIX ? &e->left : e->kind == E_CALL || e->kind == E_INDEX ? &e->right : NULL;
    Expr* named = inner ? ct_named(c, *inner) : NULL;
    int by_field = e->kind == E_MEMBER && e->right;
    int spliced = (e->kind == E_MEMBER && ct_spliced(e->name)) || (e->kind == E_IDENT && ct_spliced(e->token));
    if ((!inner || named == *inner) && !by_field && !spliced) return e;
    Expr* copy = (Expr*)xalloc(sizeof(Expr));
    *copy = *e;
    if (inner) *(e->kind == E_CALL || e->kind == E_INDEX ? &copy->right : &copy->left) = named;
    if (by_field) copy->name = ct_field_member(c, e->right), copy->right = NULL;
    if (spliced && e->kind == E_MEMBER) copy->name = ct_ident(c, e->name);
    if (spliced && e->kind == E_IDENT) copy->token = ct_ident(c, e->token);
    return copy;
}

// The type that `@Type` and `@type(e)` are.
static Type* resolve_ct_type(Ctx* c, Expr* s) {
    static const char* const rule = "'@type(...)' needs a compile-time value of type '@Type'";
    if (s->sema) return (Type*)s->sema;
    if (!s->left) return ct_type_type();
    if (!g_ct.running && ct_known(c, s->left) != 2) FAIL(c, s->token, "%s", rule);
    CtValue v = ct_value(c, s->left);
    if (!ct_is_type(v.type) || !v.v) FAIL(c, s->token, "%s", rule);
    return (Type*)(intptr_t)v.v;
}

// If a generic argument is a compile-time value of type `@Type`, returns the
// type it denotes, and NULL otherwise.
static Type* resolve_ct_arg(Ctx* c, Expr* arg) {
    if (arg->kind >= E_TY_BASE && arg->kind <= E_TY_HOLE) return NULL;
    int variable = arg->kind == E_IDENT && arg->token.kind == TK_IDENT && ct_local(c, arg->token);
    if (!variable && (arg->kind == E_IDENT || arg->kind == E_PATH)) return NULL;       // the name of a type
    if (!g_ct.running && ct_known(c, arg) != 2) return NULL;
    CtValue v = ct_value(c, arg);
    return ct_is_type(v.type) && v.v ? (Type*)(intptr_t)v.v : NULL;
}

// Returns the type that a generic argument of a predefined function denotes.
static Type* ct_type_arg(Ctx* c, Expr* arg) {
    Type* known = resolve_ct_arg(c, arg);
    Expr* syntax = known ? NULL : type_argument(arg);
    if (!known && !syntax) FAIL(c, arg->token, "a type is needed here");
    return known ? known : resolve_type(c, syntax);
}

// Carries out a call of a predefined '@' function, which says something
// about a type. Returns whether the name is that of one.
static int ct_builtin(Ctx* c, Expr* call, Token at, CtValue* out) {
    static const char* const names[] = {
        "@fields", "@count", "@name", "@mangled", "@kind", "@implements", "@pointee", "@enumerators", "@generics"
    };
    enum { FIELDS, COUNT, NAME, MANGLED, KIND, IMPLEMENTS, POINTEE, ENUMERATORS, GENERICS, NONE };
    int which = 0;
    while (which < NONE && !token_is(at, names[which])) which++;
    if (which == NONE) return 0;
    Expr* targs = call->right->generics;
    int want = which == IMPLEMENTS ? 2 : 1;
    if (generics_count(targs) != want || (call->left && call->left->left))
        FAIL(c, at, "'%s' takes %d type argument(s) and nothing else, as in '%s<...>()'", names[which], want, names[which]);
    // '@generics' also takes a function with its generic arguments
    Expr* first = targs->blockstmt[0];
    Entity* ent = which != GENERICS ? NULL : first->kind == E_PATH ? resolve_path(c, first).ent :
                  first->kind == E_IDENT && first->token.kind == TK_IDENT && !ct_local(c, first->token) ? scope_entity(c, tok_str(first->token)) : NULL;
    Func* function = ent && ent->kind == ENT_FUNC ? ent->func : NULL;
    Type** bound = function ? resolve_func_args(c, function, first->generics, at) : NULL;
    Type* type = function ? type_builtin(BK_VOID) : ct_type_arg(c, first);
    Type* k = type_canon(type);
    Type* u = type_unq(k);
    Type* named = function ? (Type*)(void*)bound : type_unq(type);
    // a generic typedef of something that is not a struct keeps its arguments in the name it gives
    Type* inst = !function && type_unq(type)->is_inst ? type_unq(type) : u;
    Type* text = type_ptr(type_qual(type_builtin(BK_CHAR), Q_CONST));
    Type* record = NULL;
    const char* key = xstrf("%d %p", which, (void*)named);
    CtValue* known = which == IMPLEMENTS ? NULL : (CtValue*)map_get(&ct_reflected, key);
    long size = 0, n = 0;
    char* memory = NULL;
    if (known) return *out = *known, 1;
    if (which == FIELDS || which == ENUMERATORS || which == GENERICS) {
        // these return an array of records, one for each thing the type has
        record = ct_builtin_type(c, at, which == FIELDS ? "Field" : which == ENUMERATORS ? "Enumerator" : "Generic");
        size = ct_size(c, at, record);
        if (k->kind == TY_STRUCT || k->kind == TY_ENUM) struct_resolve(u);
        memory = (char*)xalloc((size_t)((u->nfields + inst->nargs + generics_count(function ? function->generics : NULL) + 1) * size));
    }
    switch (which) {
        case FIELDS: case COUNT:
            if (k->kind != TY_STRUCT) FAIL(c, at, "'%s' needs a struct or union type, but '%s' is not one", names[which], type_show(type));
            struct_resolve(u);
            for (int i = 0; i < u->nfields; i++) {
                Field* f = &u->fields[i];
                char* r = memory + n * size;
                long offset = 0;
                Type* found = NULL;
                if (f->verbatim || !f->type || !f->name || ct_member(f->type)) continue;
                n++;
                if (which == COUNT) continue;
                ct_layout(c, at, type, NULL, f->name, &offset, &found);
                ct_size(c, at, f->type);
                ct_set(c, at, r, record, "size", ct_c_size);
                ct_set(c, at, r, record, "name", (Word)(intptr_t)ct_str(f->name));
                ct_set(c, at, r, record, "type_name", (Word)(intptr_t)ct_str(type_show(f->type)));
                ct_set(c, at, r, record, "offset", offset);
                ct_set(c, at, r, record, "bits", f->bits ? strtol(f->bits, NULL, 0) : 0);
                ct_set(c, at, r, record, "kind", ct_field_kind(f->type));
                ct_set(c, at, r, record, "type", (Word)(intptr_t)f->type);
                ct_set(c, at, r, record, "__of", (Word)(intptr_t)type);
            }
            if (which == COUNT) *out = (CtValue){ type_builtin(BK_INT), n };
            break;
        case ENUMERATORS:
            if (k->kind != TY_ENUM) FAIL(c, at, "'@enumerators' needs an enum type, but '%s' is not one", type_show(type));
            for (int i = 0; i < u->nfields; i++) {
                long long value = 0;
                if (!u->fields[i].name) continue;
                if (!enum_value(u, u->fields[i].name, &value))
                    FAIL(c, at, "'@enumerators' needs the value of '%s' in '%s', which is not known at compile time",
                         u->fields[i].name, type_show(type));
                ct_set(c, at, memory + n * size, record, "name", (Word)(intptr_t)ct_str(u->fields[i].name));
                ct_set(c, at, memory + n++ * size, record, "value", value);
            }
            break;
        case GENERICS:
            for (int i = 0; i < (function ? generics_count(function->generics) : inst->is_inst && inst->generic ? inst->nargs : 0); i++) {
                // a constant parameter has a declared type and a value, a type parameter is bound to a type
                Expr* param = (function ? function->generics : inst->generic->decl->generics)->blockstmt[i];
                Type* arg = function ? bound[i] : inst->args[i];
                Type* shown = param->type ? arg->base : arg;
                char* r = memory + n++ * size;
                ct_set(c, at, r, record, "name", (Word)(intptr_t)ct_str(tok_str(param->name)));
                ct_set(c, at, r, record, "type_name", (Word)(intptr_t)ct_str(type_show(shown)));
                ct_set(c, at, r, record, "type", (Word)(intptr_t)shown);
                if (param->type) ct_set(c, at, r, record, "value", (Word)(intptr_t)ct_str(arg->has_value ? xstrf("%lld", arg->value) : arg->name));
            }
            break;
        case NAME: case MANGLED: {
            const char* name = named->is_inst && named->generic ? named->generic->name : named->name ? named->name : named->cname;
            if (!name) name = type_show(type);
            if (which == MANGLED && named->sym) name = symbol_ref(named->sym);
            *out = (CtValue){ text, (Word)(intptr_t)ct_str(name) };
            break;
        }
        case KIND:
            *out = (CtValue){ ct_builtin_type(c, at, "FieldKind"), ct_field_kind(type) };
            break;
        case POINTEE:
            if (k->kind != TY_PTR) FAIL(c, at, "'@pointee' needs a pointer type, but '%s' is not one", type_show(type));
            *out = (CtValue){ ct_type_type(), (Word)(intptr_t)k->base };
            break;
        case IMPLEMENTS: {
            Expr* bound = type_argument(targs->blockstmt[1]);
            if (!bound) FAIL(c, targs->blockstmt[1]->token, "the second argument of '@implements' must be a trait");
            *out = (CtValue){ type_builtin(BK_INT), ct_implements(c, at, type, bound) };
            return 1;
        }
    }
    if (record) {
        Type* array = type_new(TY_ARRAY);
        array->base = record;
        array->has_size_value = 1;
        array->size_value = n;
        array->size = xstrf("%ld", n);
        *out = (CtValue){ array, (Word)(intptr_t)ct_keep(memory, (size_t)(n * size)) };
    }
    known = (CtValue*)xalloc(sizeof(CtValue));
    *known = *out;
    map_put(&ct_reflected, key, known);
    return 1;
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

// Runs the function that a call names, for the arguments it is given. That
// is an '@' function, or, where the call is itself in a function that is
// being run, any function or method written in Typed C. The parameters are
// the function's first variables, and its body is carried out statement by
// statement. A finished call of an '@' function is not run again for the
// same arguments.
static CtValue ct_call(Ctx* c, Expr* call) {
    Expr* callee = ct_named(c, call->right);
    Token at = callee->token, name = at;
    Func* f = NULL;
    Type** ivals = NULL;
    Type** own = NULL;
    Type* self = NULL;
    CtValue args[MAX_ARGS + 1], predefined;
    Val shown[MAX_ARGS + 1];
    int argc = 0, offset = 0;
    long top = ct_top;
    if (callee->kind == E_MEMBER && token_is(callee->token, ".")) {
        // value.method(arguments): the value is the first argument
        if (!g_ct.running) FAIL(c, at, "a method is only run at compile time when a function that is being run calls it");
        args[argc++] = ct_eval(c, callee->left);
        at = callee->name;
        f = find_method(c, type_unq(args[0].type), tok_str(at), at, &ivals);
        self = impl_self(f->impl, ivals);
        offset = 1;
    } else if ((callee->kind == E_IDENT && at.kind == TK_IDENT) || callee->kind == E_PATH) {
        Module* mod = ct_space(c, callee, &name);
        at = name;
        if (*name.start == '@') {
            if (ct_builtin(c, call, name, &predefined)) return predefined;
            f = mod ? (Func*)map_get(&ct_funcs, xstrf("%s\x01%s", mod->name, tok_str(name))) : NULL;
            if (!f) FAIL(c, name, "there is no '@' function named '%s' here", tok_str(name));
        } else if (g_ct.running && callee->kind == E_PATH) {
            PathRef r = resolve_path(c, callee);
            if (r.ent && r.ent->kind == ENT_FUNC) f = r.ent->func;
        } else if (g_ct.running && mod) {
            Entity* ent = scope_entity(c, tok_str(name));
            if (ent && ent->kind == ENT_FUNC) f = ent->func;
        }
    }
    if (!f || !f->def) FAIL(c, at, "'%s' cannot be called at compile time: typedc has no definition of it to run", tok_str(at));
    for (Expr* arg = call->left; arg; arg = arg->right) {
        if (!arg->left) continue;
        if (argc == MAX_ARGS) FAIL(c, at, "too many arguments");
        args[argc++] = ct_eval(c, arg->left);
    }

    // the generic arguments are written out, or follow from the types of the arguments
    int ngen = generics_count(f->generics);
    if (callee->generics) {
        own = resolve_func_args(c, f, callee->generics, at);
    } else if (ngen) {
        for (int i = 0; i < argc; i++)
            shown[i] = val("", type_canon(args[i].type)->kind == TY_ARRAY ? type_ptr(type_canon(args[i].type)->base) : args[i].type);
        own = infer_generics(c, f, ivals, self, shown + offset, argc - offset, offset, NULL, at, 0);
    }
    if (*f->name == '@' && !map_has(&ct_brings, xstrf("%p", (void*)f))) f->file = ct_function_file(f->file, f);
    Ctx fc = ctx_for_file(f->file);
    fc.tenv = tenv_bind(f->generics, own, f->impl ? tenv_bind(f->impl->decl->generics, ivals, NULL) : NULL);
    fc.self = self;
    fc.in_func = 1;
    Type* type = type_canon(resolve_type(&fc, f->def->type));
    Type* result = type_canon(type->base);
    if (argc != type->nargs) FAIL(c, at, "'%s' expects %d argument(s), %d given", f->name, type->nargs - offset, argc - offset);
    if (ct_dry) return (CtValue){ type->base, 0 };

    // The call is known by its function, its generic arguments, the namespace
    // it is used from, and its arguments.
    CtState outer = g_ct;
    SrcFile* use = outer.running ? outer.use : c->file;
    int at_call = *f->name == '@';
    long floor = ct_top;
    Buf key = {0};
    buf_printf(&key, "%p %s", (void*)f, use->mod->name);
    for (int i = 0; i < ngen; i++) buf_putc(&key, ' '), type_key_into(&key, own[i]);
    for (int i = 0; f->impl && i < f->impl->nparams; i++) buf_putc(&key, ' '), type_key_into(&key, ivals[i]);
    for (int i = 0; i < argc; i++) {
        // an array that is passed is a pointer in the function
        Type* param = type_canon(type->args[i])->kind == TY_ARRAY ? type_ptr(type_canon(type->args[i])->base) : type->args[i];
        param = type_qual(param, Q_COMPTIME);
        long size = ct_size(c, at, param);
        char* place = ct_at(ct_room(c, at, size));
        Word held = ct_assign(c, at, place, param, args[i]).v;
        buf_putc(&key, ',');
        if (ct_is_type(param) && held) type_key_into(&key, (Type*)(intptr_t)held);
        else for (long b = 0; b < size; b++) buf_printf(&key, "%02x", (unsigned char)place[b]);
        local_push(&fc, L_VAR, type->pnames[i] ? type->pnames[i] : "", param);
        fc.locals->at = place;
    }
    CtValue* known = at_call ? (CtValue*)map_get(&ct_results, buf_str(&key)) : NULL;
    if (!known) {
        if (ct_depth == CT_CALLS) {
            // a recursion that does not end: name the calls it went through last
            Buf chain = {0};
            for (int i = ct_depth; i-- > ct_depth - 5;) buf_printf(&chain, "%s, called from ", ct_calls[i]);
            FAIL(c, at, "compile-time calls are nested more than %d deep: %s... (a function that calls itself has to end by itself)",
                 CT_CALLS, buf_str(&chain));
        }
        // an ordinary function works on behalf of the '@' call that runs it
        int soft = ct_soft;
        ct_soft = 0;
        ct_calls[ct_depth++] = f->name;
        g_ct = (CtState){ .running = f, .ret = type->base, .unrolling = -1, .deciding = -1, .use = use,
                          .floor = at_call ? floor : outer.floor, .call = at_call ? buf_str(&key) : outer.call };
        ct_result = (CtValue){ type->base, 0 };
        for (size_t i = 0; i < f->def->right->blockstmt_size; i++) gen_stmt(&fc, f->def->right->blockstmt[i], NULL, 0);
        if (g_ct.flow != CT_RETURN && !(result->kind == TY_BUILTIN && result->builtin == BK_VOID))
            FAIL(&fc, f->def->name, "'%s' ended without returning a value", f->name);
        known = (CtValue*)xalloc(sizeof(CtValue));
        *known = ct_result;
        // a result that rests on something being unimplemented is not settled for others
        if (at_call && !ct_soft) map_put(&ct_results, buf_str(&key), known);
        ct_soft |= soft;
        ct_depth--;
        g_ct = outer;
    }
    ct_top = top;
    return *known;
}

// ---------------------------------------------------------------------------
// What a program declares for compile time
// ---------------------------------------------------------------------------

static const char ct_builtin_source[] =
    "#include <stddef.h>\n"
    "pub typedef enum {\n"
    "    FIELD_OTHER, FIELD_BOOL, FIELD_CHAR, FIELD_SCHAR, FIELD_UCHAR, FIELD_SHORT, FIELD_USHORT,\n"
    "    FIELD_INT, FIELD_UINT, FIELD_LONG, FIELD_ULONG, FIELD_LLONG, FIELD_ULLONG,\n"
    "    FIELD_FLOAT, FIELD_DOUBLE, FIELD_LDOUBLE, FIELD_POINTER, FIELD_ARRAY, FIELD_STRUCT,\n"
    "    FIELD_UNION, FIELD_ENUM, FIELD_FUNCTION, FIELD_FUNCTION_POINTER,\n"
    "} FieldKind;\n"
    "pub typedef struct {\n"
    "    const char* name;\n"
    "    const char* type_name;\n"
    "    size_t offset;\n"
    "    size_t size;\n"
    "    unsigned bits;\n"
    "    FieldKind kind;\n"
    "    @Type type;\n"
    "    @Type __of;\n"         // the struct this is a field of, for `.[f]`
    "} Field;\n"
    "pub typedef struct { const char* name; long long value; } Enumerator;\n"
    "pub typedef struct { const char* name; const char* type_name; const char* value; @Type type; } Generic;\n";

// The namespace `builtin` is a file that the compiler brings along.
static Module* ct_builtin_module(void) {
    Module* m = module_new("builtin");
    m->dir = g_opt.root;
    list_push(&m->segments, "builtin");
    map_put(&g_module_by_name, m->name, m);
    SrcFile* f = file_new(m, "<builtin>");
    f->stem = f->label = "builtin";
    f->source = xstrn(ct_builtin_source, sizeof ct_builtin_source - 1);
    return m;
}

// Returns what sort of type a type is, as a value of builtin::FieldKind.
static Word ct_field_kind(Type* type) {
    enum { OTHER, POINTER = 16, ARRAY, STRUCT, UNION, ENUM, FUNCTION, FUNCTION_POINTER };
    Type* k = type_canon(type);
    switch (k->kind) {
        case TY_BUILTIN: return k->builtin >= BK_BOOL && k->builtin <= BK_LDOUBLE ? (Word)k->builtin : OTHER;
        case TY_PTR:     return type_canon(k->base)->kind == TY_FUNC ? FUNCTION_POINTER : POINTER;
        case TY_FUNC:    return FUNCTION;
        case TY_ARRAY:   return ARRAY;
        case TY_STRUCT:  return type_unq(k)->is_union ? UNION : STRUCT;
        case TY_ENUM:    return ENUM;
        default:         return OTHER;
    }
}

static List ct_prescanned;      // the generator starts the first pass found: the start, then its file
static Map ct_emit_types;       // an '@' function -> List* of the type names it emits

// Every file has had its first pass. A type that a generator emits under a
// plain name is a type name in the namespace the generator is started in.
static void ct_parse_ready(void) {
    for (size_t i = 0; i < ct_prescanned.len; i += 2) {
        Expr* start = (Expr*)ct_prescanned.items[i];
        SrcFile* f = (SrcFile*)ct_prescanned.items[i + 1];
        Module* from = start->left ? import_find(f, tok_str(start->left->token)) : f->mod;
        List* types = from ? (List*)map_get(&ct_emit_types, xstrf("%s\x01%s", from->name, tok_str(start->name))) : NULL;
        for (size_t k = 0; types && k < types->len; k++) map_put(&f->mod->typenames, (const char*)types->items[k], f);
    }
    ct_prescanned.len = 0;
}

// The first pass over a file found an '@' function. The namespaces that its
// body imports are loaded, and the parser is told about them.
static void ct_prescan(SrcFile* f, Expr* def) {
    if (def->kind == E_GENERATOR) {
        list_push(&ct_prescanned, def);
        list_push(&ct_prescanned, f);
        return;
    }
    for (size_t i = 0; i < def->blockstmt_size; i++) {
        Expr* d = def->blockstmt[i];
        const char* function = xstrf("%s\x01%s", f->mod->name, tok_str(def->name));
        List* types = (List*)map_get(&ct_emit_types, function);
        if (d->kind == E_IDENT && !types) map_put(&ct_emit_types, function, types = (List*)xalloc(sizeof(List)));
        if (d->kind == E_IDENT) list_push(types, tok_str(d->token));
        if (d->kind != E_IMPORT) continue;
        Import* import = (Import*)xalloc(sizeof(Import));
        *import = (Import){ tok_str(d->name), module_for_import(f, d), d->name };
        module_load(import->mod);
        list_push(&f->ct_imports, import);
    }
}

static Module* ct_import_find(SrcFile* f, const char* name) {
    for (size_t i = 0; i < f->ct_imports.len; i++)
        if (strcmp(((Import*)f->ct_imports.items[i])->name, name) == 0) return ((Import*)f->ct_imports.items[i])->mod;
    return NULL;
}

// Returns the file that the body of an '@' function is checked in: its own
// file, with what the body imports and includes added. What the body brings
// by itself is also noted, for the items the function emits.
static SrcFile* ct_function_file(SrcFile* f, Func* fn) {
    SrcFile* own = (SrcFile*)xalloc(sizeof(SrcFile));
    SrcFile* brings = (SrcFile*)xalloc(sizeof(SrcFile));
    Expr* body = fn->def->right;
    *own = *brings = *f;
    own->includes = brings->includes = brings->items = (List){0};
    own->imports = (Import*)xalloc(((size_t)f->nimports + body->blockstmt_size + 1) * sizeof(Import));
    brings->imports = (Import*)xalloc((body->blockstmt_size + 1) * sizeof(Import));
    brings->nimports = 0;
    brings->last_raw = NULL;
    brings->cunit = NULL;
    brings->statics = (Map){0};
    brings->ast = expr_new(E_FILE, fn->def->name, NULL, NULL);
    if (f->nimports) memcpy(own->imports, f->imports, (size_t)f->nimports * sizeof(Import));
    for (size_t i = 0; i < f->includes.len; i++) list_push(&own->includes, f->includes.items[i]);
    for (size_t i = 0; i < body->blockstmt_size; i++) {
        Expr* s = body->blockstmt[i];
        if (s->kind == E_IMPORT)
            own->imports[own->nimports++] = brings->imports[brings->nimports++] = (Import){ tok_str(s->name), module_for_import(f, s), s->name };
        if (s->kind != E_INCLUDE) continue;
        list_push(&own->includes, tok_str(s->token));
        list_push(&brings->includes, tok_str(s->token));
        expr_push(brings->ast, s);
    }
    map_put(&ct_brings, xstrf("%p", (void*)fn), brings);
    if (!brings->nimports && !brings->includes.len) return f;
    if (brings->includes.len) own->cunit = cunit_for_file(own);
    return own;
}

// Notes an '@' function under its name in a namespace.
static void ct_function(SrcFile* f, Expr* def) {
    const char* key = xstrf("%s\x01%s", f->mod->name, tok_str(def->name));
    Func* fn = (Func*)xalloc(sizeof(Func));
    *fn = (Func){ .name = tok_str(def->name), .proto = def, .def = def, .generics = def->generics, .mod = f->mod, .flags = def->flags };
    if (map_get(&ct_funcs, key)) fatal(f->path, def->name, "'@' function '%s' is defined twice", fn->name);
    fn->file = f;
    map_put(&ct_funcs, key, fn);
}

// Takes the '@' functions and the compile-time variables out of a file,
// because they have no runtime form, and notes them.
static void ct_collect(SrcFile* f) {
    size_t kept = 0;
    for (size_t i = 0; i < f->ast->blockstmt_size; i++) {
        Expr* e = f->ast->blockstmt[i];
        int at = e->kind == E_FUNCDEF && *e->name.start == '@';
        int variables = e->kind == E_DECLGROUP && ct_spec(e->type) && !(e->flags & F_TYPEDEF);
        // an '@' function that is declared ahead of its definition
        int ahead = e->kind == E_DECLGROUP && e->blockstmt_size && *e->blockstmt[0]->name.start == '@' && !(e->flags & F_TYPEDEF);
        // an impl or a trait with control among its members is declared when that can be carried out
        int controlled = (e->kind == E_IMPL || e->kind == E_TRAIT) && ct_controlled(e);
        if (!at && !variables && !ahead && !controlled && e->kind != E_GENERATOR) f->ast->blockstmt[kept++] = e;
        if (controlled) list_push(&ct_controls, e), list_push(&ct_controls, f);
        for (size_t k = 0; (e->kind == E_IMPL || e->kind == E_TRAIT) && k < e->blockstmt_size; k++)
            if (e->blockstmt[k]->name.length && *e->blockstmt[k]->name.start == '@' && !ct_spliced(e->blockstmt[k]->name))
                fatal(f->path, e->blockstmt[k]->name, "an '@' function cannot be a method or a trait member");
        if (e->kind == E_DECLGROUP && (e->flags & F_TYPEDEF) && e->blockstmt_size && *e->blockstmt[0]->name.start == '@')
            list_push(&ct_types, e), list_push(&ct_types, f);
        if (e->kind == E_GENERATOR) ct_start(f, e);
        for (size_t k = 0; variables && k < e->blockstmt_size; k++) {
            CtGlobal* g = (CtGlobal*)xalloc(sizeof(CtGlobal));
            *g = (CtGlobal){ .decl = e->blockstmt[k], .file = f };
            map_put(&ct_globals, xstrf("%s\x01%s", f->mod->name, tok_str(g->decl->name)), g);
        }
        if (e->kind != E_FUNCDEF || !e->right) continue;
        Expr* signature = decl_function_type(e->type);
        for (size_t k = 0; !at && k < signature->blockstmt_size; k++)
            if (ct_spec(type_syntax_root(signature->blockstmt[k]->type)))
                fatal(f->path, e->name, "a compile-time variable cannot be a parameter of an ordinary function; a constant generic parameter does that");
        if (at) ct_function(f, e);
    }
    f->ast->blockstmt_size = kept;
}

// ---------------------------------------------------------------------------
// Emitting
// ---------------------------------------------------------------------------

// Something that was emitted. While the generator that emitted it is being
// run again after a wait, the generator does not see it until it has come to
// the '@emit' again, so that it takes the way it took before.
typedef struct {
    void* task;                 // the generator, a CtTask*
    Impl* impl;                 // the implementation, if it is one
    int unseen;
} CtEmitted;

static Map ct_emitted;          // an '@emit' and the '@' call that carried it out -> CtEmitted*
static Token ct_at_emit;        // the '@emit' that is being carried out
static void* ct_running_task(void);
static int ct_late;             // the program is being checked already: what is declared now is checked at once

static int ct_holds(Ctx* c, Expr* e);
static void gen_ct_decl(Ctx* c, Expr* g);
static void ct_settled(Ctx* c, Token at, Impl* im);
static Map ct_call_statics;     // an '@' call -> List* of the `static` things it emitted, which its later items see

// Returns a copy of a piece of syntax. What the compiler noted in it for one
// declaration is left out, so that it can be declared again. A part that is
// reached twice, as the specifiers of a declaration are, is copied once.
static Expr* ct_copy(Expr* e, Map* seen) {
    if (!e) return NULL;
    const char* key = xstrf("%p", (void*)e);
    Expr* copy = (Expr*)map_get(seen, key);
    if (copy) return copy;
    copy = (Expr*)xalloc(sizeof(Expr));
    *copy = *e;
    map_put(seen, key, copy);
    copy->sema = NULL;
    copy->left = ct_copy(e->left, seen);
    copy->right = ct_copy(e->right, seen);
    copy->type = ct_copy(e->type, seen);
    copy->generics = ct_copy(e->generics, seen);
    copy->blockstmt = (Expr**)xalloc((e->blockstmt_size + 1) * sizeof(Expr*));
    copy->alloc_blockstmt_size = e->blockstmt_size + 1;
    for (size_t i = 0; i < e->blockstmt_size; i++) copy->blockstmt[i] = ct_copy(e->blockstmt[i], seen);
    copy->cases = (Condition*)xalloc((e->case_size + 1) * sizeof(Condition));
    for (size_t i = 0; i < e->case_size; i++)
        copy->cases[i] = (Condition){ ct_copy(e->cases[i].base_case, seen), ct_copy(e->cases[i].condition, seen),
                                      ct_copy(e->cases[i].increment, seen), ct_copy(e->cases[i].block, seen) };
    return copy;
}

// Returns the variables and generic arguments that the code at this point
// can use, with the values they have now. They are what an item that is
// declared here goes on using.
static Local* ct_capture(Ctx* c, Token at) {
    Local* captured = NULL;
    Local** link = &captured;
    for (Local* l = c->locals; l; l = l->next) {
        Local* copy = (Local*)xalloc(sizeof(Local));
        *copy = *l;
        if (l->kind == L_VAR && l->at) copy->at = (char*)ct_keep(l->at, (size_t)ct_size(c, at, l->type));
        *link = copy;
        link = &copy->next;
    }
    for (TEnv* e = c->tenv; e; e = e->next) {
        // a type argument is a type name, and a constant one a variable
        Type* k = type_unq(e->type);
        Local* copy = (Local*)xalloc(sizeof(Local));
        Word value = 0;
        *copy = (Local){ L_TYPEDEF, e->name, e->type, NULL, NULL };
        if (k->kind == TY_CONST && !k->has_value) continue;
        if (k->kind == TY_CONST) {
            ct_store(&value, ct_kind(k->base), k->value);
            *copy = (Local){ L_VAR, e->name, type_qual(k->base, Q_COMPTIME), (char*)ct_keep(&value, sizeof value), NULL };
        }
        *link = copy;
        link = &copy->next;
    }
    *link = NULL;
    return captured;
}

// Returns a file like a given one, for something that is declared with the
// variables that can be used at this point.
static SrcFile* ct_file(Ctx* c, Token at, SrcFile* like) {
    SrcFile* file = (SrcFile*)xalloc(sizeof(SrcFile));
    *file = *like;
    file->captured = ct_capture(c, at);
    return file;
}

// Carries out the compile-time control among the members of a struct, an
// impl or a trait, and adds each member that is reached to `into`, under the
// name it has then. A field gets the type it has then, and for a method the
// file is added to `files` in which its body can use what it can use now.
static void ct_expand(Ctx* c, Expr* m, Expr* into, SrcFile* file, List* files) {
    static const char* const rule = "compile-time control among members needs a condition that is a compile-time value";
    Local* scope = c->locals;
    long top = ct_top;
    int passes = 0;
    Condition* f = m->case_size ? &m->cases[m->case_size - 1] : NULL;
    Expr* copy = (Expr*)xalloc(sizeof(Expr));
    *copy = *m;
    if (f && m->kind == E_FOR && f->base_case && f->base_case->kind == E_DECLGROUP) ct_expand(c, f->base_case, into, file, files);
    else if (f && m->kind == E_FOR && f->base_case) ct_value(c, f->base_case);
    if (f && !g_ct.running && (!f->condition || !ct_known(c, f->condition))) FAIL(c, m->token, "%s", rule);
    switch (m->kind) {
        case E_LIST:
            for (size_t i = 0; i < m->blockstmt_size; i++) ct_expand(c, m->blockstmt[i], into, file, files);
            break;
        case E_DECLGROUP:
            if (!g_ct.running && !ct_spec(m->type)) FAIL(c, m->token, "only compile-time variables can be declared among members");
            gen_ct_decl(c, m);
            return;     // its variables stay in scope
        case E_IF:
            if (ct_holds(c, f->condition)) ct_expand(c, f->block, into, file, files);
            else if (m->case_size == 2) ct_expand(c, m->cases[0].block, into, file, files);
            break;
        case E_FOR: case E_WHILE:
            while (ct_holds(c, f->condition)) {
                if (++passes > CT_PASSES) FAIL(c, m->token, "this loop is still running at compile time after %d passes", CT_PASSES);
                ct_expand(c, f->block, into, file, files);
                if (f->increment) ct_value(c, f->increment);
            }
            break;
        case E_FIELD:
            if (m->type) {
                copy->type = expr_new(E_CT_TYPE, m->type->token, NULL, NULL);
                copy->type->sema = resolve_type(c, m->type);
            }
            // fall through
        default:
            if (ct_spliced(m->name)) copy->name = ct_ident(c, m->name);
            if (ct_spliced(m->token)) copy->token = copy->name;
            if (m->kind == E_FUNCDEF && files) list_push(files, ct_file(c, m->token, file));
            expr_push(into, copy);
            break;
    }
    c->locals = scope;
    ct_top = top;
}

// Returns whether compile-time control or a made name stands among some members.
static int ct_controlled(Expr* owner) {
    for (size_t i = 0; i < owner->blockstmt_size; i++) {
        Expr* m = owner->blockstmt[i];
        if (m->kind == E_IF || m->kind == E_FOR || m->kind == E_WHILE || m->kind == E_DECLGROUP || ct_spliced(m->name)) return 1;
    }
    return 0;
}

// Returns the members of a struct with the compile-time control among them carried out.
static Expr* resolve_ct_members(Ctx* c, Type* t) {
    Expr* body = t->body;
    if (ct_quiet || ct_running_task()) list_push(&ct_resolving, t);
    if (!ct_controlled(body)) return body;
    Local* scope = c->locals;
    long top = ct_top;
    Expr* flat = (Expr*)xalloc(sizeof(Expr));
    *flat = *body;
    flat->blockstmt = NULL;
    flat->blockstmt_size = flat->alloc_blockstmt_size = 0;
    for (size_t i = 0; i < body->blockstmt_size; i++) ct_expand(c, body->blockstmt[i], flat, c->file, NULL);
    c->locals = scope;
    ct_top = top;
    return flat;
}

// Declares an item in a file, as if it were written there. The control
// among the members of an impl or a trait is carried out first.
static void ct_declare(Ctx* c, SrcFile* file, Expr* item) {
    Module* mod = file->mod;
    size_t names = mod->names.len, impls = g_impls.len;
    List files = {0};
    if (item->kind == E_FUNCDEF && *item->name.start == '@') {
        ct_function(file, item);
        return;
    }
    if (item->kind == E_IMPL || item->kind == E_TRAIT) {
        Local* scope = c->locals;
        long top = ct_top;
        Expr* flat = (Expr*)xalloc(sizeof(Expr));
        *flat = *item;
        flat->blockstmt = NULL;
        flat->blockstmt_size = flat->alloc_blockstmt_size = 0;
        for (size_t i = 0; i < item->blockstmt_size; i++) ct_expand(c, item->blockstmt[i], flat, file, &files);
        c->locals = scope;
        ct_top = top;
        item = flat;
    }
    declare_one(file, item);
    if (item->kind == E_IMPL) {
        // each method goes on using what could be used where the control reached it
        Impl* im = (Impl*)g_impls.items[g_impls.len - 1];
        for (size_t i = 0; i < im->methods.len; i++) ((Func*)im->methods.items[i])->file = (SrcFile*)files.items[i];
        ct_settled(c, ct_at_emit.length ? ct_at_emit : item->token, im);
    }
    if (!ct_late) return;
    for (size_t i = impls; i < g_impls.len; i++) {
        Impl* im = (Impl*)g_impls.items[i];
        Ctx ic = ctx_for_file(im->file);
        check_impl(im);
        for (size_t j = 0; !im->nparams && j < im->methods.len; j++) {
            Func* method = (Func*)im->methods.items[j];
            if (!method->generics) func_inst(&ic, method, NULL, impl_self(im, NULL), NULL, method->def->name);
        }
    }
    if (impls != g_impls.len) check_overlaps();
    for (size_t i = names; i < mod->names.len; i++) check_entity((Entity*)mod->names.vals[i]);
}

// Carries out an '@emit': the item is added to the program, in the namespace
// the '@' function that is running is used from. From where that function is
// written the item has only what the function brings: what its body imports
// and includes, and its variables and generic arguments as they are now.
static void ct_emit(Ctx* c, Expr* s) {
    const char* id = xstrf("%p %s", (void*)s, g_ct.call ? g_ct.call : "");
    Map seen = {0};
    if (*g_ct.running->name != '@') FAIL(c, s->token, "'@emit' is only allowed in the body of an '@' function");
    CtEmitted* emitted = (CtEmitted*)map_get(&ct_emitted, id);
    if (emitted) emitted->unseen = 0;
    if (!s->left || emitted) return;        // the same call emitting it again changes nothing
    emitted = (CtEmitted*)xalloc(sizeof(CtEmitted));
    emitted->task = ct_running_task();
    map_put(&ct_emitted, id, emitted);
    Expr* item = ct_copy(s->left, &seen);
    for (int i = 0; i < generics_count(item->generics); i++)
        for (int k = 0; k < generics_count(g_ct.running->generics); k++)
            if (token_same(item->generics->blockstmt[i]->name, g_ct.running->generics->blockstmt[k]->name))
                FAIL(c, item->generics->blockstmt[i]->name, "an emitted item cannot reuse '%s', the name of a generic parameter of the '@' function that emits it",
                     tok_str(item->generics->blockstmt[i]->name));
    if (ct_spliced(item->name)) item->name = ct_ident(c, item->name);
    if (ct_spliced(item->token)) item->token = item->name;
    for (size_t i = 0; item->kind == E_DECLGROUP && i < item->blockstmt_size; i++)
        if (ct_spliced(item->blockstmt[i]->name)) item->blockstmt[i]->name = ct_ident(c, item->blockstmt[i]->name);
    if (item->kind == E_DECLGROUP && item->type && ct_spliced(item->type->name)) item->type->name = ct_ident(c, item->type->name);

    SrcFile* file = ct_file(c, s->token, (SrcFile*)map_get(&ct_brings, xstrf("%p", (void*)g_ct.running)));
    file->mod = g_ct.use->mod;
    if (file->includes.len) file->cunit = cunit_for_file(file);
    list_push(&file->mod->files, file);
    List* statics = (List*)map_get(&ct_call_statics, g_ct.call ? g_ct.call : "");
    if (!statics) map_put(&ct_call_statics, g_ct.call ? g_ct.call : "", statics = (List*)xalloc(sizeof(List)));
    for (size_t i = 0; i < g_ct.use->statics.len; i++) map_put(&file->statics, g_ct.use->statics.keys[i], g_ct.use->statics.vals[i]);
    for (size_t i = 0; i < statics->len; i++) map_put(&file->statics, ((Entity*)statics->items[i])->name, statics->items[i]);
    size_t impls = g_impls.len, before = file->statics.len;
    ct_at_emit = s->token;
    ct_declare(c, file, item);
    if (item->kind == E_IMPL && g_impls.len > impls) emitted->impl = (Impl*)g_impls.items[impls];
    for (size_t i = before; i < file->statics.len; i++) list_push(statics, file->statics.vals[i]);
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

// A generator that a file starts with `for<...>`: one is run for each
// instance of the generic type among the type arguments of its call.
typedef struct {
    Expr* start;
    SrcFile* file;
    Type** params;              // what stands for the parameters after `for` in the pattern
    Type* pattern;              // the type argument that the instances are matched against
    int bad;
} CtStart;

// A generator that is to be run: the call, where it stands, and what the
// parameters after `for` are for this one.
typedef struct {
    Expr* call;
    SrcFile* file;
    TEnv* tenv;
    int done;
    const char* waits;          // what it asked about that does not exist yet...
    Type* wait_type;            // ...which is whether this type...
    Entity* wait_trait;         // ...implements this trait...
    Token wait_at;              // ...asked here
    SrcFile* wait_file;
} CtTask;

// Something that was settled as unimplemented for one generator, or with
// `task` NULL for ordinary code
typedef struct {
    CtTask* task;
    const char* what;
    Type* type;
    Entity* trait;
    Token at;
    SrcFile* file;
} CtNo;

static List ct_deferred;        // CtStart*
static List ct_tasks;           // CtTask*
static List ct_nos;             // CtNo*
static Map ct_spawned;          // a deferred start and an instance -> the task
static CtTask* ct_task;         // the generator that is being run
typedef struct { SrcFile* file; Token at; const char* message; } CtConflict;
static List ct_conflicts;       // CtConflict*, in source order
static Expr* ct_trying;         // the statement that is being tried by itself
static int ct_missed;           // statements of the body being tried that did not get through
static int ct_answer;           // bodies that wait for an answer get it now: nothing else moves any more
static jmp_buf* ct_wait;        // where it goes when it has to wait
static int ct_held;             // function bodies that are waiting for that
static int ct_final;            // nothing more will be emitted: what is missing is an error

static void* ct_running_task(void) {
    return ct_task;
}

static void ct_start(SrcFile* f, Expr* start) {
    Ctx c = ctx_for_file(f);
    Expr* call = start->left;
    if (call->kind != E_CALL || !ct_at_call(&c, call)) fatal(f->path, start->token, "only a call of an '@' function can stand at file scope");
    if (start->generics) {
        CtStart* d = (CtStart*)xalloc(sizeof(CtStart));
        *d = (CtStart){ .start = start, .file = f };
        list_push(&ct_deferred, d);
    } else {
        CtTask* t = (CtTask*)xalloc(sizeof(CtTask));
        *t = (CtTask){ .call = call, .file = f };
        list_push(&ct_tasks, t);
    }
}

// Returns whether a type implements a trait. If it does not, whoever asks
// waits, because a generator may still emit the implementation. When no
// generator can go on, the answer is that it is unimplemented, and stays so.
static int ct_implements(Ctx* c, Token at, Type* type, Expr* bound) {
    Entity* trait = lookup_type_entity(c, bound);
    Impl* found = bound_impl(c, type, bound);
    Buf what = {0};
    for (size_t i = 0; found && i < ct_emitted.len; i++) {
        CtEmitted* emitted = (CtEmitted*)ct_emitted.vals[i];
        if (emitted->impl == found && emitted->unseen && emitted->task == ct_task) found = NULL;
    }
    if (found) return 1;
    buf_printf(&what, "%p ", (void*)trait);
    type_key_into(&what, type);
    for (size_t i = 0; i < ct_nos.len; i++) {
        CtNo* no = (CtNo*)ct_nos.items[i];
        if (no->task == ct_task && strcmp(no->what, buf_str(&what)) == 0) return ct_soft = 1, 0;
    }
    if (ct_task) {
        ct_task->waits = buf_str(&what);
        ct_task->wait_type = type;
        ct_task->wait_trait = trait;
        ct_task->wait_at = at;
        ct_task->wait_file = c->file;
        longjmp(*ct_wait, 1);
    }
    if (ct_quiet && !ct_answer) FAIL(c, at, "this waits for a generator");
    CtNo* no = (CtNo*)xalloc(sizeof(CtNo));
    *no = (CtNo){ NULL, buf_str(&what), type, trait, at, c->file };
    list_push(&ct_nos, no);
    ct_soft = 1;
    return 0;
}

// An implementation was just declared. If it makes something implemented
// that was settled as unimplemented for someone else, that is an error.
static void ct_settled(Ctx* c, Token at, Impl* im) {
    impl_prepare(im);
    for (size_t i = 0; i < ct_nos.len; i++) {
        CtNo* no = (CtNo*)ct_nos.items[i];
        Bind b = {0};
        if (no->task == ct_task || im->trait != no->trait || !impl_match(im, no->type, &b)) continue;
        // all of these are reported, in source order, when the generators have run
        CtConflict* conflict = (CtConflict*)xalloc(sizeof(CtConflict));
        size_t place = ct_conflicts.len;
        *conflict = (CtConflict){ c->file, at, xstrf("this makes '%s' implement '%s', which was already settled as unimplemented for the code at %s:%zu",
                                                      type_show(no->type), no->trait->name, no->file->path, no->at.row) };
        list_push(&ct_conflicts, conflict);
        for (; place > 0; place--) {
            CtConflict* earlier = (CtConflict*)ct_conflicts.items[place - 1];
            if (earlier->at.row < at.row || (earlier->at.row == at.row && earlier->at.col <= at.col)) break;
            ct_conflicts.items[place] = earlier;
        }
        ct_conflicts.items[place] = conflict;
    }
}

// Something that was resolving the members of types did not get through.
// Those types are resolved again when they are next needed.
static void ct_unresolve(void) {
    for (size_t i = 0; i < ct_resolving.len; i++)
        if (((Type*)ct_resolving.items[i])->state == 1) ((Type*)ct_resolving.items[i])->state = 0;
    ct_resolving.len = 0;
}

// Runs a generator. Returns whether that got anywhere: it finished, or it
// emitted something before it had to wait.
static int ct_attempt(CtTask* t) {
    Ctx c = ctx_for_file(t->file);
    jmp_buf wait;
    jmp_buf* recover = g_recover;
    size_t emitted = ct_emitted.len;
    int tries = g_try_depth;
    c.tenv = t->tenv;
    ct_owner = NULL;
    ct_top = ct_depth = ct_dry = ct_soft = 0;
    g_ct = (CtState){ .unrolling = -1, .deciding = -1 };
    ct_task = t;
    ct_wait = &wait;
    t->done = 1;
    if (setjmp(wait) == 0) {
        RECOVER(ct_eval(&c, t->call));
    } else {
        // It waits: it is run again from the start later. What it emitted stays.
        t->done = 0;
        g_try_depth = tries;
        g_recover = recover;
        ct_unresolve();
        for (size_t i = 0; i < ct_emitted.len; i++)
            if (((CtEmitted*)ct_emitted.vals[i])->task == t) ((CtEmitted*)ct_emitted.vals[i])->unseen = 1;
    }
    ct_task = NULL;
    ct_top = ct_depth = 0;
    g_ct = (CtState){ .unrolling = -1, .deciding = -1 };
    return t->done || ct_emitted.len != emitted;
}

// Starts a generator for each instance of a generic type that a start with
// `for<...>` is about and that has appeared since this was last done.
static void ct_spawn_from(CtStart* d) {
    Expr* g = d->start->generics;
    Expr* targs = d->start->left->right->generics;
    int n = generics_count(g);
    Ctx c = ctx_for_file(d->file);
    if (!d->params) {
        d->bad = 1;
        d->params = (Type**)xalloc((size_t)(n + 1) * sizeof(Type*));
        for (int i = 0; i < n; i++) d->params[i] = param_placeholder(d->file, g->blockstmt[i]);
        c.tenv = tenv_bind(g, d->params, NULL);
        for (int k = 0; k < generics_count(targs) && !d->pattern; k++) {
            Expr* syntax = type_argument(targs->blockstmt[k]);
            Type* pattern = syntax ? resolve_type(&c, syntax) : NULL;
            if (pattern && type_unq(pattern)->kind == TY_APP) d->pattern = pattern;
        }
        if (!d->pattern) FAIL(&c, d->start->token, "a generator that is started with 'for<...>' needs a generic type with those parameters among its type arguments");
        for (int i = 0; i < n; i++)
            if (!type_mentions(d->pattern, d->params[i]))
                FAIL(&c, g->blockstmt[i]->name, "generic parameter '%s' does not occur in the type arguments of the call", d->params[i]->name);
        d->bad = 0;
    }
    Entity* generic = type_unq(d->pattern)->generic;
    for (size_t i = 0; !d->bad && i < generic->insts.len; i++) {
        Type* inst = (Type*)generic->insts.vals[i];
        const char* key = xstrf("%p %p", (void*)d, (void*)inst);
        Bind b = { .n = n, .params = d->params, .vals = (Type**)xalloc((size_t)(n + 1) * sizeof(Type*)) };
        int bound = !map_has(&ct_spawned, key) && type_unq(inst)->kind != TY_APP && unify(d->pattern, inst, &b);
        for (int k = 0; k < n; k++) bound = bound && b.vals[k];
        if (!bound) continue;
        CtTask* t = (CtTask*)xalloc(sizeof(CtTask));
        *t = (CtTask){ .call = d->start->left, .file = d->file, .tenv = tenv_bind(g, b.vals, NULL) };
        map_put(&ct_spawned, key, t);
        list_push(&ct_tasks, t);
    }
}

// Runs the generators until none of them can go on. Those that still wait
// then get their answer, which is that what they wait for is unimplemented,
// and go on with that.
static void ct_schedule(void) {
    static int scheduling;
    if (scheduling) return;
    scheduling = 1;
    for (size_t i = 0; i < ct_controls.len; i += 2) {
        SrcFile* file = (SrcFile*)ct_controls.items[i + 1];
        Ctx c = ctx_for_file(file);
        RECOVER(ct_declare(&c, file, (Expr*)ct_controls.items[i]));
    }
    ct_controls.len = 0;
    for (size_t i = 0; i < ct_types.len; i += 2) {
        // a compile-time type is declared like any type, but stays out of the generated C
        Expr* g = (Expr*)ct_types.items[i];
        SrcFile* file = (SrcFile*)ct_types.items[i + 1];
        for (size_t k = 0; k < g->blockstmt_size; k++) {
            const char* name = tok_str(g->blockstmt[k]->name);
            Entity* e = (Entity*)map_get(&file->mod->names, name);
            Entity* plain = (Entity*)map_get(&file->mod->names, name + 1);
            if (e && e->item) e->item->state = 2, e->item->text = "";
            if (plain && (plain->kind == ENT_TYPEDEF || plain->kind == ENT_GTYPEDEF))
                RECOVER(fatal(file->path, g->blockstmt[k]->name, "'%s' and '%s' are both declared as types", name + 1, name));
        }
    }
    ct_types.len = 0;
    for (;;) {
        int progress = 0, waiting = 0;
        for (size_t i = 0; i < ct_deferred.len; i++) RECOVER(ct_spawn_from((CtStart*)ct_deferred.items[i]));
        for (size_t i = 0; i < ct_tasks.len; i++) {
            CtTask* t = (CtTask*)ct_tasks.items[i];
            if (t->done) continue;
            progress |= ct_attempt(t);
            waiting |= !t->done;
        }
        if (progress) continue;
        if (!waiting) break;
        for (size_t i = 0; i < ct_tasks.len; i++) {
            CtTask* t = (CtTask*)ct_tasks.items[i];
            CtNo* no = (CtNo*)xalloc(sizeof(CtNo));
            if (t->done) continue;
            *no = (CtNo){ t, t->waits, t->wait_type, t->wait_trait, t->wait_at, t->wait_file };
            list_push(&ct_nos, no);
        }
    }
    for (size_t i = 0; i < ct_conflicts.len; i++) {
        CtConflict* conflict = (CtConflict*)ct_conflicts.items[i];
        RECOVER(fatal(conflict->file->path, conflict->at, "%s", conflict->message));
    }
    ct_conflicts.len = 0;
    scheduling = 0;
}

// The checker is about to generate the body of a function. While generators
// can still be started, the body is only tried: if it does not get through,
// that may be because what it needs has not been emitted yet, so it is tried
// again later. Returns whether the function was dealt with here.
static int gen_ct_function(FuncInst* fi) {
    jmp_buf attempt;
    int depth = g_try_depth;
    ct_late = 1;
    if ((!ct_deferred.len && !ct_emits) || ct_final) return 0;
    g_ct_quiet = &attempt;
    ct_quiet++;
    ct_missed = 0;
    ct_resolving.len = 0;
    if (setjmp(attempt) == 0) gen_function(fi);
    else ct_missed++;
    if (ct_missed) {
        fi->generated = 0;
        ct_held++;
        ct_owner = NULL;
        g_generating = NULL;
        ct_unresolve();
    }
    ct_quiet--;
    g_ct_quiet = NULL;
    g_try_depth = depth;
    return 1;
}

// Called when every function body that was asked for has been generated or
// tried. Runs the generators that can run now, and returns whether the
// bodies have to be gone through again.
static int ct_more(void) {
    static size_t emitted;
    ct_late = 1;
    ct_schedule();
    int progress = ct_emitted.len != emitted, held = ct_held;
    emitted = ct_emitted.len;
    ct_held = 0;
    // First the bodies that ask whether something exists get their answer,
    // and only when that moves nothing either is what is missing an error.
    if (progress) return ct_answer = ct_final = 0, 1;
    if (held && !ct_answer && !ct_final) return ct_answer = 1;
    if (held && !ct_final) return ct_final = 1;
    ct_answer = ct_final = 0;
    return 0;
}

// Called once, when every file has been parsed and nothing has been declared
// yet, so that what only exists at compile time is never declared.
static void ct_collect_program(void) {
    static int done;
    if (done++) return;
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++) RECOVER(ct_collect((SrcFile*)mod->files.items[i]));
    }
}

// ---------------------------------------------------------------------------
// Where the checker meets compile-time code
// ---------------------------------------------------------------------------

// The compile-time variables of a function body live as long as the checker
// is generating that body. A new body starts with none.
static void ct_frame(void) {
    if (ct_owner == g_generating) return;
    ct_owner = g_generating;
    ct_top = 0;
    ct_depth = 0;
    ct_dry = 0;
    g_ct = (CtState){ .unrolling = -1, .deciding = -1 };
}

// Evaluates a compile-time expression that stands by itself. The address of
// a struct or an array that it returns stays good until more is evaluated.
static CtValue ct_value(Ctx* c, Expr* e) {
    ct_frame();
    long top = ct_top;
    CtValue value = ct_eval(c, e);
    ct_top = top;
    return value;
}

// Returns whether a compile-time condition holds. A loop without a condition goes on.
static int ct_holds(Ctx* c, Expr* e) {
    if (!e) return 1;
    CtValue value = ct_value(c, e);
    if (!ct_kind(value.type)) FAIL(c, e->token, "a condition is needed here, and this is a value of type '%s'", type_show(value.type));
    return ct_kind(value.type) & CT_FLOAT ? ct_double(value.v) != 0 : value.v != 0;
}

// Returns whether a compile-time variable is changed anywhere in an expression.
static int ct_changes(Ctx* c, Expr* e) {
    if (!e) return 0;
    int steps = (e->kind == E_UNARY || e->kind == E_POSTFIX) && (token_is(e->token, "++") || token_is(e->token, "--"));
    if ((steps || ct_assigns(e)) && e->left->kind == E_IDENT && e->left->token.kind == TK_IDENT && ct_local(c, e->left->token)) return 1;
    if (ct_changes(c, e->left) || ct_changes(c, e->right)) return 1;
    for (size_t i = 0; i < e->case_size; i++)
        if (ct_changes(c, e->cases[i].condition) || ct_changes(c, e->cases[i].block)) return 1;
    return 0;
}

// Evaluates a compile-time expression that the checker met and writes its
// value as Typed C. Returns whether the expression was a compile-time one.
static int ct_written(Ctx* c, Expr* e, Val* out, int braces) {
    Word number;
    if (ct_known(c, e) != 2) return 0;
    CtValue value = ct_value(c, e);
    void* address = (void*)(intptr_t)value.v;
    // a number is put in memory to be read as its type
    if (ct_kind(value.type)) ct_store(address = &number, ct_kind(value.type), value.v);
    *out = val(ct_text(c, e->token, value.type, address, braces), type_unq(value.type));
    return 1;
}

// The checker is about to lower an expression. If the expression is made of
// compile-time values, it is evaluated, and its value is what the checker
// writes. Returns whether that was so.
static int gen_ct_expr(Ctx* c, Expr* e, Val* out) {
    // The right operand of && and ||, and the two results of ?:, are only
    // evaluated if a runtime value says so: nothing may change there.
    int guarded = e->kind == E_BINARY && (token_is(e->token, "&&") || token_is(e->token, "||"));
    Expr* named = ct_named(c, e);
    if (named != e) return *out = gen_expr(c, named, NULL), 1;
    if (ct_written(c, e, out, 0)) return 1;
    if ((guarded && ct_changes(c, e->right)) ||
        (e->kind == E_TERNARY && (ct_changes(c, e->cases[1].block) || ct_changes(c, e->cases[0].block))))
        FAIL(c, e->token, "a compile-time variable cannot change in a part of an expression that is decided at runtime");
    return 0;
}

// The checker is about to lower the value a variable is initialized with.
static int gen_ct_init(Ctx* c, Expr* e, Type* expected, Val* out) {
    // a list in braces is what a struct or an array is initialized with
    int kind = expected ? type_canon(expected)->kind : TY_UNKNOWN;
    Expr* list = NULL;
    for (size_t i = 0; e->kind == E_INIT_LIST && i < e->blockstmt_size; i++) {
        // a member that `.[f]` designates is written by its name
        Expr* item = e->blockstmt[i];
        if (item->kind != E_INIT_ITEM || !item->left->aux) continue;
        if (!list) {
            list = (Expr*)xalloc(sizeof(Expr));
            *list = *e;
            list->blockstmt = (Expr**)memcpy(xalloc(e->blockstmt_size * sizeof(Expr*)), e->blockstmt, e->blockstmt_size * sizeof(Expr*));
        }
        Expr* designator = (Expr*)memcpy(xalloc(sizeof(Expr)), item->left, sizeof(Expr));
        designator->name = ct_field_member(c, item->left->left);
        designator->left = NULL;
        designator->aux = 0;
        list->blockstmt[i] = (Expr*)memcpy(xalloc(sizeof(Expr)), item, sizeof(Expr));
        list->blockstmt[i]->left = designator;
    }
    if (list) return *out = gen_init(c, list, expected), 1;
    return e->kind != E_INIT_LIST && ct_written(c, e, out, kind == TY_STRUCT || kind == TY_ARRAY);
}

// The checker needs the value of a constant expression, such as the size of
// an array. Returns whether the expression was a compile-time one.
static int const_ct_eval(Ctx* c, Expr* e, long long* out) {
    if (ct_known(c, e) != 2) return 0;
    CtValue value = ct_value(c, e);
    *out = value.v;
    return ct_kind(value.type) && !(ct_kind(value.type) & CT_FLOAT);
}

// Declares the compile-time variables of a declaration in the scope and
// gives them their first values. Nothing of it reaches the generated C.
static void gen_ct_decl(Ctx* c, Expr* g) {
    for (size_t i = 0; i < g->blockstmt_size; i++) {
        // the variable is not in scope yet while its first value is evaluated
        Expr* d = g->blockstmt[i];
        Type* type = ct_declared(c, d);
        long at = ct_room(c, d->name, ct_size(c, d->name, type));
        long top = ct_top;
        if (d->left) ct_init(c, ct_at(at), type, d->left);
        ct_top = top;
        local_push(c, L_VAR, tok_str(d->name), type);
        c->locals->at = ct_at(at);
    }
}

// Carries out the body of a decided branch, or one pass of a loop that is
// run at compile time. In ordinary code that leaves the statements of the
// body in the output, in braces only if they declare an ordinary variable.
static void gen_ct_body(Ctx* c, Expr* s, Buf* out, int ind) {
    Local* scope = c->locals;
    long top = ct_top;
    int declares = 0;
    for (size_t i = 0; s->kind == E_BLOCK && i < s->blockstmt_size; i++)
        declares |= s->blockstmt[i]->kind == E_DECLGROUP && !ct_spec(s->blockstmt[i]->type);
    if (g_ct.running) {
        gen_stmt(c, s, out, ind);
    } else if (s->kind != E_BLOCK || declares) {
        // a block that a compile-time `continue` emptied leaves nothing
        Buf body = {0}, empty = {0};
        gen_stmt(c, s, &body, ind);
        buf_indent(&empty, ind);
        buf_puts(&empty, "{\n");
        buf_indent(&empty, ind);
        buf_puts(&empty, "}\n");
        if (strcmp(buf_str(&body), buf_str(&empty)) != 0) buf_printf(out, "%s%s", line_at(c->file, s->token), buf_str(&body));
    } else {
        for (size_t i = 0; i < s->blockstmt_size; i++) {
            buf_puts(out, line_at(c->file, s->blockstmt[i]->token));
            gen_stmt(c, s->blockstmt[i], out, ind);
        }
    }
    c->locals = scope;
    ct_top = top;
}

// The checker is about to lower a statement. Returns whether the statement
// was dealt with here: a declaration of compile-time variables, a branch or
// loop on a compile-time condition, a `break` or `continue` that belongs to
// such a loop, or an expression that only works with compile-time values.
//
// The statements of a function that is being run come here too. Each of them
// is carried out, so all of them are dealt with here.
static int gen_ct_stmt(Ctx* c, Expr* s, Buf* out, int ind) {
    if (g_ct.flow) return 1;    // skipped by a break, a continue or a return that was carried out
    if (s == g_ct_pass) {
        g_ct_pass = NULL;
        return 0;
    }
    if (ct_quiet && !g_ct.running && s != ct_trying) {
        // While a body is tried, each statement is tried by itself, so that
        // one that misses something does not keep the rest from being seen.
        jmp_buf attempt;
        jmp_buf* outer = g_ct_quiet;
        CtState state = g_ct;
        int depth = g_try_depth;
        g_ct_quiet = &attempt;
        ct_trying = s;
        if (setjmp(attempt) == 0) {
            gen_stmt(c, s, out, ind);
        } else {
            ct_missed++;
            g_ct = state;
            g_ct_pass = NULL;
        }
        ct_trying = NULL;
        g_ct_quiet = outer;
        g_try_depth = depth;
        return 1;
    }
    ct_trying = NULL;
    ct_frame();
    CtState outer = g_ct;
    Local* scope = c->locals;
    long top = ct_top;
    int running = g_ct.running != NULL;
    int loop = s->kind == E_WHILE || s->kind == E_DOWHILE || s->kind == E_FOR;
    int control = loop || s->kind == E_IF || s->kind == E_SWITCH;
    Condition* f = control ? &s->cases[s->case_size - 1] : NULL;

    if (s->kind == E_DECLGROUP) {
        if (s->flags & F_TYPEDEF) {
            // a type that a function being run declares is declared as in any function
            Buf unused = {0};
            if (running) g_ct_pass = s, gen_stmt(c, s, &unused, 0);
            return running;
        }
        if (!running && !ct_spec(s->type)) return 0;
        gen_ct_decl(c, s);
        return 1;
    }
    if (s->kind == E_BREAK || s->kind == E_CONTINUE) {
        // a `break` leaves the switch being decided, or else the loop being run, as `continue` does
        int decided = s->kind == E_BREAK && g_ct.deciding >= 0;
        int level = decided ? g_ct.deciding : g_ct.unrolling;
        if (level < 0 || (g_ct.in_switch && s->kind == E_BREAK && !decided)) {
            if (running) FAIL(c, s->token, "'%s' has no loop to leave here", tok_str(s->token));
            return 0;
        }
        if (g_ct.runtime > level)
            FAIL(c, s->token, "'%s' is decided at runtime here, but it would leave code that is run at compile time", tok_str(s->token));
        g_ct.flow = s->kind == E_BREAK ? CT_BREAK : CT_CONTINUE;
        return 1;
    }
    if (running && s->kind == E_RETURN) {
        // a struct that is returned outlives the variables of the call
        Type* type = g_ct.ret;
        if (s->left) {
            long size = ct_size(c, s->token, type);
            char* address = ct_at(ct_room(c, s->token, size));
            ct_init(c, address, type, s->left);
            ct_result = ct_read(c, s->token, address, type);
            if (!ct_kind(type)) ct_result.v = (Word)(intptr_t)ct_keep(address, (size_t)size);
        }
        ct_top = top;
        g_ct.flow = CT_RETURN;
        return 1;
    }
    if (s->kind == E_IMPORT || s->kind == E_EMIT || (running && s->kind == E_INCLUDE)) {
        // what the body imports and includes was taken up when its function was noted
        if (!running) FAIL(c, s->token, "'%s' is only allowed in the body of an '@' function", s->kind == E_EMIT ? "@emit" : "import");
        if (s->kind == E_EMIT) ct_emit(c, s);
        return 1;
    }
    if (running && (s->kind == E_BLOCK || s->kind == E_ALL)) {
        for (size_t i = 0; s->kind == E_BLOCK && i < s->blockstmt_size; i++) gen_stmt(c, s->blockstmt[i], out, ind);
        c->locals = scope;
        ct_top = top;
        return 1;
    }
    if (!control) {
        // a statement that only works with compile-time values leaves nothing behind
        if (!running && ct_known(c, s) != 2) return 0;
        ct_eval(c, s);
        ct_top = top;
        return 1;
    }

    // the first clause of a `for` may declare the compile-time variable its condition is about
    int declares = s->kind == E_FOR && f->base_case && f->base_case->kind == E_DECLGROUP && (running || ct_spec(f->base_case->type));
    if (declares) gen_ct_decl(c, f->base_case);
    int runs = running || (f->condition && ct_known(c, f->condition) == 2);
    if (declares && !runs) FAIL(c, s->token, "a loop over a compile-time variable needs a condition that is a compile-time value");

    int flow = 0, passes = 0;
    if (runs && loop) {
        if (s->kind == E_FOR && f->base_case && !declares) gen_stmt(c, f->base_case, out, ind);
        g_ct.unrolling = g_ct.runtime;
        g_ct.deciding = -1;
        g_ct.in_switch = 0;
        for (int first = 1; (first && s->kind == E_DOWHILE) || ct_holds(c, f->condition); first = 0) {
            if (++passes > CT_PASSES) FAIL(c, s->token, "this loop is still running at compile time after %d passes", CT_PASSES);
            gen_ct_body(c, f->block, out, ind);
            if (g_ct.flow == CT_BREAK || g_ct.flow == CT_RETURN) break;
            g_ct.flow = 0;
            if (s->kind == E_FOR && f->increment) ct_value(c, f->increment);
        }
        flow = g_ct.flow == CT_RETURN ? CT_RETURN : 0;
    } else if (runs && s->kind == E_SWITCH) {
        // the statements from the matching case, or from `default`, up to a `break`
        Expr* body = f->block;
        if (body->kind != E_BLOCK) FAIL(c, s->token, "a switch on a compile-time value needs a body in braces");
        Word value = ct_value(c, f->condition).v;
        size_t count = body->blockstmt_size, start = count, otherwise = count;
        for (size_t i = 0; i < count && start == count; i++)
            for (Expr* label = body->blockstmt[i]; label->kind == E_CASE || label->kind == E_DEFAULT; label = label->right) {
                if (label->kind == E_DEFAULT) otherwise = i;
                else if ((running || ct_known(c, label->left)) && ct_value(c, label->left).v == value) start = i;
            }
        g_ct.deciding = g_ct.runtime;
        for (size_t i = start == count ? otherwise : start; i < count && !g_ct.flow; i++) {
            Expr* inner = body->blockstmt[i];
            while (inner->kind == E_CASE || inner->kind == E_DEFAULT) inner = inner->right;
            gen_stmt(c, inner, out, ind);
        }
        // a `continue` goes on to the loop around the switch, and a `return` to the call
        flow = g_ct.flow == CT_BREAK ? 0 : g_ct.flow;
    } else if (runs) {
        if (ct_holds(c, f->condition)) gen_ct_body(c, f->block, out, ind);
        else if (s->case_size == 2) gen_ct_body(c, s->cases[0].block, out, ind);
        flow = g_ct.flow;
    } else {
        // Decided at runtime: compile-time variables from outside cannot change
        // in it, and a loop or switch takes over `break` and `continue`.
        g_ct.runtime++;
        g_ct.floor = ct_top;
        if (s->kind == E_SWITCH) g_ct.in_switch = 1;
        if (loop) g_ct.unrolling = -1;
        if (s->kind != E_IF) g_ct.deciding = -1;
        g_ct_pass = s;
        gen_stmt(c, s, out, ind);
    }
    g_ct = outer;
    g_ct.flow = flow;
    c->locals = scope;
    ct_top = top;
    return 1;
}
