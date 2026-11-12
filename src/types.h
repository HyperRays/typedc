#pragma once
#include "util.h"
#include "parser.h"

// Semantic model shared by the loader, the checker and the emitter.

typedef struct Type Type;
typedef struct Field Field;
typedef struct Symbol Symbol;
typedef struct Item Item;
typedef struct Entity Entity;
typedef struct Func Func;
typedef struct FuncInst FuncInst;
typedef struct Impl Impl;
typedef struct TEnv TEnv;
typedef struct Local Local;
typedef struct SrcFile SrcFile;
typedef struct Module Module;
typedef struct Ctx Ctx;

// ---------------------------------------------------------------------------
// Generated C names
// ---------------------------------------------------------------------------

enum { SP_ORDINARY, SP_TAG };

// A name in the generated C. It starts as the source name and only grows a
// prefix (from `context`, innermost first) when it would collide.
struct Symbol {
    int id;
    int space;
    const char* base;
    const char* context[16];
    int ncontext;
    int level;              // how many context entries are prepended
    int fixed;              // must keep its source name (C linkage, `main`)
    const char* identity;   // stable ordering key for the numeric fallback
    const char* final;
    Map avoid;              // names of locals in scope where the symbol is used:
                            // taking one of them would let the local shadow it
};

static List g_symbols;

static inline Symbol* symbol_new(const char* base, int space) {
    Symbol* s = (Symbol*)xalloc(sizeof(Symbol));
    s->id = (int)g_symbols.len;
    s->space = space;
    s->base = base;
    list_push(&g_symbols, s);
    return s;
}

static inline void symbol_context(Symbol* s, const char* text) {
    if (!text || !*text || s->ncontext >= 16) return;
    // only identifier characters survive
    char* clean = xstrn(text, strlen(text));
    for (char* c = clean; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_') *c = '_';
    s->context[s->ncontext++] = clean;
}

// Symbols are written into generated text as placeholders and substituted
// once every name in the program is known.
static inline const char* symbol_ref(Symbol* s) {
    return xstrf("\x01%d\x02", s->id);
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef enum {
    BK_VOID, BK_BOOL, BK_CHAR, BK_SCHAR, BK_UCHAR, BK_SHORT, BK_USHORT,
    BK_INT, BK_UINT, BK_LONG, BK_ULONG, BK_LLONG, BK_ULLONG,
    BK_FLOAT, BK_DOUBLE, BK_LDOUBLE,
    BK_CFLOAT, BK_CDOUBLE, BK_CLDOUBLE,
    BK_OPAQUE,          // compiler-provided type known only by name
    BK_COUNT
} BuiltinKind;

static const char* const builtin_names[BK_COUNT] = {
    "void", "_Bool", "char", "signed char", "unsigned char", "short", "unsigned short",
    "int", "unsigned int", "long", "unsigned long", "long long", "unsigned long long",
    "float", "double", "long double",
    "float _Complex", "double _Complex", "long double _Complex",
    "?"
};

typedef enum {
    TY_UNKNOWN,     // not known to Typed C (macros, raw C); the C compiler checks it
    TY_BUILTIN,
    TY_PTR,
    TY_ARRAY,
    TY_FUNC,
    TY_STRUCT,      // struct or union
    TY_ENUM,
    TY_ALIAS,       // typedef name
    TY_PARAM,       // generic parameter inside a pattern
    TY_APP,         // generic typedef applied to arguments that mention a parameter
    TY_CONST,       // a constant generic argument (never the type of a value)
} TypeKind;

enum { Q_CONST = 1, Q_VOLATILE = 2, Q_RESTRICT = 4, Q_ATOMIC = 8 };

struct Field {
    const char* name;       // NULL for anonymous members
    Type* type;
    const char* bits;       // bit-field width / enumerator value, as C text
    const char* align;      // _Alignas(...) prefix
    const char* verbatim;   // raw C, #include or _Static_assert member
    int defines;            // the member's specifier defines its struct / enum type
    Symbol* sym;            // enumerator name
    const char* lead;  size_t lead_len;     // source comments before the member
    const char* trail; size_t trail_len;    // ... and finishing its line
};

struct Type {
    TypeKind kind;
    int quals;
    Type* unq;              // the unqualified type this is a qualified copy of
    BuiltinKind builtin;
    const char* name;       // builtin spelling, tag, typedef name, parameter name
    Type* base;             // pointee, element, return type, aliased type

    Type** args;            // function parameters, or generic arguments
    int nargs;
    const char** pnames;    // function parameter names
    int variadic;
    int voidparams;
    const char* size;       // array size as C text, NULL when omitted
    int arr_static;
    int has_size_value;     // the size is a constant typedc could evaluate
    long long size_value;
    Type* size_param;       // pattern: the size is exactly this constant parameter

    // constant generics: TY_CONST holds its C text in `name` and the declared
    // type in `base`; a constant TY_PARAM has `is_const` and `base`
    int is_const;
    int has_value;
    long long value;

    // named types
    Symbol* sym;            // generated name (typedef name, tag, or instance name)
    int from_c;             // declared by a C header: printed and compared by name
    int is_union;
    int is_pub;             // a tag declared `pub`
    int is_inst;            // instance of a generic typedef
    Entity* generic;        // the generic typedef (instances and TY_APP)
    Type* requester;        // the instance whose definition first asked for this one
    const char* cname;      // typedef name of an anonymous struct / enum
    Item* item;             // where the generated C defines it

    // struct / union / enum bodies are resolved on demand
    Expr* body;
    SrcFile* file;
    TEnv* tenv;
    Type* self;
    int layout;             // by-value containment check; 0: not yet, 1: inside, 2: finite
    int state;              // 0: unresolved, 1: resolving, 2: done
    int opaque;             // body could not be understood: members are unknown
    int quiet;              // a later specialization: printed without the source comments
    Field* fields;
    int nfields;
};

static inline Type* type_new(TypeKind kind) {
    Type* t = (Type*)xalloc(sizeof(Type));
    t->kind = kind;
    return t;
}

static Type g_unknown = { .kind = TY_UNKNOWN, .name = "<unknown>" };
static Type* g_builtin[BK_COUNT];
static Type* g_size_t;
static Type* g_ptrdiff_t;
static Type* g_char_ptr;

static inline Type* type_builtin(BuiltinKind kind) {
    if (!g_builtin[kind]) {
        Type* t = type_new(TY_BUILTIN);
        t->builtin = kind;
        t->name = builtin_names[kind];
        g_builtin[kind] = t;
    }
    return g_builtin[kind];
}

static inline Type* type_opaque(const char* name) {
    static Map opaque;
    Type* t = (Type*)map_get(&opaque, name);
    if (!t) {
        t = type_new(TY_BUILTIN);
        t->builtin = BK_OPAQUE;
        t->name = name;
        map_put(&opaque, name, t);
    }
    return t;
}

static inline Type* type_unq(Type* t) {
    return t->unq ? t->unq : t;
}

// Add qualifiers. The copy shares everything with the original and points back
// at it, so named types keep one identity.
static inline Type* type_qual(Type* t, int quals) {
    if (!quals || (t->quals & quals) == quals || t->kind == TY_UNKNOWN) return t;
    Type* copy = (Type*)xalloc(sizeof(Type));
    *copy = *t;
    copy->quals = t->quals | quals;
    copy->unq = type_unq(t);
    return copy;
}

static inline Type* type_unqual(Type* t) {
    return type_unq(t);
}

static inline Type* type_ptr(Type* base) {
    Type* t = type_new(TY_PTR);
    t->base = base;
    return t;
}

static inline void types_init(void) {
    Type* size = type_new(TY_ALIAS);
    size->name = "size_t";
    size->base = type_builtin(BK_ULONG);
    size->from_c = 1;
    g_size_t = size;
    Type* diff = type_new(TY_ALIAS);
    diff->name = "ptrdiff_t";
    diff->base = type_builtin(BK_LONG);
    diff->from_c = 1;
    g_ptrdiff_t = diff;
    g_char_ptr = type_ptr(type_builtin(BK_CHAR));
}

// Strip typedef names, keeping the qualifiers they carry
static inline Type* type_canon(Type* t) {
    int quals = 0;
    while (t->kind == TY_ALIAS && t->base) {
        quals |= t->quals;
        t = t->base;
    }
    return type_qual(t, quals);
}

static inline int type_is_unknown(Type* t) {
    return !t || type_canon(t)->kind == TY_UNKNOWN;
}

static inline int type_has_param(Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_PARAM: return 1;
        case TY_APP:
            for (int i = 0; i < t->nargs; i++)
                if (type_has_param(t->args[i])) return 1;
            return 0;
        case TY_ARRAY:
            if (t->size_param) return 1;
            return type_has_param(t->base);
        case TY_PTR:
            return type_has_param(t->base);
        case TY_FUNC:
            if (type_has_param(t->base)) return 1;
            for (int i = 0; i < t->nargs; i++)
                if (type_has_param(t->args[i])) return 1;
            return 0;
        default: return 0;
    }
}

// Exact type identity. Typedef names are transparent; top-level qualifiers
// count only when `top_quals` is set.
static inline int type_eq(Type* a, Type* b, int top_quals) {
    a = type_canon(a);
    b = type_canon(b);
    if (top_quals && a->quals != b->quals) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case TY_UNKNOWN: return 0;
        case TY_BUILTIN:
            if (a->builtin != b->builtin) return 0;
            return a->builtin != BK_OPAQUE || strcmp(a->name, b->name) == 0;
        case TY_PTR:
            return type_eq(a->base, b->base, 1);
        case TY_ARRAY:
            if (a->has_size_value && b->has_size_value) {
                if (a->size_value != b->size_value) return 0;
            } else if (a->size && b->size && strcmp(a->size, b->size) != 0) {
                return 0;
            }
            return type_eq(a->base, b->base, 1);
        case TY_CONST:
            // by value where known, else by spelling
            if (a->has_value && b->has_value) return a->value == b->value;
            return a->has_value == b->has_value && strcmp(a->name, b->name) == 0;
        case TY_FUNC:
            if (a->nargs != b->nargs || a->variadic != b->variadic) return 0;
            if (!type_eq(a->base, b->base, 1)) return 0;
            for (int i = 0; i < a->nargs; i++)
                if (!type_eq(a->args[i], b->args[i], 0)) return 0;
            return 1;
        case TY_STRUCT: case TY_ENUM: {
            Type* ua = type_unq(a);
            Type* ub = type_unq(b);
            if (ua == ub) return 1;
            // the same header type seen through two different include sets
            if (ua->from_c && ub->from_c && ua->is_union == ub->is_union) {
                const char* na = ua->name ? ua->name : ua->cname;
                const char* nb = ub->name ? ub->name : ub->cname;
                return na && nb && strcmp(na, nb) == 0;
            }
            return 0;
        }
        case TY_PARAM:
            return type_unq(a) == type_unq(b);
        case TY_APP:
            if (a->generic != b->generic || a->nargs != b->nargs) return 0;
            for (int i = 0; i < a->nargs; i++)
                if (!type_eq(a->args[i], b->args[i], 1)) return 0;
            return 1;
        case TY_ALIAS: return 0;    // only an unresolved alias gets here
    }
    return 0;
}

// Array-to-pointer and function-to-pointer conversion of a value
static inline Type* type_decay(Type* t) {
    Type* c = type_canon(t);
    if (c->kind == TY_ARRAY) return type_ptr(c->base);
    if (c->kind == TY_FUNC) return type_ptr(type_unq(c));
    return t;
}

// A key that is equal exactly for identical types (used to share instances)
static inline void type_key_into(Buf* b, Type* t) {
    t = type_canon(t);
    if (t->quals) buf_printf(b, "q%d ", t->quals);
    switch (t->kind) {
        case TY_UNKNOWN: buf_puts(b, "?"); break;
        case TY_BUILTIN: buf_puts(b, t->name); break;
        case TY_PTR: type_key_into(b, t->base); buf_putc(b, '*'); break;
        case TY_ARRAY:
            type_key_into(b, t->base);
            if (t->has_size_value) buf_printf(b, "[=%lld]", t->size_value);
            else buf_printf(b, "[%s]", t->size ? t->size : "");
            break;
        case TY_CONST:
            if (t->has_value) buf_printf(b, "=%lld", t->value);
            else buf_printf(b, "=`%s`", t->name);
            break;
        case TY_FUNC:
            buf_putc(b, '(');
            type_key_into(b, t->base);
            for (int i = 0; i < t->nargs; i++) {
                buf_putc(b, ';');
                type_key_into(b, t->args[i]);
            }
            buf_puts(b, t->variadic ? ";...)" : ")");
            break;
        case TY_STRUCT: case TY_ENUM: {
            Type* u = type_unq(t);
            const char* name = u->name ? u->name : u->cname;
            if (u->from_c && name) buf_printf(b, "c:%s", name);
            else buf_printf(b, "#%p", (void*)u);
            break;
        }
        case TY_PARAM: buf_printf(b, "?%s%p", t->name, (void*)type_unq(t)); break;
        case TY_APP:
            buf_printf(b, "app%p<", (void*)t->generic);
            for (int i = 0; i < t->nargs; i++) {
                type_key_into(b, t->args[i]);
                buf_putc(b, ',');
            }
            buf_putc(b, '>');
            break;
        case TY_ALIAS: buf_printf(b, "alias:%s", t->name); break;
    }
}

static inline const char* types_key(Type** types, int count) {
    Buf b = {0};
    for (int i = 0; i < count; i++) {
        type_key_into(&b, types[i]);
        buf_putc(&b, '|');
    }
    return buf_str(&b);
}

// An identifier fragment describing a type, used to tell specializations apart
static inline void type_ident_into(Buf* b, Type* t) {
    if (t->quals & Q_CONST) buf_puts(b, "const_");
    switch (t->kind) {
        case TY_BUILTIN:
            for (const char* c = t->name; *c; c++) buf_putc(b, *c == ' ' ? '_' : *c);
            break;
        case TY_PTR: type_ident_into(b, t->base); buf_puts(b, "_ptr"); break;
        case TY_ARRAY: type_ident_into(b, t->base); buf_puts(b, "_arr"); break;
        case TY_FUNC: buf_puts(b, "fn"); break;
        case TY_ALIAS: case TY_STRUCT: case TY_ENUM: case TY_APP: {
            Type* u = type_unq(t);
            buf_puts(b, u->name ? u->name : u->cname ? u->cname : "anon");
            if (u->is_inst || u->kind == TY_APP)
                for (int i = 0; i < u->nargs; i++) {
                    buf_putc(b, '_');
                    type_ident_into(b, u->args[i]);
                }
            break;
        }
        case TY_PARAM: buf_puts(b, t->name); break;
        case TY_UNKNOWN: buf_puts(b, "unknown"); break;
        case TY_CONST:
            if (t->has_value && t->value < 0) buf_printf(b, "m%lld", -t->value);
            else if (t->has_value) buf_printf(b, "%lld", t->value);
            else buf_puts(b, t->name);
            break;
    }
}

static inline const char* types_ident(Type** types, int count) {
    Buf b = {0};
    for (int i = 0; i < count; i++) {
        if (i) buf_putc(&b, '_');
        type_ident_into(&b, types[i]);
    }
    return buf_str(&b);
}

// Source-level spelling for diagnostics
static inline void type_show_into(Buf* b, Type* t) {
    if (!t) { buf_puts(b, "<none>"); return; }
    if (t->kind != TY_PTR) {
        if (t->quals & Q_CONST) buf_puts(b, "const ");
        if (t->quals & Q_VOLATILE) buf_puts(b, "volatile ");
    }
    switch (t->kind) {
        case TY_UNKNOWN: buf_puts(b, "<unknown>"); break;
        case TY_BUILTIN: case TY_PARAM: case TY_CONST: buf_puts(b, t->name); break;
        case TY_PTR:
            type_show_into(b, t->base);
            buf_putc(b, '*');
            if (t->quals & Q_CONST) buf_puts(b, " const");
            break;
        case TY_ARRAY:
            type_show_into(b, t->base);
            buf_printf(b, "[%s]", t->size ? "N" : "");
            break;
        case TY_FUNC:
            type_show_into(b, t->base);
            buf_putc(b, '(');
            for (int i = 0; i < t->nargs; i++) {
                if (i) buf_puts(b, ", ");
                type_show_into(b, t->args[i]);
            }
            buf_puts(b, t->variadic ? ", ...)" : ")");
            break;
        case TY_STRUCT: case TY_ENUM: case TY_ALIAS: case TY_APP: {
            Type* u = type_unq(t);
            if (u->is_inst || u->kind == TY_APP) {
                buf_puts(b, u->name);
                buf_putc(b, '<');
                for (int i = 0; i < u->nargs; i++) {
                    if (i) buf_puts(b, ", ");
                    type_show_into(b, u->args[i]);
                }
                buf_putc(b, '>');
            } else if (u->kind == TY_ALIAS) {
                buf_puts(b, u->name);
            } else if (u->name) {
                buf_printf(b, "%s %s", u->kind == TY_ENUM ? "enum" : u->is_union ? "union" : "struct", u->name);
            } else {
                buf_puts(b, u->cname ? u->cname : u->kind == TY_ENUM ? "enum <anonymous>" : "struct <anonymous>");
            }
            break;
        }
    }
}

static inline const char* type_show(Type* t) {
    Buf b = {0};
    type_show_into(&b, t);
    return buf_str(&b);
}

// ---------------------------------------------------------------------------
// Program structure
// ---------------------------------------------------------------------------

// A piece of the generated declaration section. Items are ordered so that
// every type is complete before something needs its size.
enum { IT_RAW, IT_TYPEDECL, IT_TAGDEF, IT_INSTANCE, IT_VARDECL, IT_STATIC_ASSERT };

struct Item {
    int kind;
    SrcFile* file;
    int file_index;         // position among the file's items
    Expr* node;
    Type* type;             // IT_INSTANCE
    Entity* ent;            // IT_VARDECL
    Item* prev_raw;         // the raw block this item follows in its file
    const char* text;
    int state;              // 0: pending, 1: being placed, 2: placed
};

enum { ENT_FUNC, ENT_VAR, ENT_TYPEDEF, ENT_GTYPEDEF, ENT_TRAIT, ENT_ENUMCONST, ENT_MACRO };

static const char* const entity_kind_names[] = {
    "function", "variable", "type", "generic type", "trait", "enumerator", "macro"
};

// Anything a name at file scope can refer to
struct Entity {
    int kind;
    const char* name;
    Module* mod;
    SrcFile* file;
    Token tok;
    Expr* decl;             // E_DECL, E_FUNCDEF, E_GTYPEDEF, E_TRAIT or E_ENUMERATOR
    Expr* group;            // the E_DECLGROUP holding an E_DECL
    int flags;              // F_* storage and F_PUB
    int from_c;
    int has_def;            // a variable defined (not just declared extern) in Typed C
    int state;              // 0: unresolved, 1: resolving, 2: done
    Type* type;
    Symbol* sym;
    Item* item;
    Item* prev_raw;
    Func* func;             // ENT_FUNC written in Typed C
    Map insts;              // ENT_GTYPEDEF: argument key -> Type*
    const char* owner;      // ENT_ENUMCONST: the name its enum is qualified by, if it has one
    Entity* twin;           // ENT_ENUMCONST: an enumerator of the same name in another enum
    const char* macro_body; // ENT_MACRO (object-like only)
    const char* init_text;  // ENT_VAR: generated initializer
};

// A function or method written in Typed C
struct Func {
    const char* name;
    Entity* ent;            // free functions
    Impl* impl;             // methods
    Expr* proto;            // E_DECL or E_FUNCDEF carrying the signature
    Expr* def;              // E_FUNCDEF with the body, if any
    Expr* generics;         // own generic parameters
    SrcFile* file;
    Module* mod;
    int flags;
    Map insts;              // argument key -> FuncInst*
};

// One concrete function in the generated C
struct FuncInst {
    Func* fn;
    TEnv* tenv;
    Type* self;
    Type* type;             // concrete TY_FUNC
    Symbol* sym;
    const char* body;
    int generated;
    int first;              // first specialization of its function: carries the comments
    FuncInst* requester;    // the specialization whose body first asked for this one
    const char* show;       // source spelling for diagnostics
};

struct Impl {
    Module* mod;
    SrcFile* file;
    Expr* decl;
    int nparams;
    Type** params;          // TY_PARAM placeholders of the impl's generics
    TEnv* tenv;             // generics bound to those placeholders
    Type* target;           // pattern
    Entity* trait;          // NULL for inherent impls
    Type** trait_args;      // patterns
    int ntrait_args;
    List methods;           // Func*
    int state;
};

// Generic parameter bindings, innermost first
struct TEnv {
    const char* name;
    Type* type;
    Expr* param;            // the generic parameter's declaration, with its bounds
    TEnv* next;
};

enum { L_VAR = 1, L_TYPEDEF = 2, L_TAG = 4, L_ENUMCONST = 8 };

// Block-scope names, innermost first
struct Local {
    int kind;
    const char* name;
    Type* type;
    Local* next;
};

typedef struct {
    const char* name;
    Module* mod;
    Token tok;
} Import;

struct SrcFile {
    const char* path;
    const char* stem;       // file name without directory and extension
    const char* label;      // module path + stem, the global-group label
    char* source;
    Module* mod;
    Expr* ast;
    Import* imports;
    int nimports;
    List includes;          // const char*: #include lines at file scope
    Module* cunit;          // what those headers declare
    List items;             // Item*
    Item* last_raw;
    Map statics;            // `static` functions and variables: visible in this file only
    Map dotops;             // dotted operators visible in this file -> packed binding powers
};

// A namespace (one directory), or the declarations of a set of C headers
struct Module {
    const char* name;       // "math::linear"; "" for the root
    List segments;          // const char*
    const char* dir;
    List files;             // SrcFile*
    Map names;              // ordinary identifiers -> Entity*
    Map tags;               // struct / union / enum tags -> Type*
    Map typenames;          // type names found by the prescan
    Map dotops;             // dotted operators declared by this namespace's traits
    Map generics;           // names of functions declared with generic parameters
    int is_c;
    int is_root;
    int has_raw;            // contains raw C: unknown identifiers may come from there
    int loaded;
};

// Where an expression or type is being resolved
struct Ctx {
    SrcFile* file;
    Module* mod;
    TEnv* tenv;
    Type* self;
    Local* locals;
    Type* ret;
    int in_func;
    int quiet;              // leave source comments out (later specializations)
    List* declared;         // collects the name of every local of the function being generated
    Expr* stop_syn;         // shared specifiers of the declaration being resolved...
    Type* stop_type;        // ...and the type they were resolved to
};

static List g_modules;      // Module*, imported namespaces before their importers
static List g_impls;        // Impl*
static List g_insts;        // FuncInst*, in generation order
static List g_items;        // Item*
static List g_type_insts;   // Type*: instances of generic typedefs

static inline Ctx ctx_for_file(SrcFile* file) {
    Ctx c = {0};
    c.file = file;
    c.mod = file->mod;
    return c;
}

static inline Item* item_new(int kind, SrcFile* file, Expr* node) {
    Item* it = (Item*)xalloc(sizeof(Item));
    it->kind = kind;
    it->file = file;
    it->node = node;
    if (file) {
        it->file_index = (int)file->items.len;
        it->prev_raw = file->last_raw;
        list_push(&file->items, it);
        if (kind == IT_RAW) file->last_raw = it;
    }
    list_push(&g_items, it);
    return it;
}

// file, then the enclosing namespaces from the innermost outwards
static inline void symbol_context_file(Symbol* s, SrcFile* file) {
    symbol_context(s, file->stem);
    for (size_t i = file->mod->segments.len; i-- > 0;)
        symbol_context(s, (const char*)file->mod->segments.items[i]);
    s->identity = xstrf("%s:%s", file->path, s->base);
}
