#pragma once
#include "load.h"

// Type resolution, generic inference, trait / method / operator resolution and
// the lowering of function bodies to C text. Generic code is checked once per
// specialization, with its type parameters bound to concrete types.

typedef struct {
    const char* s;      // generated C
    Type* t;
} Val;

static Type* resolve_type(Ctx* c, Expr* s);
static Val gen_expr(Ctx* c, Expr* e, Type* expected);
static Val gen_init(Ctx* c, Expr* e, Type* expected);
static void gen_stmt(Ctx* c, Expr* e, Buf* out, int ind);
static void struct_resolve(Type* t);
static Type* entity_type(Entity* e);
static void impl_prepare(Impl* im);
static const char* type_decl(Type* t, const char* name, Type* define, int indent);
static const char* raw_text(Token tok);
static int const_eval(Ctx* c, Expr* e, long long* out);
static int reflection_constant(Ctx* c, Expr* e, long long* out);
static Type* number_type(Token tok);

#define FAIL(c, tok, ...) fatal((c)->file->path, (tok), __VA_ARGS__)

// ---------------------------------------------------------------------------
// #line directives (--line-directives)
// ---------------------------------------------------------------------------

// Which line the C compiler should reports for the given code
static const char* line_at(SrcFile* f, Token tok) {
    if (!g_opt.lines || !f || !tok.row) return "";
    size_t row = tok.row;
    if (tok.length >= 2 && tok.start[0] == '#' && tok.start[1] == '{') {
        // a raw block is written without the blank space after its `#{`
        for (size_t i = 2; i < tok.length && isspace((unsigned char)tok.start[i]); i++)
            if (tok.start[i] == '\n') row++;
    }
    Buf b = {0};
    buf_printf(&b, "#line %zu \"", row);
    for (const char* p = f->path; *p; p++) {
        if (*p == '"' || *p == '\\') buf_putc(&b, '\\');
        buf_putc(&b, *p);
    }
    buf_puts(&b, "\"\n");
    return buf_str(&b);
}

// ... and from here on the generated file again. The line number is only known
// once the output is assembled, so this is a marker that `lines_finish` fills in.
#define LINE_MARK "#line \x04\n"
static const char* line_back(void) {
    return g_opt.lines ? LINE_MARK : "";
}

static const char* lines_finish(const char* text, const char* name) {
    if (!g_opt.lines) return text;
    Buf b = {0};
    size_t line = 1, mark = strlen(LINE_MARK);
    for (const char* p = text; *p;) {
        const char* end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) + 1 : strlen(p);
        if (n == mark && strncmp(p, LINE_MARK, mark) == 0)
            buf_printf(&b, "#line %zu \"%s\"\n", line + 1, name);
        else
            buf_putn(&b, p, n);
        line++;
        p += n;
    }
    return buf_str(&b);
}

// ---------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------

static inline Local* local_find(Ctx* c, int kinds, const char* name) {
    for (Local* l = c->locals; l; l = l->next)
        if ((l->kind & kinds) && strcmp(l->name, name) == 0) return l;
    return NULL;
}

static inline void local_push(Ctx* c, int kind, const char* name, Type* type) {
    Local* l = (Local*)xalloc(sizeof(Local));
    l->kind = kind;
    l->name = name;
    l->type = type;
    l->next = c->locals;
    c->locals = l;
    if (c->declared && kind != L_TAG) list_push(c->declared, (void*)name);
}

// ---------------------------------------------------------------------------
// Source comments
// ---------------------------------------------------------------------------

// One comment, re-indented: continuation lines of a block comment lose the
// indentation the comment had in the source and gain the output's.
static void put_comment_text(Buf* out, const char* s, size_t n, size_t column, int ind) {
    while (n && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    for (size_t i = 0; i < n; i++) {
        buf_putc(out, s[i]);
        if (s[i] != '\n') continue;
        size_t skipped = 0;
        while (i + 1 < n && skipped < column && (s[i + 1] == ' ' || s[i + 1] == '\t')) {
            i++;
            skipped++;
        }
        if (i + 1 < n && s[i + 1] != '\n') buf_indent(out, ind);
    }
}

// Carry the comments and blank lines found in front of an item into the
// output. A blank line is kept (as one) between things that are both emitted.
static void put_comments(Buf* out, const char* s, size_t n, int ind, int leading_blank, int trailing_blank, int quiet) {
    if (!s) return;
    size_t i = 0, line_start = 0;
    int newlines = 0, emitted = leading_blank;
    while (i < n) {
        if (s[i] == '\n') {
            newlines++;
            line_start = ++i;
            continue;
        }
        if (s[i] != '/' || i + 1 >= n || (s[i + 1] != '/' && s[i + 1] != '*')) {
            i++;
            continue;
        }
        size_t end = i + 2;
        if (s[i + 1] == '/') {
            while (end < n && s[end] != '\n') end++;
        } else {
            while (end + 1 < n && !(s[end] == '*' && s[end + 1] == '/')) end++;
            end = end + 2 <= n ? end + 2 : n;
        }
        if (!quiet) {
            if (newlines >= 2 && emitted) buf_putc(out, '\n');
            buf_indent(out, ind);
            put_comment_text(out, s + i, end - i, i - line_start, ind);
            buf_putc(out, '\n');
            emitted = 1;
            newlines = 0;
        } else if (newlines > 0) {
            newlines--;     // a dropped comment takes its own line with it
        }
        i = end;
    }
    if (trailing_blank && newlines >= 2 && emitted) buf_putc(out, '\n');
}

// Comments that finish an item's line go back onto the end of that line
static void put_trail(Buf* out, const char* s, size_t n, int quiet) {
    if (!s || !n || quiet) return;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!n) return;
    if (out->len && out->data[out->len - 1] == '\n') out->data[--out->len] = '\0';
    buf_putc(out, ' ');
    buf_putn(out, s, n);
    buf_putc(out, '\n');
}

// A file-scope name: the namespace's own declarations, then the file's C headers
static inline Entity* scope_entity(Ctx* c, const char* name) {
    Entity* e = file_entity(c->file, name);
    if (!e && c->file->cunit) e = (Entity*)map_get(&c->file->cunit->names, name);
    return e;
}

static inline TEnv* tenv_bind(Expr* generics, Type** vals, TEnv* next) {
    if (!generics) return next;
    for (size_t i = 0; i < generics->blockstmt_size; i++) {
        TEnv* e = (TEnv*)xalloc(sizeof(TEnv));
        e->name = tok_str(generics->blockstmt[i]->name);
        e->type = vals[i];
        e->param = generics->blockstmt[i];
        e->next = next;
        next = e;
    }
    return next;
}

static inline int generics_count(Expr* generics) {
    return generics ? (int)generics->blockstmt_size : 0;
}

// ---------------------------------------------------------------------------
// Printing types as C
// ---------------------------------------------------------------------------

static inline Type* type_spec_root(Type* t) {
    while (t->kind == TY_PTR || t->kind == TY_ARRAY || t->kind == TY_FUNC) t = t->base;
    return t;
}

static void struct_body_into(Buf* b, Type* u, int indent) {
    struct_resolve(u);
    buf_puts(b, " {\n");
    for (int i = 0; i < u->nfields; i++) {
        Field* f = &u->fields[i];
        put_comments(b, f->lead, f->lead_len, indent + 1, i > 0, 1, u->quiet);
        buf_indent(b, indent + 1);
        if (f->verbatim) {
            buf_printf(b, "%s\n", f->verbatim);
        } else if (u->kind == TY_ENUM) {
            buf_puts(b, f->sym ? symbol_ref(f->sym) : f->name);
            if (f->bits) buf_printf(b, " = %s", f->bits);
            buf_puts(b, ",\n");
        } else {
            if (f->align) buf_puts(b, f->align);
            Type* define = f->defines ? type_unq(type_spec_root(f->type)) : NULL;
            buf_puts(b, type_decl(f->type, f->name, define, indent + 1));
            if (f->bits) buf_printf(b, " : %s", f->bits);
            buf_puts(b, ";\n");
        }
        put_trail(b, f->trail, f->trail_len, u->quiet);
    }
    if (u->body) put_comments(b, u->body->inner, u->body->inner_len, indent + 1, u->nfields > 0, 0, u->quiet);
    buf_indent(b, indent);
    buf_putc(b, '}');
}

// The specifier part of a declaration. `define` is the struct / enum whose
// body belongs here; anonymous ones always carry their body.
static void type_spec_into(Buf* b, Type* t, Type* define, int indent) {
    Type* u = type_unq(t);
    if (t->quals & Q_CONST) buf_puts(b, "const ");
    if (t->quals & Q_VOLATILE) buf_puts(b, "volatile ");
    if (t->quals & Q_ATOMIC) buf_puts(b, "_Atomic ");
    switch (t->kind) {
        case TY_BUILTIN:
            buf_puts(b, t->name);
            break;
        case TY_ALIAS:
            buf_puts(b, u->sym ? symbol_ref(u->sym) : u->name);
            break;
        case TY_STRUCT: case TY_ENUM:
            if (u->is_inst) {
                buf_puts(b, symbol_ref(u->sym));
                break;
            }
            buf_puts(b, u->kind == TY_ENUM ? "enum" : u->is_union ? "union" : "struct");
            if (u->name) buf_printf(b, " %s", u->sym ? symbol_ref(u->sym) : u->name);
            if ((u == define || !u->name) && u->body) struct_body_into(b, u, indent);
            break;
        default:
            buf_puts(b, type_show(t));
            break;
    }
}

// Build the declarator around `inner` and report the specifier type it ends at
static const char* decl_build(Type* t, const char* inner, Type** leaf) {
    switch (t->kind) {
        case TY_PTR: {
            const char* s = xstrf("*%s%s%s%s",
                                  t->quals & Q_CONST ? "const " : "",
                                  t->quals & Q_VOLATILE ? "volatile " : "",
                                  t->quals & Q_RESTRICT ? "restrict " : "", inner);
            if (t->base->kind == TY_ARRAY || t->base->kind == TY_FUNC) s = xstrf("(%s)", s);
            return decl_build(t->base, s, leaf);
        }
        case TY_ARRAY:
            return decl_build(t->base, xstrf("%s[%s%s]", inner, t->arr_static ? "static " : "",
                                             t->size ? t->size : ""), leaf);
        case TY_FUNC: {
            Buf params = {0};
            if (t->voidparams) buf_puts(&params, "void");
            for (int i = 0; i < t->nargs; i++) {
                if (i) buf_puts(&params, ", ");
                buf_puts(&params, type_decl(t->args[i], t->pnames ? t->pnames[i] : NULL, NULL, 0));
            }
            if (t->variadic) buf_puts(&params, ", ...");
            return decl_build(t->base, xstrf("%s(%s)", inner, buf_str(&params)), leaf);
        }
        default:
            *leaf = t;
            return inner;
    }
}

// A complete C declaration of `name` with type `t` (no storage class, no ';').
// With a NULL name this is a type name.
static const char* type_decl(Type* t, const char* name, Type* define, int indent) {
    Type* leaf = NULL;
    const char* d = decl_build(t, name ? name : "", &leaf);
    Buf b = {0};
    type_spec_into(&b, leaf, define, indent);
    if (!*d) return buf_str(&b);
    if (d[0] == '*') {
        // `int* name`, matching how pointer types read in Typed C source
        size_t stars = strspn(d, "*");
        buf_putn(&b, d, stars);
        if (d[stars]) buf_putc(&b, ' ');
        buf_puts(&b, d + stars);
    } else {
        if (d[0] != '[') buf_putc(&b, ' ');
        buf_puts(&b, d);
    }
    // a trailing qualifier leaves a space behind: `char* const `
    while (b.len && b.data[b.len - 1] == ' ') b.data[--b.len] = '\0';
    return buf_str(&b);
}

// ---------------------------------------------------------------------------
// Resolving type syntax
// ---------------------------------------------------------------------------

static inline int quals_from_flags(int flags) {
    return (flags & F_CONST ? Q_CONST : 0) | (flags & F_VOLATILE ? Q_VOLATILE : 0) |
           (flags & F_RESTRICT ? Q_RESTRICT : 0) | (flags & F_ATOMIC ? Q_ATOMIC : 0);
}

static inline BuiltinKind builtin_from_mask(int mask) {
    int u = mask & B_UNSIGNED;
    if (mask & B_COMPLEX)
        return mask & B_FLOAT ? BK_CFLOAT : (mask & B_LONG) ? BK_CLDOUBLE : BK_CDOUBLE;
    if (mask & B_VOID) return BK_VOID;
    if (mask & B_BOOL) return BK_BOOL;
    if (mask & B_FLOAT) return BK_FLOAT;
    if (mask & B_DOUBLE) return mask & B_LONG ? BK_LDOUBLE : BK_DOUBLE;
    if (mask & B_CHAR) return u ? BK_UCHAR : (mask & B_SIGNED) ? BK_SCHAR : BK_CHAR;
    if (mask & B_SHORT) return u ? BK_USHORT : BK_SHORT;
    if (mask & B_LONGLONG) return u ? BK_ULLONG : BK_LLONG;
    if (mask & B_LONG) return u ? BK_ULONG : BK_LONG;
    return u ? BK_UINT : BK_INT;
}

// The file-scope entity a (possibly qualified) type-position name refers to
static Entity* lookup_type_entity(Ctx* c, Expr* s) {
    size_t n = s->blockstmt_size;
    const char* first = tok_str(s->blockstmt[0]->token);
    if (n == 1) return scope_entity(c, first);
    if (n > 2)
        FAIL(c, s->token, "'%s::...' has too many segments: a type is named 'namespace::Type'", first);
    Module* m = import_find(c->file, first);
    if (!m)
        FAIL(c, s->token, "unknown namespace '%s' (is it imported in this file?)", first);
    const char* name = tok_str(s->blockstmt[1]->token);
    Entity* e = (Entity*)map_get(&m->names, name);
    if (!e)
        FAIL(c, s->blockstmt[1]->token, "namespace '%s' has no member '%s'", m->name, name);
    if (m != c->mod && !(e->flags & F_PUB))
        FAIL(c, s->blockstmt[1]->token, "'%s' is private to namespace '%s' (not marked 'pub')", name, m->name);
    return e;
}

static Impl* bound_impl(Ctx* c, Type* t, Expr* bound);

// ---------------------------------------------------------------------------
// Constant generic parameters
// ---------------------------------------------------------------------------

static int type_is_integer(Type* t) {
    Type* c = type_canon(t);
    if (c->kind == TY_ENUM) return 1;
    return c->kind == TY_BUILTIN && c->builtin >= BK_BOOL && c->builtin <= BK_ULLONG;
}

// The declared type of a constant parameter, resolved where it is declared
static Type* const_param_type(SrcFile* decl_file, Expr* param) {
    Ctx c = ctx_for_file(decl_file);
    Type* t = resolve_type(&c, param->type);
    if (!type_is_integer(t))
        FAIL(&c, param->name, "constant parameter '%.*s' must have an integer type, not '%s'",
             (int)param->name.length, param->name.start, type_show(t));
    return t;
}

// The stand-in for a generic parameter inside a pattern
static Type* param_placeholder(SrcFile* decl_file, Expr* param) {
    Type* p = type_new(TY_PARAM);
    p->name = tok_str(param->name);
    if (param->type) {
        p->is_const = 1;
        p->base = const_param_type(decl_file, param);
    }
    return p;
}

// What a name bound by the generic environment stands for, if it is a constant
static Type* const_binding(Ctx* c, const char* name) {
    if (local_find(c, L_VAR | L_ENUMCONST, name)) return NULL;
    for (TEnv* e = c->tenv; e; e = e->next) {
        if (strcmp(e->name, name) != 0) continue;
        Type* u = type_unq(e->type);
        return u->kind == TY_CONST || (u->kind == TY_PARAM && u->is_const) ? u : NULL;
    }
    return NULL;
}

// The C text a constant argument is substituted as
static const char* const_text(Type* k) {
    if (!k->has_value) return xstrf("(%s)", k->name);
    return k->value < 0 ? xstrf("(%lld)", k->value) : xstrf("%lld", k->value);
}

static Type* const_from_value(Type* base, long long value) {
    Type* k = type_new(TY_CONST);
    k->base = base;
    k->has_value = 1;
    k->value = value;
    k->name = xstrf("%lld", value);
    return k;
}

// A constant argument becomes part of a type, so it must be a constant
// expression. What typedc cannot see into
// (macros, sizeof, raw C) is accepted and left to the C compiler.
static void const_require(Ctx* c, Expr* e, const char* pname) {
    switch (e->kind) {
        case E_IDENT: {
            Token tok = e->token;
            if (tok.kind == TK_STR) break;
            if (tok.kind == TK_PP_NUM && number_type(tok)->builtin >= BK_FLOAT) break;
            if (tok.kind != TK_IDENT) return;
            const char* name = tok_str(tok);
            if (const_binding(c, name)) return;
            Local* l = local_find(c, L_VAR | L_ENUMCONST, name);
            if (l && l->kind == L_ENUMCONST) return;
            Entity* ent = l ? NULL : scope_entity(c, name);
            if (l || (ent && (ent->kind == ENT_VAR || ent->kind == ENT_FUNC))) break;
            return;     // enum constant, macro, or a name only C knows
        }
        case E_PATH: case E_SIZEOF_TYPE: case E_RAW:
            return;
        case E_CAST:
            const_require(c, e->left, pname);
            return;
        case E_TERNARY:
            const_require(c, e->cases[1].condition, pname);
            const_require(c, e->cases[1].block, pname);
            const_require(c, e->cases[0].block, pname);
            return;
        case E_UNARY:
            if (token_kw(e->token, "sizeof")) return;
            if (token_is(e->token, "-") || token_is(e->token, "+") || token_is(e->token, "~") || token_is(e->token, "!")) {
                const_require(c, e->left, pname);
                return;
            }
            break;
        case E_BINARY: {
            static const char* const ok[] = {"+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^",
                                             "==", "!=", "<", ">", "<=", ">=", "&&", "||"};
            for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
                if (e->token.kind == TK_PUNCT && token_is(e->token, ok[i])) {
                    const_require(c, e->left, pname);
                    const_require(c, e->right, pname);
                    return;
                }
            }
            break;
        }
        case E_CALL: {
            if (reflection_constant(c, e, NULL)) return;
            // only something that can be a function-like macro
            Expr* callee = e->right;
            if (callee->kind == E_IDENT && callee->token.kind == TK_IDENT) {
                const char* name = tok_str(callee->token);
                Entity* ent = local_find(c, L_VAR, name) ? NULL : scope_entity(c, name);
                if (!local_find(c, L_VAR, name) && (!ent || ent->kind == ENT_MACRO)) return;
            }
            break;
        }
        default: break;
    }
    FAIL(c, e->token, "the argument for constant parameter '%s' must be a compile-time constant", pname);
}

// A type argument may be parsed as a name before its declaration is known.
static Expr* type_argument(Expr* arg) {
    if (arg->kind >= E_TY_BASE && arg->kind <= E_TY_HOLE) return arg;
    if ((arg->flags & F_PAREN) ||
        !((arg->kind == E_IDENT && arg->token.kind == TK_IDENT) || arg->kind == E_PATH)) return NULL;
    if (!arg->sema) {
        Expr* name = expr_new(E_TY_NAME, arg->token, arg->generics, NULL);
        if (arg->kind == E_IDENT) expr_push(name, expr_new(E_IDENT, arg->token, NULL, NULL));
        for (size_t i = 0; i < arg->blockstmt_size; i++) expr_push(name, arg->blockstmt[i]);
        arg->sema = name;
    }
    return (Expr*)arg->sema;
}

// Resolve one generic argument against the parameter it is passed for: a type
// for a type parameter, a constant expression for a constant parameter.
static Type* resolve_generic_arg(Ctx* c, SrcFile* decl_file, Expr* param, Expr* arg) {
    int type_syntax = arg->kind >= E_TY_BASE && arg->kind <= E_TY_HOLE;
    const char* pname = tok_str(param->name);

    if (!param->type) {
        Expr* type = type_argument(arg);
        if (!type)
            FAIL(c, arg->token, "generic parameter '%s' is a type, but a constant was given", pname);
        return resolve_type(c, type);
    }

    Expr* e = arg;
    if (type_syntax) {
        int name_only = arg->kind == E_TY_NAME && arg->blockstmt_size == 1 && !arg->left && !(arg->flags & F_QUALS);
        if (!name_only)
            FAIL(c, arg->token, "generic parameter '%s' is a constant, but a type was given", pname);
        if (!arg->sema) arg->sema = expr_new(E_IDENT, arg->blockstmt[0]->token, NULL, NULL);
        e = (Expr*)arg->sema;
    }
    Type* base = const_param_type(decl_file, param);
    // a constant parameter handed on as it is
    if (e->kind == E_IDENT && e->token.kind == TK_IDENT && !(e->flags & F_PAREN)) {
        Type* bound = const_binding(c, tok_str(e->token));
        if (bound) return bound;
    }
    const_require(c, e, pname);
    Val v = gen_expr(c, e, base);
    long long value;
    if (const_eval(c, e, &value)) return const_from_value(base, value);
    // not something typedc can evaluate (a macro, sizeof, ...): known by its spelling
    Type* k = type_new(TY_CONST);
    k->base = base;
    k->name = v.s;
    return k;
}

// A generic that asks for a new specialization of itself from inside one of its
// own specializations, over and over, never finishes: `f<T>` calling
// `f<Box<T>>`, or a struct `R<T>` holding an `R<Box<T>>*`. Whether such a chain
// ends cannot be decided in general, so its length is limited.
#define SPECIALIZATION_LIMIT 64

static Type* g_resolving_type;      // generic instance whose definition is being resolved
static FuncInst* g_generating;      // function specialization whose body is being generated

// `chain` holds the specializations of one generic, newest first
static _Noreturn void fail_specialization_chain(Ctx* c, Token at, const char* name, const char** chain, int n) {
    Buf b = {0};
    for (int i = n - 1; i >= 0 && i >= n - 3; i--)
        buf_printf(&b, "%s -> ", chain[i]);
    FAIL(c, at,
         "specializing '%s' exceeded nesting limit; "
         "stopped after %d nested specializations",
         name, SPECIALIZATION_LIMIT);
}

// Instantiate a generic typedef. With arguments that still mention a generic
// parameter this only records the application, for pattern matching.
static Type* gtypedef_inst(Ctx* c, Entity* e, Type** args, int nargs, Token at) {
    Expr* d = e->decl;
    if (nargs != generics_count(d->generics))
        FAIL(c, at, "generic type '%s' takes %d type argument(s), %d given",
             e->name, generics_count(d->generics), nargs);
    for (int i = 0; i < nargs; i++) {
        if (type_has_param(args[i])) {
            Type* app = type_new(TY_APP);
            app->name = e->name;
            app->generic = e;
            app->args = args;
            app->nargs = nargs;
            return app;
        }
    }
    const char* key = types_key(args, nargs);
    Type* t = (Type*)map_get(&e->insts, key);
    if (t) {
        if (t->kind == TY_ALIAS && !t->base)
            FAIL(c, at, "generic typedef '%s' refers to itself", e->name);
        return t;
    }

    Ctx dc = ctx_for_file(e->file);
    dc.tenv = tenv_bind(d->generics, args, NULL);
    for (int i = 0; i < nargs; i++) {
        Expr* param = d->generics->blockstmt[i];
        for (size_t j = 0; j < param->blockstmt_size; j++)
            if (!bound_impl(&dc, args[i], param->blockstmt[j]))
                FAIL(c, at, "type '%s' does not implement trait '%.*s', required by parameter '%.*s' of '%s'",
                     type_show(args[i]), (int)param->blockstmt[j]->token.length, param->blockstmt[j]->token.start,
                     (int)param->name.length, param->name.start, e->name);
    }

    Expr* spec = d->left;
    int is_struct = d->type == spec && spec->kind == E_TY_STRUCT && (spec->flags & F_HAS_BODY);
    t = type_new(is_struct ? TY_STRUCT : TY_ALIAS);
    t->name = e->name;
    t->is_inst = 1;
    t->generic = e;
    t->args = args;
    t->nargs = nargs;
    t->requester = g_resolving_type;
    const char* chain[SPECIALIZATION_LIMIT + 1];
    int nchain = 0;
    for (Type* r = t; r; r = r->requester) {
        if (r->generic != e) continue;
        chain[nchain++] = type_show(r);
        if (nchain > SPECIALIZATION_LIMIT) fail_specialization_chain(c, at, e->name, chain, nchain);
    }
    t->sym = symbol_new(e->name, SP_ORDINARY);
    symbol_context(t->sym, types_ident(args, nargs));
    symbol_context_file(t->sym, e->file);
    t->sym->identity = xstrf("%s<%s>", t->sym->identity, key);
    // source comments go to the first specialization only
    t->quiet = e->insts.len > 0;
    // registered before the body is resolved so the type can refer to itself
    map_put(&e->insts, key, t);
    if (is_struct) {
        t->is_union = (spec->flags & F_UNION) != 0;
        t->body = spec;
        t->file = e->file;
        t->tenv = dc.tenv;
    } else {
        Type* outer = g_resolving_type;
        g_resolving_type = t;
        t->base = resolve_type(&dc, d->type);
        g_resolving_type = outer;
    }
    t->item = item_new(IT_INSTANCE, NULL, d);
    t->item->file = e->file;
    t->item->prev_raw = e->prev_raw;
    t->item->type = t;
    list_push(&g_type_insts, t);
    return t;
}

// A type name: generic parameter, Self, typedef, or generic typedef instance
static Type* resolve_named(Ctx* c, Expr* s) {
    Expr* targs = s->left;
    const char* first = tok_str(s->blockstmt[0]->token);
    if (s->blockstmt_size == 1) {
        Type* found = NULL;
        Local* l = local_find(c, L_TYPEDEF, first);
        if (l) found = l->type;
        for (TEnv* e = c->tenv; e && !found; e = e->next)
            if (strcmp(e->name, first) == 0) found = e->type;
        if (!found && strcmp(first, "Self") == 0) {
            if (!c->self) FAIL(c, s->token, "'Self' is only a type inside a trait or impl");
            found = c->self;
        }
        if (found) {
            Type* u = type_unq(found);
            if (u->kind == TY_CONST || (u->kind == TY_PARAM && u->is_const))
                FAIL(c, s->token, "'%s' is a constant generic parameter, not a type", first);
            if (targs) FAIL(c, s->token, "'%s' does not take type arguments", first);
            return found;
        }
    }
    Entity* e = lookup_type_entity(c, s);
    if (!e) {
        if (is_builtin_type_name(first)) return type_opaque(first);
        FAIL(c, s->token, "unknown type name '%s'", first);
    }
    if (e->kind == ENT_TYPEDEF) {
        if (targs) FAIL(c, s->token, "'%s' is not a generic type and takes no type arguments", e->name);
        return entity_type(e);
    }
    if (e->kind == ENT_GTYPEDEF) {
        if (!targs) FAIL(c, s->token, "generic type '%s' requires type arguments, as in '%s<...>'", e->name, e->name);
        int n = (int)targs->blockstmt_size;
        if (n != generics_count(e->decl->generics))
            FAIL(c, s->token, "generic type '%s' takes %d argument(s), %d given",
                 e->name, generics_count(e->decl->generics), n);
        Type** args = (Type**)xalloc((size_t)n * sizeof(Type*));
        for (int i = 0; i < n; i++)
            args[i] = resolve_generic_arg(c, e->file, e->decl->generics->blockstmt[i], targs->blockstmt[i]);
        return gtypedef_inst(c, e, args, n, s->token);
    }
    FAIL(c, s->token, "'%s' is a %s, not a type", e->name, entity_kind_names[e->kind]);
}

static void resolve_fields(Ctx* c, Type* t) {
    Expr* body = t->body;
    t->state = 1;
    Field* fields = (Field*)xalloc((body->blockstmt_size + 1) * sizeof(Field));
    int n = 0;
    for (size_t i = 0; i < body->blockstmt_size; i++) {
        Expr* m = body->blockstmt[i];
        Field* f = &fields[n++];
        f->lead = m->lead;
        f->lead_len = m->lead_len;
        f->trail = m->trail;
        f->trail_len = m->trail_len;
        switch (m->kind) {
            case E_FIELD:
                if (m->name.length) f->name = tok_str(m->name);
                f->type = resolve_type(c, m->type);
                if (m->left) f->bits = gen_expr(c, m->left, NULL).s;
                f->defines = spec_defines_type(m->right);
                if (m->right && m->right->right && m->right->right->kind == E_ALIGNAS) {
                    Buf a = {0};
                    for (Expr* x = m->right->right; x; x = x->right)
                        buf_printf(&a, "_Alignas(%s) ", x->type ? type_decl(resolve_type(c, x->type), NULL, NULL, 0)
                                                               : gen_expr(c, x->left, NULL).s);
                    f->align = buf_str(&a);
                }
                break;
            case E_ENUMERATOR:
                f->name = tok_str(m->name);
                if (m->left) f->bits = gen_expr(c, m->left, NULL).s;
                if (m->sema) f->sym = ((Entity*)m->sema)->sym;
                // later enumerators may use this one in their value
                if (c->in_func) local_push(c, L_ENUMCONST, f->name, t);
                break;
            case E_RAW:
                f->verbatim = raw_text(m->token);
                break;
            case E_INCLUDE:
                f->verbatim = xstrf("\n%s", tok_str(m->token));
                break;
            case E_STATIC_ASSERT:
                f->verbatim = xstrf("_Static_assert(%s, %s);", gen_expr(c, m->left, NULL).s, tok_str(m->right->token));
                break;
            default: n--; break;
        }
    }
    t->fields = fields;
    t->nfields = n;
    t->state = 2;
}

// Resolve a struct / union / enum body the first time its members matter
static void struct_resolve(Type* t) {
    t = type_unq(t);
    if (t->state) return;
    if (!t->body) {
        t->state = 2;
        t->opaque = 1;      // incomplete: defined in C, if anywhere
        return;
    }
    Ctx c = ctx_for_file(t->file);
    c.tenv = t->tenv;
    c.self = t->self;
    if (!t->from_c) {
        Type* outer = g_resolving_type;
        jmp_buf failed;
        jmp_buf* recover = g_recover;
        if (g_try_depth == 0) {
            g_recover = &failed;
            if (setjmp(failed) != 0) {
                // reported; its members stay unknown so that uses are not errors too
                t->opaque = 1;
                t->state = 2;
                g_resolving_type = outer;
                error_continue(recover);
            }
        }
        if (t->is_inst) g_resolving_type = t;
        resolve_fields(&c, t);
        g_resolving_type = outer;
        g_recover = recover;
        return;
    }
    jmp_buf attempt;
    int depth = g_try_depth;
    g_try_stack[g_try_depth++] = &attempt;
    if (setjmp(attempt) == 0) {
        resolve_fields(&c, t);
    } else {
        t->opaque = 1;
        t->state = 2;
    }
    g_try_depth = depth;
}

static Field* struct_field(Type* t, const char* name) {
    t = type_unq(t);
    struct_resolve(t);
    for (int i = 0; i < t->nfields; i++) {
        Field* f = &t->fields[i];
        if (f->verbatim) continue;
        if (f->name) {
            if (strcmp(f->name, name) == 0) return f;
        } else if (f->type) {
            // members of an anonymous struct / union belong to the enclosing type
            Type* inner = type_canon(f->type);
            if (inner->kind == TY_STRUCT) {
                Field* found = struct_field(inner, name);
                if (found) return found;
            }
        }
    }
    return NULL;
}

static Type* resolve_tagged(Ctx* c, Expr* s) {
    if (s->sema) return (Type*)s->sema;
    TypeKind kind = s->kind == E_TY_ENUM ? TY_ENUM : TY_STRUCT;
    const char* name = s->name.length ? tok_str(s->name) : NULL;

    if (s->left) {
        // a tag of an imported namespace
        const char* word = kind == TY_ENUM ? "enum" : s->flags & F_UNION ? "union" : "struct";
        const char* ns = tok_str(s->left->token);
        Module* m = import_find(c->file, ns);
        if (!m) FAIL(c, s->left->token, "unknown namespace '%s' (is it imported in this file?)", ns);
        Type* t = (Type*)map_get(&m->tags, name);
        if (!t || !t->body || t->kind != kind || (kind == TY_STRUCT && t->is_union != ((s->flags & F_UNION) != 0)))
            FAIL(c, s->name, "namespace '%s' has no %s '%s'", m->name, word, name);
        if (m != c->mod && !t->is_pub)
            FAIL(c, s->name, "'%s %s' is private to namespace '%s' (not marked 'pub')", word, name, m->name);
        return t;
    }
    if (!(s->flags & F_HAS_BODY)) {
        Local* l = local_find(c, L_TAG, name);
        if (l) return l->type;
        Type* t = (Type*)map_get(&c->mod->tags, name);
        if (!t && c->file->cunit) t = (Type*)map_get(&c->file->cunit->tags, name);
        if (!t) t = tag_type(c->file, name, kind, (s->flags & F_UNION) != 0);
        return t;
    }

    // a definition inside a function, or inside generic code (one per specialization)
    Type* t = type_new(kind);
    t->name = name;
    t->is_union = (s->flags & F_UNION) != 0;
    t->from_c = c->mod->is_c;
    t->body = s;
    t->file = c->file;
    t->tenv = c->tenv;
    t->self = c->self;
    t->quiet = c->quiet;
    if (!c->tenv && !c->self) s->sema = t;
    if (c->in_func) {
        if (name) local_push(c, L_TAG, name, t);
        // local types can use local names, so their members are resolved in place
        resolve_fields(c, t);
    }
    return t;
}

static Type* resolve_type(Ctx* c, Expr* s) {
    int quals = quals_from_flags(s->flags);
    switch (s->kind) {
        case E_TY_HOLE:
            return resolve_type(c, s->left);
        case E_TY_BASE:
            return type_qual(type_builtin(builtin_from_mask(s->aux)), quals);
        case E_TY_NAME:
            return type_qual(resolve_named(c, s), quals);
        case E_TY_STRUCT: case E_TY_ENUM:
            return type_qual(resolve_tagged(c, s), quals);
        case E_TY_PTR:
            return type_qual(type_ptr(resolve_type(c, s->left)), quals);
        case E_TY_ARRAY: {
            Type* t = type_new(TY_ARRAY);
            t->base = resolve_type(c, s->left);
            if (s->right) {
                Expr* size = s->right;
                Type* bound = size->kind == E_IDENT && size->token.kind == TK_IDENT && !(size->flags & F_PAREN)
                                  ? const_binding(c, tok_str(size->token)) : NULL;
                if (bound && bound->kind == TY_PARAM) {
                    // a pattern: the size is inferred from the argument
                    t->size = bound->name;
                    t->size_param = bound;
                } else if (bound) {
                    t->size = bound->has_value ? xstrf("%lld", bound->value) : bound->name;
                    t->has_size_value = bound->has_value;
                    t->size_value = bound->value;
                } else {
                    t->size = gen_expr(c, size, NULL).s;
                    t->has_size_value = const_eval(c, size, &t->size_value);
                }
            }
            else if (s->flags & F_ARR_STAR) t->size = "*";
            t->arr_static = (s->flags & F_ARR_STATIC) != 0;
            return t;
        }
        case E_TY_FUNC: {
            Type* t = type_new(TY_FUNC);
            t->base = resolve_type(c, s->left);
            t->nargs = (int)s->blockstmt_size;
            t->args = (Type**)xalloc((size_t)t->nargs * sizeof(Type*));
            t->pnames = (const char**)xalloc((size_t)t->nargs * sizeof(char*));
            for (int i = 0; i < t->nargs; i++) {
                Expr* param = s->blockstmt[i];
                t->args[i] = resolve_type(c, param->type);
                t->pnames[i] = param->name.length ? tok_str(param->name) : NULL;
            }
            t->variadic = (s->flags & F_VARIADIC) != 0;
            t->voidparams = (s->flags & F_VOIDPARAMS) != 0;
            return t;
        }
        default:
            FAIL(c, s->token, "expected a type");
    }
}

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

static FuncInst* func_inst(Ctx* c, Func* f, Type** ivals, Type* self, Type** own, Token at);

// Resolve the declared type of an entity. Header declarations that use
// something Typed C cannot model simply have an unknown type.
static Type* entity_decl_type(Entity* e) {
    Ctx c = ctx_for_file(e->file);
    if (!e->from_c) return resolve_type(&c, e->decl->type);
    jmp_buf attempt;
    Type* volatile result = &g_unknown;
    int depth = g_try_depth;
    g_try_stack[g_try_depth++] = &attempt;
    if (setjmp(attempt) == 0)
        result = resolve_type(&c, e->decl->type);
    g_try_depth = depth;
    return result;
}

// The type of an object-like macro, found by typing its replacement text
static Type* macro_type(Entity* e) {
    if (e->state) return e->type ? e->type : &g_unknown;
    e->state = 1;
    e->type = &g_unknown;
    if (!e->macro_body || !*e->macro_body) return e->type;

    Parser* p = (Parser*)xalloc(sizeof(Parser));
    *p = parser_new(e->macro_body);
    p->filename = "<macro>";
    p->header_mode = 1;
    p->env = e->file;
    p->lookup_type = env_lookup_type;
    parser_skip_noise(p);

    jmp_buf parse_attempt;
    Expr* volatile expr = NULL;
    p->spec_jmp[p->spec_depth++] = &parse_attempt;
    if (setjmp(parse_attempt) == 0) {
        Expr* parsed = parse_expression(p);
        if (p->token.kind == TK_EOF) expr = parsed;
    }
    if (!expr) return e->type;

    jmp_buf attempt;
    Type* volatile result = &g_unknown;
    int depth = g_try_depth;
    g_try_stack[g_try_depth++] = &attempt;
    if (setjmp(attempt) == 0) {
        Ctx c = ctx_for_file(e->file);
        result = gen_expr(&c, expr, NULL).t;
    }
    g_try_depth = depth;
    e->type = result;
    return e->type;
}

static Type* entity_type_resolve(Entity* e);

static Type* entity_type(Entity* e) {
    if (e->kind == ENT_MACRO || e->state || g_try_depth) return entity_type_resolve(e);
    jmp_buf failed;
    jmp_buf* recover = g_recover;
    g_recover = &failed;
    if (setjmp(failed) != 0) {
        // reported; the declaration keeps an unknown type so that uses are not errors too
        e->type = &g_unknown;
        e->state = 2;
        error_continue(recover);
    }
    Type* t = entity_type_resolve(e);
    g_recover = recover;
    return t;
}

static Type* entity_type_resolve(Entity* e) {
    if (e->kind == ENT_MACRO) return macro_type(e);
    if (e->state == 2) return e->type;
    if (e->state == 1) {
        if (e->from_c) return &g_unknown;
        fatal(e->file->path, e->tok, "'%s' is defined in terms of itself", e->name);
    }
    e->state = 1;
    switch (e->kind) {
        case ENT_TYPEDEF: {
            Type* base = entity_decl_type(e);
            Type* alias = type_new(TY_ALIAS);
            alias->name = e->name;
            alias->base = base;
            alias->sym = e->sym;
            alias->from_c = e->from_c;
            alias->item = e->item;
            // an anonymous struct is known by its typedef name
            Type* u = type_unq(base);
            if ((u->kind == TY_STRUCT || u->kind == TY_ENUM) && !u->name && !u->cname)
                u->cname = e->name;
            e->type = alias;
            break;
        }
        case ENT_VAR:
            e->type = entity_decl_type(e);
            break;
        case ENT_FUNC:
            if (e->func) {
                if (e->func->generics)
                    fatal(e->file->path, e->tok, "generic function '%s' has no type without type arguments", e->name);
                Ctx c = ctx_for_file(e->file);
                e->type = func_inst(&c, e->func, NULL, NULL, NULL, e->tok)->type;
            } else {
                e->type = entity_decl_type(e);
            }
            break;
        default:
            e->type = &g_unknown;
            break;
    }
    e->state = 2;
    return e->type;
}

// ---------------------------------------------------------------------------
// Pattern matching: generic inference and implementation selection
// ---------------------------------------------------------------------------

typedef struct {
    int n;
    Type** params;      // TY_PARAM placeholders
    Type** vals;        // what each is bound to so far
    int conflict;       // index + 1 of a parameter that got two different types
    Type* other;        // the second, conflicting type
} Bind;

static int bind_index(Bind* b, Type* param) {
    param = type_unq(param);
    for (int i = 0; i < b->n; i++)
        if (b->params[i] == param) return i;
    return -1;
}

// Match a pattern against a concrete type, binding generic parameters.
// A parameter that appears twice must receive exactly the same type both times.
static int unify(Type* pat, Type* t, Bind* b) {
    if (pat->kind == TY_PARAM) {
        int i = bind_index(b, pat);
        if (i < 0) return type_eq(pat, t, 0);
        // a constant parameter takes a constant, a type parameter a type
        if (type_unq(pat)->is_const != (t->kind == TY_CONST)) return 0;
        // `const T` against `const int` binds T to int
        Type* val = type_qual(type_unq(t), t->quals & ~pat->quals);
        if (b->vals[i]) {
            if (type_eq(b->vals[i], val, 1)) return 1;
            b->conflict = i + 1;
            b->other = val;
            return 0;
        }
        b->vals[i] = val;
        return 1;
    }
    if (!type_has_param(pat)) return type_eq(pat, t, 0);
    if (pat->kind == TY_APP) {
        // look through typedef names for an instance of the same generic type
        for (Type* x = t;;) {
            Type* u = type_unq(x);
            if (u->is_inst && u->generic == pat->generic) {
                for (int i = 0; i < pat->nargs; i++)
                    if (!unify(pat->args[i], u->args[i], b)) return 0;
                return 1;
            }
            if (x->kind != TY_ALIAS || !x->base) return 0;
            x = x->base;
        }
    }
    Type* ct = type_canon(t);
    switch (pat->kind) {
        case TY_PTR:
            return ct->kind == TY_PTR && unify(pat->base, ct->base, b);
        case TY_ARRAY:
            if (ct->kind != TY_ARRAY || !unify(pat->base, ct->base, b)) return 0;
            if (pat->size_param) {
                // `T (*a)[N]`: the array's size is the constant
                if (!ct->size) return 0;
                Type* k = type_new(TY_CONST);
                k->base = pat->size_param->base;
                k->has_value = ct->has_size_value;
                k->value = ct->size_value;
                k->name = ct->has_size_value ? xstrf("%lld", ct->size_value) : ct->size;
                return unify(pat->size_param, k, b);
            }
            return 1;
        case TY_FUNC:
            if (ct->kind != TY_FUNC || ct->nargs != pat->nargs) return 0;
            if (!unify(pat->base, ct->base, b)) return 0;
            for (int i = 0; i < pat->nargs; i++)
                if (!unify(pat->args[i], type_unq(ct->args[i]), b)) return 0;
            return 1;
        default:
            return 0;
    }
}

// Could two implementation targets apply to the same type? Conservative: a
// generic parameter on either side matches anything.
static int types_overlap(Type* a, Type* b) {
    if (a->kind == TY_PARAM || b->kind == TY_PARAM) return 1;
    Type* ua = type_unq(a);
    Type* ub = type_unq(b);
    int ga = ua->kind == TY_APP || ua->is_inst;
    int gb = ub->kind == TY_APP || ub->is_inst;
    if (ga || gb) {
        if (!ga || !gb || ua->generic != ub->generic) {
            // an alias instance may still be the same type underneath
            if (ua->kind == TY_APP || ub->kind == TY_APP) return 0;
            return type_eq(a, b, 0);
        }
        for (int i = 0; i < ua->nargs; i++)
            if (!types_overlap(ua->args[i], ub->args[i])) return 0;
        return 1;
    }
    Type* ca = type_canon(a);
    Type* cb = type_canon(b);
    if (ca->kind != cb->kind) return 0;
    switch (ca->kind) {
        case TY_PTR: case TY_ARRAY:
            // `char*` and `const char*` are different types: below a pointer
            // the qualifiers count, as they do in method lookup
            if (ca->base->kind != TY_PARAM && cb->base->kind != TY_PARAM &&
                type_canon(ca->base)->quals != type_canon(cb->base)->quals)
                return 0;
            return types_overlap(ca->base, cb->base);
        case TY_FUNC:
            if (ca->nargs != cb->nargs || !types_overlap(ca->base, cb->base)) return 0;
            for (int i = 0; i < ca->nargs; i++)
                if (!types_overlap(ca->args[i], cb->args[i])) return 0;
            return 1;
        default:
            return type_eq(ca, cb, 0);
    }
}

static int type_mentions(Type* t, Type* param) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_PARAM: return type_unq(t) == param;
        case TY_ARRAY:
            if (t->size_param == param) return 1;
            return type_mentions(t->base, param);
        case TY_PTR: return type_mentions(t->base, param);
        case TY_APP: case TY_FUNC:
            if (t->kind == TY_FUNC && type_mentions(t->base, param)) return 1;
            for (int i = 0; i < t->nargs; i++)
                if (type_mentions(t->args[i], param)) return 1;
            return 0;
        default: return 0;
    }
}

static void impl_prepare(Impl* im) {
    if (im->state) return;
    im->state = 1;
    Expr* g = im->decl->generics;
    im->nparams = generics_count(g);
    im->params = (Type**)xalloc((size_t)(im->nparams + 1) * sizeof(Type*));
    for (int i = 0; i < im->nparams; i++)
        im->params[i] = param_placeholder(im->file, g->blockstmt[i]);
    im->tenv = tenv_bind(g, im->params, NULL);

    Ctx c = ctx_for_file(im->file);
    c.tenv = im->tenv;
    Type* target = resolve_type(&c, im->decl->type);
    Type* u = type_unq(target);
    if ((u->kind == TY_STRUCT || u->kind == TY_ENUM) && !u->is_inst)
        FAIL(&c, im->decl->type->token, "an implementation target must be a named type: give this %s a typedef name",
             u->kind == TY_ENUM ? "enum" : "struct");
    for (int i = 0; i < im->nparams; i++)
        if (!type_mentions(target, im->params[i]))
            FAIL(&c, g->blockstmt[i]->name, "generic parameter '%s' is not used by the implementation target '%s'",
                 im->params[i]->name, type_show(target));

    Expr* tref = im->decl->left;
    if (tref) {
        c.self = target;
        Entity* tr = lookup_type_entity(&c, tref);
        if (!tr) FAIL(&c, tref->token, "unknown trait '%.*s'", (int)tref->token.length, tref->token.start);
        if (tr->kind != ENT_TRAIT)
            FAIL(&c, tref->token, "'%s' is a %s, not a trait", tr->name, entity_kind_names[tr->kind]);
        im->ntrait_args = tref->left ? (int)tref->left->blockstmt_size : 0;
        if (im->ntrait_args != generics_count(tr->decl->generics))
            FAIL(&c, tref->token, "trait '%s' takes %d type argument(s), %d given",
                 tr->name, generics_count(tr->decl->generics), im->ntrait_args);
        im->trait_args = (Type**)xalloc((size_t)(im->ntrait_args + 1) * sizeof(Type*));
        for (int i = 0; i < im->ntrait_args; i++)
            im->trait_args[i] = resolve_generic_arg(&c, tr->file, tr->decl->generics->blockstmt[i],
                                                    tref->left->blockstmt[i]);
        im->trait = tr;
    }
    im->target = target;
    im->state = 2;
}

// Does the implementation apply to this type? Fills the impl's generic arguments.
static int impl_match(Impl* im, Type* t, Bind* b) {
    if (!im->target) return 0;
    b->n = im->nparams;
    b->params = im->params;
    b->vals = (Type**)xalloc((size_t)(im->nparams + 1) * sizeof(Type*));
    b->conflict = 0;
    return unify(im->target, type_unq(t), b);
}

// The impl's own bounds, checked once its parameters are known
static int impl_bounds_hold(Impl* im, Type** vals) {
    Expr* g = im->decl->generics;
    if (!g) return 1;
    Ctx c = ctx_for_file(im->file);
    c.tenv = tenv_bind(g, vals, NULL);
    for (int i = 0; i < im->nparams; i++) {
        if (!vals[i]) return 0;
        Expr* param = g->blockstmt[i];
        for (size_t j = 0; j < param->blockstmt_size; j++)
            if (!bound_impl(&c, vals[i], param->blockstmt[j])) return 0;
    }
    return 1;
}

// The implementation through which type `t` satisfies the trait named by
// `bound` (resolved in `c`), or NULL
static Impl* bound_impl(Ctx* c, Type* t, Expr* bound) {
    Entity* tr = lookup_type_entity(c, bound);
    if (!tr) FAIL(c, bound->token, "unknown trait '%.*s'", (int)bound->token.length, bound->token.start);
    if (tr->kind != ENT_TRAIT)
        FAIL(c, bound->token, "'%s' is a %s; a bound must name a trait", tr->name, entity_kind_names[tr->kind]);
    int nargs = bound->left ? (int)bound->left->blockstmt_size : 0;
    if (nargs != generics_count(tr->decl->generics))
        FAIL(c, bound->token, "trait '%s' takes %d type argument(s), %d given",
             tr->name, generics_count(tr->decl->generics), nargs);
    Type** args = (Type**)xalloc((size_t)(nargs + 1) * sizeof(Type*));
    for (int i = 0; i < nargs; i++)
        args[i] = resolve_generic_arg(c, tr->file, tr->decl->generics->blockstmt[i], bound->left->blockstmt[i]);

    Impl* found = NULL;
    for (size_t i = 0; i < g_impls.len; i++) {
        Impl* im = (Impl*)g_impls.items[i];
        impl_prepare(im);
        if (im->trait != tr) continue;
        Bind b = {0};
        if (!impl_match(im, t, &b)) continue;
        int ok = 1;
        for (int k = 0; k < nargs && ok; k++)
            ok = unify(im->trait_args[k], args[k], &b);
        if (!ok || !impl_bounds_hold(im, b.vals)) continue;
        if (found)
            FAIL(c, bound->token, "type '%s' has competing implementations of trait '%s' (%s:%zu and %s:%zu)",
                 type_show(t), tr->name, found->file->path, found->decl->token.row,
                 im->file->path, im->decl->token.row);
        found = im;
    }
    return found;
}

// Is `im` what a trait bound in scope requires of the receiver type? Inside
// `T twice<T, X: Transform<T>>(X x, T v)` with X = Scale and T = int, this
// holds for `impl Transform<int> for Scale` only.
static int impl_is_bound(Ctx* c, Impl* im, Type* recv) {
    for (TEnv* e = c->tenv; e; e = e->next) {
        if (!e->param || !e->type || e->type->kind == TY_CONST || !type_eq(e->type, recv, 0)) continue;
        for (size_t j = 0; j < e->param->blockstmt_size; j++)
            if (bound_impl(c, recv, e->param->blockstmt[j]) == im) return 1;
    }
    return 0;
}

// Select the one method / dotted operator that applies to a receiver type
static Func* find_method(Ctx* c, Type* recv, const char* name, Token at, Type*** ivals) {
    Func* found = NULL;
    Func* second = NULL;
    List others = {0};      // further candidates and their impl arguments, in pairs
    for (size_t i = 0; i < g_impls.len; i++) {
        Impl* im = (Impl*)g_impls.items[i];
        Func* method = NULL;
        for (size_t j = 0; j < im->methods.len && !method; j++)
            if (strcmp(((Func*)im->methods.items[j])->name, name) == 0)
                method = (Func*)im->methods.items[j];
        if (!method) continue;
        impl_prepare(im);
        Bind b = {0};
        if (!impl_match(im, recv, &b) || !impl_bounds_hold(im, b.vals)) continue;
        if (found) {
            list_push(&others, method);
            list_push(&others, b.vals);
            continue;
        }
        found = method;
        *ivals = b.vals;
    }
    if (others.len) {
        // Several implementations apply. In generic code the trait bounds on
        // the receiver's type say which one is meant.
        list_push(&others, found);
        list_push(&others, *ivals);
        second = (Func*)others.items[0];
        Func* first = found;
        int chosen = 0;
        for (size_t i = 0; i < others.len; i += 2) {
            Func* method = (Func*)others.items[i];
            if (!impl_is_bound(c, method->impl, recv)) continue;
            found = method;
            *ivals = (Type**)others.items[i + 1];
            chosen++;
        }
        if (chosen != 1)
            FAIL(c, at, "ambiguous %s '%s' for type '%s': the implementations at %s:%zu and %s:%zu both apply",
                 name[0] == '.' ? "operator" : "method", name, type_show(recv),
                 first->file->path, first->def->name.row, second->file->path, second->def->name.row);
    }
    if (!found)
        FAIL(c, at, "type '%s' has no %s '%s'", type_show(recv), name[0] == '.' ? "operator" : "method", name);
    Impl* im = found->impl;
    if (found->mod != c->mod && !(found->flags & F_PUB) && !(im->trait && (im->trait->flags & F_PUB)))
        FAIL(c, at, "method '%s' is private to namespace '%s' (not marked 'pub')", name, found->mod->name);
    return found;
}

// Is there any method / dotted operator of this name for the receiver type?
static int has_method(Type* recv, const char* name) {
    for (size_t i = 0; i < g_impls.len; i++) {
        Impl* im = (Impl*)g_impls.items[i];
        for (size_t j = 0; j < im->methods.len; j++) {
            if (strcmp(((Func*)im->methods.items[j])->name, name) != 0) continue;
            impl_prepare(im);
            Bind b = {0};
            if (impl_match(im, recv, &b) && impl_bounds_hold(im, b.vals)) return 1;
        }
    }
    return 0;
}

static Type* impl_self(Impl* im, Type** ivals) {
    Ctx c = ctx_for_file(im->file);
    c.tenv = tenv_bind(im->decl->generics, ivals, NULL);
    return resolve_type(&c, im->decl->type);
}

// ---------------------------------------------------------------------------
// Function specializations
// ---------------------------------------------------------------------------

// The concrete function for a set of type arguments, created on first request
// and queued for code generation.
static FuncInst* func_inst(Ctx* c, Func* f, Type** ivals, Type* self, Type** own, Token at) {
    Impl* im = f->impl;
    int nimpl = im ? im->nparams : 0;
    int nown = generics_count(f->generics);
    const char* key = xstrf("%s/%s", types_key(ivals, nimpl), types_key(own, nown));
    FuncInst* fi = (FuncInst*)map_get(&f->insts, key);
    if (fi) return fi;

    fi = (FuncInst*)xalloc(sizeof(FuncInst));
    fi->fn = f;
    fi->first = f->insts.len == 0;
    fi->self = self;
    fi->tenv = tenv_bind(f->generics, own, im ? tenv_bind(im->decl->generics, ivals, NULL) : NULL);
    fi->requester = g_generating;
    {
        Buf show = {0};
        if (self) buf_printf(&show, "%s.", type_show(self));
        buf_puts(&show, f->name);
        for (int i = 0; i < nown; i++)
            buf_printf(&show, "%s%s%s", i ? ", " : "<", type_show(own[i]), i == nown - 1 ? ">" : "");
        fi->show = buf_str(&show);
    }
    const char* chain[SPECIALIZATION_LIMIT + 1];
    int nchain = 0;
    for (FuncInst* r = fi; r; r = r->requester) {
        if (r->fn != f) continue;
        chain[nchain++] = r->show;
        if (nchain > SPECIALIZATION_LIMIT) fail_specialization_chain(c, at, f->name, chain, nchain);
    }

    Ctx fc = ctx_for_file(f->file);
    fc.tenv = fi->tenv;
    fc.self = self;
    for (int i = 0; i < nown; i++) {
        Expr* param = f->generics->blockstmt[i];
        for (size_t j = 0; j < param->blockstmt_size; j++)
            if (!bound_impl(&fc, own[i], param->blockstmt[j]))
                FAIL(c, at, "type '%s' does not implement trait '%.*s', required by parameter '%.*s' of '%s'",
                     type_show(own[i]), (int)param->blockstmt[j]->token.length, param->blockstmt[j]->token.start,
                     (int)param->name.length, param->name.start, f->name);
    }
    fi->type = type_canon(resolve_type(&fc, f->proto->type));

    // dotted operators are named by the word between the dots
    const char* base = f->name[0] == '.' ? xstrn(f->name + 1, strlen(f->name) - 2) : f->name;
    Symbol* s = symbol_new(base, SP_ORDINARY);
    if (nimpl + nown) {
        Buf spec = {0};
        buf_puts(&spec, types_ident(ivals, nimpl));
        if (nimpl && nown) buf_putc(&spec, '_');
        buf_puts(&spec, types_ident(own, nown));
        symbol_context(s, buf_str(&spec));
    }
    if (im) {
        symbol_context(s, types_ident(&self, 1));
        if (im->trait) symbol_context(s, im->trait->name);
    }
    symbol_context_file(s, f->file);
    s->identity = xstrf("%s<%s>", s->identity, key);
    // a function without a body is a C function; `main` is the program entry
    if (!f->def || (!im && f->mod->is_root && strcmp(f->name, "main") == 0)) s->fixed = 1;
    fi->sym = s;

    map_put(&f->insts, key, fi);
    list_push(&g_insts, fi);
    return fi;
}

// Infer a call's generic arguments from its argument types and, for parameters
// the arguments leave open, from the type the context expects.
static Type** infer_generics(Ctx* c, Func* f, Type** ivals, Type* self, Val* args, int nargs, int offset,
                             Type* expected, Token at) {
    int n = generics_count(f->generics);
    Bind b = {0};
    b.n = n;
    b.params = (Type**)xalloc((size_t)n * sizeof(Type*));
    b.vals = (Type**)xalloc((size_t)n * sizeof(Type*));
    for (int i = 0; i < n; i++)
        b.params[i] = param_placeholder(f->file, f->generics->blockstmt[i]);
    Ctx pc = ctx_for_file(f->file);
    pc.tenv = tenv_bind(f->generics, b.params, f->impl ? tenv_bind(f->impl->decl->generics, ivals, NULL) : NULL);
    pc.self = self;
    Type* pattern = type_canon(resolve_type(&pc, f->proto->type));

    for (int i = 0; i < nargs && i + offset < pattern->nargs; i++) {
        Type* pt = pattern->args[i + offset];
        // parameters of array type are pointers
        if (pt->kind == TY_ARRAY) pt = type_ptr(pt->base);
        if (!type_has_param(pt) || type_is_unknown(args[i].t)) continue;
        Type* at_type = type_unq(type_decay(args[i].t));
        b.conflict = 0;
        if (unify(type_unq(pt), at_type, &b)) continue;
        if (b.conflict)
            FAIL(c, at, "conflicting %s for generic parameter '%s' of '%s': '%s' and '%s'; "
                 "inference requires exactly the same %s, so pass it explicitly as '%s<...>'",
                 b.params[b.conflict - 1]->is_const ? "values" : "types",
                 b.params[b.conflict - 1]->name, f->name, type_show(b.vals[b.conflict - 1]),
                 type_show(b.other), b.params[b.conflict - 1]->is_const ? "value" : "type", f->name);
        FAIL(c, at, "argument %d of '%s' has type '%s', which does not match the parameter type '%s'",
             i + 1, f->name, type_show(args[i].t), type_show(pt));
    }
    // return-type inference only fills what the arguments left undetermined
    if (expected && !type_is_unknown(expected)) {
        Bind probe = b;
        probe.vals = (Type**)xalloc((size_t)n * sizeof(Type*));
        memcpy(probe.vals, b.vals, (size_t)n * sizeof(Type*));
        if (unify(type_unq(pattern->base), type_unq(expected), &probe))
            for (int i = 0; i < n; i++)
                if (!b.vals[i]) b.vals[i] = probe.vals[i];
    }
    for (int i = 0; i < n; i++)
        if (!b.vals[i])
            FAIL(c, at, "cannot infer generic parameter '%s' of '%s'; pass it explicitly as '%s<...>'",
                 b.params[i]->name, f->name, f->name);
    return b.vals;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

#define val(s, t) (Val){s, t ? t : &g_unknown}

static Type* number_type(Token tok) {
    const char* s = tok.start;
    size_t n = tok.length;
    int hex = n > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    int floating = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '.' || (!hex && (ch == 'e' || ch == 'E')) || (hex && (ch == 'p' || ch == 'P')))
            floating = 1;
    }
    char last = n ? s[n - 1] : 0;
    if (floating) {
        if (last == 'f' || last == 'F') return type_builtin(BK_FLOAT);
        if (last == 'l' || last == 'L') return type_builtin(BK_LDOUBLE);
        return type_builtin(BK_DOUBLE);
    }
    int is_unsigned = 0, longs = 0;
    for (size_t i = n; i-- > 0 && strchr("uUlL", s[i]);) {
        if (s[i] == 'u' || s[i] == 'U') is_unsigned = 1;
        else longs++;
    }
    if (longs >= 2) return type_builtin(is_unsigned ? BK_ULLONG : BK_LLONG);
    if (longs == 1) return type_builtin(is_unsigned ? BK_ULONG : BK_LONG);
    return type_builtin(is_unsigned ? BK_UINT : BK_INT);
}

// The usual arithmetic conversions, closely enough to type sub-expressions
static Type* arith_type(Type* a, Type* b) {
    Type* ca = type_canon(a);
    Type* cb = type_canon(b);
    if (ca->kind == TY_ENUM) ca = type_builtin(BK_INT);
    if (cb->kind == TY_ENUM) cb = type_builtin(BK_INT);
    if (ca->kind != TY_BUILTIN || cb->kind != TY_BUILTIN) return &g_unknown;
    if (ca->builtin == BK_OPAQUE || cb->builtin == BK_OPAQUE) return &g_unknown;
    // two operands of one typedef'd type keep that name
    if (ca->builtin == cb->builtin && ca->builtin >= BK_INT) return type_unq(a);
    BuiltinKind ka = ca->builtin < BK_INT ? BK_INT : ca->builtin;
    BuiltinKind kb = cb->builtin < BK_INT ? BK_INT : cb->builtin;
    return type_builtin(ka > kb ? ka : kb);
}

static inline int type_is_pointerish(Type* t) {
    Type* c = type_canon(t);
    return c->kind == TY_PTR || c->kind == TY_ARRAY;
}

static const char* raw_text(Token tok) {
    // strip the delimiters and decode `}#!`, the escape for a literal `}#`
    Buf raw = {0};
    const char* s = tok.start + 2;
    const char* end = tok.start + tok.length - 2;
    while (s < end) {
        if (end - s >= 3 && s[0] == '}' && s[1] == '#' && s[2] == '!') {
            buf_puts(&raw, "}#");
            s += 3;
        } else {
            buf_putc(&raw, *s++);
        }
    }
    // surrounding blank space is layout, not content
    const char* text = buf_str(&raw);
    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1])) len--;
    size_t start = 0;
    while (start < len && isspace((unsigned char)text[start])) start++;
    // keep the indentation of the first line if the block spans several
    return xstrn(text + start, len - start);
}

static Val gen_enum_const(Ctx* c, Type* t, Token name) {
    Type* e = type_canon(t);
    if (e->kind != TY_ENUM)
        FAIL(c, name, "'%s' is not an enum type, so it has no member '%.*s'", type_show(t), (int)name.length, name.start);
    const char* key = tok_str(name);
    struct_resolve(e);
    Type* u = type_unq(e);
    for (int i = 0; i < u->nfields; i++)
        if (strcmp(u->fields[i].name, key) == 0)
            return val(u->fields[i].sym ? symbol_ref(u->fields[i].sym) : key, type_unq(t));
    FAIL(c, name, "enum '%s' has no constant '%s'", type_show(t), key);
}

static Type** resolve_func_args(Ctx* c, Func* f, Expr* targs, Token at) {
    int n = generics_count(f->generics);
    int given = generics_count(targs);
    if (n && !targs)
        FAIL(c, at, "generic function '%s' needs explicit type arguments when it is not called directly, as in '%s<...>'",
             f->name, f->name);
    if (given != n)
        FAIL(c, at, "'%s' takes %d type argument(s), %d given", f->name, n, given);
    Type** own = (Type**)xalloc((size_t)(n + 1) * sizeof(Type*));
    for (int i = 0; i < n; i++)
        own[i] = resolve_generic_arg(c, f->file, f->generics->blockstmt[i], targs->blockstmt[i]);
    return own;
}

static Val gen_entity_ref(Ctx* c, Entity* e, Expr* at) {
    switch (e->kind) {
        case ENT_FUNC: {
            if (!e->func) {
                if (at->generics) FAIL(c, at->token, "'%s' is a C function and takes no type arguments", e->name);
                return val(e->name, entity_type(e));
            }
            Type** own = resolve_func_args(c, e->func, at->generics, at->token);
            FuncInst* fi = func_inst(c, e->func, NULL, NULL, own, at->token);
            return val(symbol_ref(fi->sym), fi->type);
        }
        case ENT_VAR:
            if (at->generics) FAIL(c, at->token, "variable '%s' takes no type arguments", e->name);
            return val(e->sym ? symbol_ref(e->sym) : e->name, entity_type(e));
        case ENT_ENUMCONST:
            if (e->twin)
                FAIL(c, at->token, "enumerator '%s' belongs to more than one enum; qualify it, as in '%s::%s' or '%s::%s'",
                     e->name, e->owner, e->name, e->twin->owner, e->name);
            return val(e->sym ? symbol_ref(e->sym) : e->name, e->type);
        case ENT_MACRO:
            return val(e->name, entity_type(e));
        default:
            FAIL(c, at->token, "'%s' is a %s and cannot be used as a value", e->name, entity_kind_names[e->kind]);
    }
}

typedef struct {
    Entity* ent;        // namespace::symbol
    Type* enum_type;    // Enum::Constant, with the constant in `constant`
    Token constant;
} PathRef;

static PathRef resolve_path(Ctx* c, Expr* e) {
    size_t n = e->blockstmt_size;
    Token first_tok = e->blockstmt[0]->token;
    const char* first = tok_str(first_tok);
    PathRef r = {0};

    Module* m = local_find(c, L_VAR | L_TYPEDEF, first) ? NULL : import_find(c->file, first);
    if (m) {
        const char* name = tok_str(e->blockstmt[1]->token);
        Entity* ent = (Entity*)map_get(&m->names, name);
        if (n == 3 && (!ent || ent->kind != ENT_TYPEDEF)) {
            // namespace::Enum::Constant with the enum named by its tag
            Type* tag = (Type*)map_get(&m->tags, name);
            if (tag && tag->body && tag->kind == TY_ENUM) {
                if (m != c->mod && !tag->is_pub)
                    FAIL(c, e->blockstmt[1]->token, "'enum %s' is private to namespace '%s' (not marked 'pub')", name, m->name);
                r.enum_type = tag;
                r.constant = e->blockstmt[2]->token;
                return r;
            }
        }
        if (!ent)
            FAIL(c, e->blockstmt[1]->token, "namespace '%s' has no member '%s'", m->name, name);
        if (n == 2) {
            if (ent->kind == ENT_ENUMCONST)
                FAIL(c, e->blockstmt[1]->token, "enumerator '%s' is accessed through its enum type: '%s::<Enum>::%s'",
                     name, first, name);
            if (m != c->mod && !(ent->flags & F_PUB))
                FAIL(c, e->blockstmt[1]->token, "'%s' is private to namespace '%s' (not marked 'pub')", name, m->name);
            r.ent = ent;
            return r;
        }
        if (n == 3) {
            if (ent->kind != ENT_TYPEDEF)
                FAIL(c, e->blockstmt[1]->token, "'%s::%s' is not an enum type", first, name);
            if (m != c->mod && !(ent->flags & F_PUB))
                FAIL(c, e->blockstmt[1]->token, "'%s' is private to namespace '%s' (not marked 'pub')", name, m->name);
            r.enum_type = entity_type(ent);
            r.constant = e->blockstmt[2]->token;
            return r;
        }
        FAIL(c, e->token, "'%s::...' has too many segments", first);
    }

    if (n == 2) {
        // Enum::Constant, the enum named by typedef or by tag
        Type* t = NULL;
        Local* l = local_find(c, L_TYPEDEF | L_TAG, first);
        if (l) t = l->type;
        for (TEnv* te = c->tenv; te && !t; te = te->next)
            if (strcmp(te->name, first) == 0) t = te->type;
        if (!t) {
            Entity* ent = scope_entity(c, first);
            if (ent && ent->kind == ENT_TYPEDEF) t = entity_type(ent);
        }
        if (!t) t = (Type*)map_get(&c->mod->tags, first);
        if (!t && c->file->cunit) t = (Type*)map_get(&c->file->cunit->tags, first);
        if (t) {
            r.enum_type = t;
            r.constant = e->blockstmt[1]->token;
            return r;
        }
    }
    FAIL(c, first_tok, "'%s' is neither a namespace imported in this file nor an enum type", first);
}

static Val gen_ident(Ctx* c, Expr* e) {
    const char* name = tok_str(e->token);
    Local* l = local_find(c, L_VAR | L_ENUMCONST, name);
    if (l) {
        if (e->generics) FAIL(c, e->token, "'%s' takes no type arguments", name);
        return val(name, l->type);
    }
    Type* k = const_binding(c, name);
    if (k) {
        if (e->generics) FAIL(c, e->token, "'%s' takes no type arguments", name);
        if (k->kind == TY_PARAM) return val(name, k->base);
        // the value, with the parameter's declared type where that is not int
        Type* base = type_canon(k->base);
        if (base->kind == TY_BUILTIN && base->builtin == BK_INT && k->has_value) return val(const_text(k), k->base);
        return val(xstrf("(%s)%s", type_decl(k->base, NULL, NULL, 0), const_text(k)), k->base);
    }
    Entity* ent = scope_entity(c, name);
    if (ent) return gen_entity_ref(c, ent, e);

    if (strcmp(name, "__func__") == 0 || strcmp(name, "__FILE__") == 0 ||
        strcmp(name, "__DATE__") == 0 || strcmp(name, "__TIME__") == 0)
        return val(name, g_char_ptr);
    if (strcmp(name, "__LINE__") == 0) return val(name, type_builtin(BK_INT));
    // names Typed C cannot see: compiler builtins, and anything raw C declares
    if (strncmp(name, "__builtin_", 10) == 0 || c->mod->has_raw || c->mod->is_c || g_try_depth)
        return val(name, &g_unknown);
    if (local_find(c, L_TYPEDEF, name) || map_has(&c->mod->typenames, name))
        FAIL(c, e->token, "type name '%s' used as a value", name);
    FAIL(c, e->token, "undeclared identifier '%s'", name);
}

// An operand that becomes a function argument does not need its own parentheses
static Val gen_operand(Ctx* c, Expr* e, Type* expected) {
    int comma = e->kind == E_BINARY && token_is(e->token, ",");
    if (!(e->flags & F_PAREN) || comma) return gen_expr(c, e, expected);
    e->flags &= ~F_PAREN;
    Val v = gen_expr(c, e, expected);
    e->flags |= F_PAREN;
    return v;
}

#define MAX_ARGS 128

static int gen_reflection(Ctx* c, Expr* e, Type* expected, int initializer, Val* out);
static int reflection_set_indent(int indent);

static Val gen_call(Ctx* c, Expr* e, Type* expected) {
    Expr* callee = e->right;
    Val reflected;
    if (gen_reflection(c, e, expected, 0, &reflected)) return reflected;
    Expr* argv[MAX_ARGS];
    int argc = 0;
    for (Expr* a = e->left; a; a = a->right) {
        if (!a->left) continue;
        if (argc >= MAX_ARGS) FAIL(c, e->token, "too many arguments");
        argv[argc++] = a->left;
    }

    Func* f = NULL;
    Type** ivals = NULL;
    Type* self = NULL;
    Val recv = {0};
    int offset = 0;
    Token at = callee->token;
    int plain = !(callee->flags & F_PAREN);

    if (callee->kind == E_MEMBER && plain && token_is(callee->token, ".")) {
        // value.method(args): a method, unless the receiver has a member of
        // that name that can be called (C keeps its meaning) or no such method
        recv = gen_operand(c, callee->left, NULL);
        Type* rt = type_canon(recv.t);
        const char* name = tok_str(callee->name);
        at = callee->name;
        int is_member = 0;
        if (rt->kind == TY_STRUCT) {
            Field* field = struct_field(rt, name);
            is_member = field != NULL || type_unq(rt)->opaque;
            if (field && field->type) {
                Type* ft = type_canon(field->type);
                if (ft->kind == TY_PTR) ft = type_canon(ft->base);
                if (ft->kind != TY_FUNC && ft->kind != TY_UNKNOWN && has_method(type_unq(recv.t), name))
                    is_member = 0;
            }
        }
        if (rt->kind != TY_UNKNOWN && !is_member) {
            f = find_method(c, type_unq(recv.t), name, at, &ivals);
            self = impl_self(f->impl, ivals);
            offset = 1;
        }
    } else if (callee->kind == E_IDENT && callee->token.kind == TK_IDENT && plain) {
        const char* name = tok_str(callee->token);
        if (!local_find(c, L_VAR | L_ENUMCONST, name)) {
            Entity* ent = scope_entity(c, name);
            if (ent && ent->kind == ENT_FUNC) f = ent->func;
        }
    } else if (callee->kind == E_PATH && plain) {
        PathRef r = resolve_path(c, callee);
        if (r.ent && r.ent->kind == ENT_FUNC) f = r.ent->func;
    }

    Buf out = {0};
    if (!f) {
        // an ordinary C call
        Val cv = gen_expr(c, callee, NULL);
        Type* ft = type_canon(cv.t);
        if (ft->kind == TY_PTR) ft = type_canon(ft->base);
        if (ft->kind != TY_FUNC && ft->kind != TY_UNKNOWN)
            FAIL(c, at, "called object has type '%s', which is not a function", type_show(cv.t));
        buf_printf(&out, "%s(", cv.s);
        for (int i = 0; i < argc; i++) {
            Type* pt = ft->kind == TY_FUNC && i < ft->nargs ? ft->args[i] : NULL;
            buf_printf(&out, "%s%s", i ? ", " : "", gen_expr(c, argv[i], pt).s);
        }
        buf_putc(&out, ')');
        return val(buf_str(&out), ft->kind == TY_FUNC ? ft->base : &g_unknown);
    }

    int ngen = generics_count(f->generics);
    Expr* targs = callee->generics;
    Val args[MAX_ARGS];
    Type** own = NULL;
    FuncInst* fi = NULL;
    if (targs) own = resolve_func_args(c, f, targs, at);
    if (ngen == 0 || own) {
        // the signature is known: arguments are converted by C, and nested
        // generic calls can infer from the parameter types
        fi = func_inst(c, f, ivals, self, own, at);
        for (int i = 0; i < argc; i++)
            args[i] = gen_expr(c, argv[i], i + offset < fi->type->nargs ? fi->type->args[i + offset] : NULL);
    } else {
        for (int i = 0; i < argc; i++) args[i] = gen_expr(c, argv[i], NULL);
        own = infer_generics(c, f, ivals, self, args, argc, offset, expected, at);
        fi = func_inst(c, f, ivals, self, own, at);
    }

    int want = fi->type->nargs - offset;
    if (argc < want || (argc > want && !fi->type->variadic && (want || fi->type->voidparams || offset)))
        FAIL(c, at, "'%s' expects %d argument(s), %d given", f->name, want, argc);

    buf_printf(&out, "%s(", symbol_ref(fi->sym));
    if (offset) buf_puts(&out, recv.s);
    for (int i = 0; i < argc; i++)
        buf_printf(&out, "%s%s", i || offset ? ", " : "", args[i].s);
    buf_putc(&out, ')');
    return val(buf_str(&out), fi->type->base);
}

static Val gen_member(Ctx* c, Expr* e) {
    Val obj = gen_expr(c, e->left, NULL);
    const char* name = tok_str(e->name);
    int arrow = token_is(e->token, "->");
    const char* text = xstrf("%s%s%s", obj.s, arrow ? "->" : ".", name);

    Type* t = type_canon(obj.t);
    if (t->kind == TY_UNKNOWN) return val(text, &g_unknown);
    if (arrow) {
        if (t->kind != TY_PTR && t->kind != TY_ARRAY)
            FAIL(c, e->token, "'->' needs a pointer, but the operand has type '%s'", type_show(obj.t));
        int quals = t->base->quals;
        t = type_canon(t->base);
        if (t->kind == TY_UNKNOWN) return val(text, &g_unknown);
        (void)quals;
    } else if (t->kind == TY_PTR) {
        FAIL(c, e->token, "member access on a pointer of type '%s'; use '->' (methods are looked up on the exact receiver type)",
             type_show(obj.t));
    }
    if (t->kind == TY_STRUCT) {
        Field* f = struct_field(t, name);
        if (f) return val(text, f->type);
        if (type_unq(t)->opaque) return val(text, &g_unknown);
    }
    if (!arrow) {
        // is it a method used without a call?
        for (size_t i = 0; i < g_impls.len; i++) {
            Impl* im = (Impl*)g_impls.items[i];
            for (size_t j = 0; j < im->methods.len; j++)
                if (strcmp(((Func*)im->methods.items[j])->name, name) == 0)
                    FAIL(c, e->name, "'%s' is a method; method values are not supported, so it must be called", name);
        }
    }
    if (t->kind != TY_STRUCT)
        FAIL(c, e->name, "type '%s' has no members", type_show(obj.t));
    FAIL(c, e->name, "type '%s' has no member '%s'", type_show(obj.t), name);
}

static Val gen_binary(Ctx* c, Expr* e, Type* expected) {
    Token op = e->token;

    if (op.kind == TK_DOTOP) {
        // user-defined operator: a call to the method the trait declares
        Val l = gen_operand(c, e->left, NULL);
        const char* name = tok_str(op);
        if (type_is_unknown(l.t))
            FAIL(c, op, "cannot resolve operator '%s': the type of its left operand is not known to Typed C", name);
        Type** ivals = NULL;
        Func* f = find_method(c, type_unq(l.t), name, op, &ivals);
        Type* self = impl_self(f->impl, ivals);
        Type** own = NULL;
        Val r;
        FuncInst* fi;
        if (f->generics) {
            r = gen_operand(c, e->right, NULL);
            own = infer_generics(c, f, ivals, self, &r, 1, 1, expected, op);
            fi = func_inst(c, f, ivals, self, own, op);
        } else {
            fi = func_inst(c, f, ivals, self, NULL, op);
            r = gen_operand(c, e->right, fi->type->args[1]);
        }
        return val(xstrf("%s(%s, %s)", symbol_ref(fi->sym), l.s, r.s), fi->type->base);
    }

    static const char* const assign[] = {"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|="};
    for (size_t i = 0; i < sizeof(assign) / sizeof(assign[0]); i++) {
        if (!token_is(op, assign[i])) continue;
        Val l = gen_expr(c, e->left, NULL);
        Val r = e->right->kind == E_INIT_LIST ? gen_init(c, e->right, l.t)
                                              : gen_expr(c, e->right, i == 0 ? l.t : NULL);
        return val(xstrf("%s %s %s", l.s, assign[i], r.s), l.t);
    }

    Val l = gen_expr(c, e->left, NULL);
    Val r = gen_expr(c, e->right, token_is(op, ",") ? expected : NULL);
    char* ops = tok_str(op);
    if (token_is(op, ",")) return val(xstrf("%s, %s", l.s, r.s), r.t);

    const char* text = xstrf("%s %s %s", l.s, ops, r.s);
    if (strchr("=!<>", ops[0]) && (ops[1] == '=' || ops[1] == '\0') && strcmp(ops, "=") != 0)
        return val(text, type_builtin(BK_INT));     // == != < > <= >=
    if (strcmp(ops, "&&") == 0 || strcmp(ops, "||") == 0)
        return val(text, type_builtin(BK_INT));
    if (type_is_unknown(l.t) || type_is_unknown(r.t)) return val(text, &g_unknown);
    if (strcmp(ops, "<<") == 0 || strcmp(ops, ">>") == 0)
        return val(text, arith_type(l.t, type_builtin(BK_INT)));
    if (strcmp(ops, "+") == 0) {
        if (type_is_pointerish(l.t)) return val(text, type_decay(l.t));
        if (type_is_pointerish(r.t)) return val(text, type_decay(r.t));
    }
    if (strcmp(ops, "-") == 0 && type_is_pointerish(l.t))
        return val(text, type_is_pointerish(r.t) ? g_ptrdiff_t : type_decay(l.t));
    return val(text, arith_type(l.t, r.t));
}

static Val gen_unary(Ctx* c, Expr* e) {
    Token op = e->token;
    Val v = gen_expr(c, e->left, NULL);
    if (token_kw(op, "sizeof"))
        return val(xstrf(v.s[0] == '(' ? "sizeof%s" : "sizeof %s", v.s), g_size_t);

    char* ops = tok_str(op);
    // `- -x` must not turn into `--x`
    const char* text = xstrf(v.s[0] == ops[0] && strlen(ops) == 1 ? "%s %s" : "%s%s", ops, v.s);
    if (strcmp(ops, "!") == 0) return val(text, type_builtin(BK_INT));
    if (type_is_unknown(v.t)) return val(text, &g_unknown);
    if (strcmp(ops, "&") == 0) return val(text, type_ptr(v.t));
    if (strcmp(ops, "*") == 0) {
        Type* t = type_canon(type_decay(v.t));
        if (t->kind != TY_PTR)
            FAIL(c, op, "cannot dereference a value of type '%s'", type_show(v.t));
        return val(text, t->base);
    }
    if (strcmp(ops, "++") == 0 || strcmp(ops, "--") == 0) return val(text, v.t);
    return val(text, arith_type(v.t, v.t));
}

static Val gen_expr_inner(Ctx* c, Expr* e, Type* expected) {
    switch (e->kind) {
        case E_COND:
            return gen_expr(c, e->left, expected);
        case E_IDENT:
            switch (e->token.kind) {
                case TK_IDENT: return gen_ident(c, e);
                case TK_STR: {
                    char prefix = e->token.start[0];
                    return val(tok_str(e->token), prefix == '"' || prefix == 'u' ? g_char_ptr : &g_unknown);
                }
                case TK_CHAR: return val(tok_str(e->token), type_builtin(BK_INT));
                default: return val(tok_str(e->token), number_type(e->token));
            }
        case E_PATH: {
            PathRef r = resolve_path(c, e);
            if (r.ent) return gen_entity_ref(c, r.ent, e);
            if (e->generics) FAIL(c, e->token, "an enum constant takes no type arguments");
            return gen_enum_const(c, r.enum_type, r.constant);
        }
        case E_RAW:
            return val(raw_text(e->token), &g_unknown);
        case E_TYPEARG:
            return val(type_decl(resolve_type(c, e->type), NULL, NULL, 0), &g_unknown);
        case E_UNARY:
            return gen_unary(c, e);
        case E_POSTFIX: {
            Val v = gen_expr(c, e->left, NULL);
            return val(xstrf("%s%s", v.s, tok_str(e->token)), v.t);
        }
        case E_BINARY:
            return gen_binary(c, e, expected);
        case E_TERNARY: {
            Val cond = gen_expr(c, e->cases[1].condition, NULL);
            Val a = gen_expr(c, e->cases[1].block, expected);
            Val b = gen_expr(c, e->cases[0].block, expected);
            const char* text = xstrf("%s ? %s : %s", cond.s, a.s, b.s);
            Type* arith = arith_type(a.t, b.t);
            if (arith->kind != TY_UNKNOWN) return val(text, arith);
            return val(text, type_is_unknown(a.t) ? b.t : type_decay(a.t));
        }
        case E_CALL:
            return gen_call(c, e, expected);
        case E_INDEX: {
            Val base = gen_expr(c, e->right, NULL);
            Val index = gen_expr(c, e->left, NULL);
            const char* text = xstrf("%s[%s]", base.s, index.s);
            Type* t = type_canon(base.t);
            if (t->kind != TY_PTR && t->kind != TY_ARRAY) t = type_canon(index.t);
            if (t->kind == TY_PTR || t->kind == TY_ARRAY) return val(text, t->base);
            if (type_is_unknown(base.t) || type_is_unknown(index.t)) return val(text, &g_unknown);
            FAIL(c, e->token, "cannot index a value of type '%s'", type_show(base.t));
        }
        case E_MEMBER:
            return gen_member(c, e);
        case E_CAST: {
            Type* t = resolve_type(c, e->type);
            Val v = gen_expr(c, e->left, NULL);
            return val(xstrf("(%s)%s", type_decl(t, NULL, NULL, 0), v.s), t);
        }
        case E_SIZEOF_TYPE: {
            Type* t = resolve_type(c, e->type);
            return val(xstrf("%s(%s)", tok_str(e->token), type_decl(t, NULL, NULL, 0)), g_size_t);
        }
        case E_COMPOUND: {
            Type* t = resolve_type(c, e->type);
            Type* define = spec_defines_type(type_syntax_root(e->type)) ? type_unq(type_spec_root(t)) : NULL;
            Val init = gen_init(c, e->left, t);
            return val(xstrf("(%s)%s", type_decl(t, NULL, define, 0), init.s), t);
        }
        case E_INIT_LIST:
            return gen_init(c, e, expected);
        default:
            FAIL(c, e->token, "expected an expression");
    }
}

static Val gen_expr(Ctx* c, Expr* e, Type* expected) {
    Val v = gen_expr_inner(c, e, expected);
    if (e->flags & F_PAREN) v.s = xstrf("(%s)", v.s);
    return v;
}

// An initializer. Braced lists pass the member / element types down so nested
// generic calls can infer from them.
static Val gen_init(Ctx* c, Expr* e, Type* expected) {
    Val reflected;
    if (gen_reflection(c, e, expected, 1, &reflected)) return reflected;
    if (e->kind != E_INIT_LIST) return gen_expr(c, e, expected);

    Type* t = expected ? type_canon(expected) : &g_unknown;
    Type* st = t->kind == TY_STRUCT ? type_unq(t) : NULL;
    if (st) struct_resolve(st);
    Type* elem = t->kind == TY_ARRAY ? t->base : NULL;
    int position = 0;

    Buf out = {0};
    buf_putc(&out, '{');
    for (size_t i = 0; i < e->blockstmt_size; i++) {
        Expr* item = e->blockstmt[i];
        if (i) buf_puts(&out, ", ");
        Type* want = elem;
        if (item->kind == E_INIT_ITEM) {
            Expr* d = item->left;
            if (st && d->name.length && !d->right) {
                const char* name = tok_str(d->name);
                for (int k = 0; k < st->nfields; k++)
                    if (st->fields[k].name && strcmp(st->fields[k].name, name) == 0) {
                        want = st->fields[k].type;
                        position = k + 1;
                    }
            } else if (d->right) {
                want = NULL;    // nested designators: leave the checking to C
            }
            for (; d; d = d->right) {
                if (d->name.length) buf_printf(&out, ".%s", tok_str(d->name));
                else buf_printf(&out, "[%s]", gen_expr(c, d->left, NULL).s);
            }
            buf_printf(&out, " = %s", gen_init(c, item->right, want).s);
        } else {
            if (st) {
                while (position < st->nfields && (st->fields[position].verbatim || !st->fields[position].name))
                    position++;
                want = position < st->nfields ? st->fields[position].type : NULL;
                position++;
            }
            buf_puts(&out, gen_init(c, item, want).s);
        }
    }
    buf_putc(&out, '}');
    return val(buf_str(&out), expected);
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

static const char* storage_text(int flags) {
    Buf b = {0};
    if (flags & F_TYPEDEF) buf_puts(&b, "typedef ");
    if (flags & F_EXTERN) buf_puts(&b, "extern ");
    if (flags & F_STATIC) buf_puts(&b, "static ");
    if (flags & F_THREAD) buf_puts(&b, "_Thread_local ");
    if (flags & F_AUTO) buf_puts(&b, "auto ");
    if (flags & F_REGISTER) buf_puts(&b, "register ");
    if (flags & F_INLINE) buf_puts(&b, "inline ");
    if (flags & F_NORETURN) buf_puts(&b, "_Noreturn ");
    return buf_str(&b);
}

static const char* alignas_text(Ctx* c, Expr* spec) {
    Buf b = {0};
    if (spec && spec->kind != E_TY_ARRAY)
        for (Expr* a = spec->right; a && a->kind == E_ALIGNAS; a = a->right)
            buf_printf(&b, "_Alignas(%s) ", a->type ? type_decl(resolve_type(c, a->type), NULL, NULL, 0)
                                                   : gen_expr(c, a->left, NULL).s);
    return buf_str(&b);
}

// A block-scope declaration, without its terminating ';'
static void gen_local_decl(Ctx* c, Expr* g, Buf* out, int ind) {
    int is_typedef = (g->flags & F_TYPEDEF) != 0;
    buf_puts(out, storage_text(g->flags & F_STORAGE));
    buf_puts(out, alignas_text(c, g->type));

    if (!g->blockstmt_size) {
        // only declares a struct / union / enum
        Type* t = resolve_type(c, g->type);
        type_spec_into(out, t, type_unq(t), ind);
        return;
    }

    int defines = spec_defines_type(g->type);
    int single = g->blockstmt_size == 1;
    for (size_t i = 0; i < g->blockstmt_size; i++) {
        Expr* d = g->blockstmt[i];
        const char* name = tok_str(d->name);
        Type* t = resolve_type(c, d->type);
        Type* define = defines && i == 0 ? type_unq(type_spec_root(t)) : NULL;

        if (is_typedef) {
            Type* alias = type_new(TY_ALIAS);
            alias->name = name;
            alias->base = t;
            Type* u = type_unq(t);
            if ((u->kind == TY_STRUCT || u->kind == TY_ENUM) && !u->name && !u->cname) u->cname = name;
            local_push(c, L_TYPEDEF, name, alias);
        }
        Val init = {0};
        if (d->left) {
            int outer = reflection_set_indent(ind);
            init = gen_init(c, d->left, t);
            reflection_set_indent(outer);
        }
        if (!is_typedef) local_push(c, L_VAR, name, t);

        if (single) {
            buf_puts(out, type_decl(t, name, define, ind));
        } else {
            // declarators share one specifier, as written
            Type* leaf = NULL;
            const char* declarator = decl_build(t, name, &leaf);
            if (i == 0) type_spec_into(out, leaf, define, ind);
            buf_printf(out, "%s%s", i ? ", " : " ", declarator);
        }
        if (d->left) buf_printf(out, " = %s", init.s);
    }
}

static void gen_block(Ctx* c, Expr* block, Buf* out, int ind) {
    Local* scope = c->locals;
    buf_puts(out, "{\n");
    for (size_t i = 0; i < block->blockstmt_size; i++) {
        Expr* item = block->blockstmt[i];
        put_comments(out, item->lead, item->lead_len, ind + 1, i > 0, 1, c->quiet);
        // an error ends the statement it is in; the following ones are still checked
        jmp_buf failed;
        jmp_buf* recover = g_recover;
        Local* before = c->locals;
        if (g_try_depth == 0) {
            g_recover = &failed;
            if (setjmp(failed) != 0) {
                c->locals = before;
                g_resolving_type = NULL;
                g_recover = recover;
                // what a failed declaration declares is still a name, of unknown
                // type, or every later use would be reported as undeclared
                if (item->kind == E_DECLGROUP)
                    for (size_t k = 0; k < item->blockstmt_size; k++)
                        if (item->blockstmt[k]->name.length)
                            local_push(c, item->flags & F_TYPEDEF ? L_TYPEDEF : L_VAR,
                                       tok_str(item->blockstmt[k]->name), &g_unknown);
                continue;
            }
        }
        buf_puts(out, line_at(c->file, item->token));
        gen_stmt(c, item, out, ind + 1);
        g_recover = recover;
        put_trail(out, item->trail, item->trail_len, c->quiet);
    }
    put_comments(out, block->inner, block->inner_len, ind + 1, block->blockstmt_size > 0, 0, c->quiet);
    buf_indent(out, ind);
    buf_putc(out, '}');
    c->locals = scope;
}

// The statement controlled by if / for / while / ...: a block continues the
// line, anything else goes on its own. Returns whether it was a block.
static int gen_body(Ctx* c, Expr* s, Buf* out, int ind) {
    if (s->kind == E_BLOCK) {
        buf_putc(out, ' ');
        gen_block(c, s, out, ind);
        return 1;
    }
    buf_putc(out, '\n');
    Local* scope = c->locals;
    put_comments(out, s->lead, s->lead_len, ind + 1, 0, 0, c->quiet);
    buf_puts(out, line_at(c->file, s->token));
    gen_stmt(c, s, out, ind + 1);
    put_trail(out, s->trail, s->trail_len, c->quiet);
    c->locals = scope;
    return 0;
}

static void gen_if(Ctx* c, Expr* e, Buf* out, int ind) {
    int has_else = e->case_size == 2;
    Condition* branch = &e->cases[has_else ? 1 : 0];
    buf_printf(out, "if (%s)", gen_expr(c, branch->condition, NULL).s);
    int block = gen_body(c, branch->block, out, ind);
    if (!has_else) {
        if (block) buf_putc(out, '\n');
        return;
    }
    if (block) buf_puts(out, " else");
    else { buf_indent(out, ind); buf_puts(out, "else"); }
    Expr* other = e->cases[0].block;
    if (other->kind == E_IF) {
        buf_putc(out, ' ');
        gen_if(c, other, out, ind);
    } else if (gen_body(c, other, out, ind)) {
        buf_putc(out, '\n');
    }
}

static void gen_raw_lines(Buf* out, Token tok, int ind) {
    const char* text = raw_text(tok);
    // preprocessor lines must start their line; everything else is indented once
    if (text[0] != '#') buf_indent(out, ind);
    buf_printf(out, "%s\n", text);
}

static void gen_stmt(Ctx* c, Expr* e, Buf* out, int ind) {
    switch (e->kind) {
        case E_BLOCK:
            buf_indent(out, ind);
            gen_block(c, e, out, ind);
            buf_putc(out, '\n');
            break;
        case E_IF:
            buf_indent(out, ind);
            gen_if(c, e, out, ind);
            break;
        case E_WHILE:
            buf_indent(out, ind);
            buf_printf(out, "while (%s)", gen_expr(c, e->cases[0].condition, NULL).s);
            if (gen_body(c, e->cases[0].block, out, ind)) buf_putc(out, '\n');
            break;
        case E_DOWHILE: {
            buf_indent(out, ind);
            buf_puts(out, "do");
            if (gen_body(c, e->cases[0].block, out, ind)) buf_putc(out, ' ');
            else buf_indent(out, ind);
            buf_printf(out, "while (%s);\n", gen_expr(c, e->cases[0].condition, NULL).s);
            break;
        }
        case E_FOR: {
            Local* scope = c->locals;
            Condition* f = &e->cases[0];
            buf_indent(out, ind);
            buf_puts(out, "for (");
            if (f->base_case) {
                if (f->base_case->kind == E_DECLGROUP) gen_local_decl(c, f->base_case, out, ind);
                else buf_puts(out, gen_expr(c, f->base_case, NULL).s);
            }
            buf_puts(out, ";");
            if (f->condition) buf_printf(out, " %s", gen_expr(c, f->condition, NULL).s);
            buf_puts(out, ";");
            if (f->increment) buf_printf(out, " %s", gen_expr(c, f->increment, NULL).s);
            buf_puts(out, ")");
            if (gen_body(c, f->block, out, ind)) buf_putc(out, '\n');
            c->locals = scope;
            break;
        }
        case E_SWITCH:
            buf_indent(out, ind);
            buf_printf(out, "switch (%s)", gen_expr(c, e->cases[0].condition, NULL).s);
            if (gen_body(c, e->cases[0].block, out, ind)) buf_putc(out, '\n');
            break;
        case E_CASE:
            // labels sit one level out from the statements they mark
            buf_indent(out, ind > 0 ? ind - 1 : 0);
            buf_printf(out, "case %s:\n", gen_expr(c, e->left, NULL).s);
            put_comments(out, e->right->lead, e->right->lead_len, ind, 0, 0, c->quiet);
            gen_stmt(c, e->right, out, ind);
            break;
        case E_DEFAULT:
            buf_indent(out, ind > 0 ? ind - 1 : 0);
            buf_puts(out, "default:\n");
            put_comments(out, e->right->lead, e->right->lead_len, ind, 0, 0, c->quiet);
            gen_stmt(c, e->right, out, ind);
            break;
        case E_LABEL:
            buf_indent(out, ind > 0 ? ind - 1 : 0);
            buf_printf(out, "%s:\n", tok_str(e->name));
            put_comments(out, e->right->lead, e->right->lead_len, ind, 0, 0, c->quiet);
            gen_stmt(c, e->right, out, ind);
            break;
        case E_GOTO:
            buf_indent(out, ind);
            buf_printf(out, "goto %s;\n", tok_str(e->left->token));
            break;
        case E_RETURN:
            buf_indent(out, ind);
            if (e->left) {
                Val v = e->left->kind == E_INIT_LIST ? gen_init(c, e->left, c->ret) : gen_expr(c, e->left, c->ret);
                buf_printf(out, "return %s;\n", v.s);
            } else {
                buf_puts(out, "return;\n");
            }
            break;
        case E_BREAK:
            buf_indent(out, ind);
            buf_puts(out, "break;\n");
            break;
        case E_CONTINUE:
            buf_indent(out, ind);
            buf_puts(out, "continue;\n");
            break;
        case E_ALL:
            buf_indent(out, ind);
            buf_puts(out, ";\n");
            break;
        case E_RAW:
            gen_raw_lines(out, e->token, ind);
            break;
        case E_INCLUDE:
            // Not followed by header discovery (only file-scope includes are):
            // what it declares is left for the C compiler to check.
            c->mod->has_raw = 1;
            buf_printf(out, "%s\n", tok_str(e->token));
            break;
        case E_STATIC_ASSERT:
            buf_indent(out, ind);
            buf_printf(out, "_Static_assert(%s, %s);\n", gen_expr(c, e->left, NULL).s, tok_str(e->right->token));
            break;
        case E_DECLGROUP:
            buf_indent(out, ind);
            gen_local_decl(c, e, out, ind);
            buf_puts(out, ";\n");
            break;
        default:
            buf_indent(out, ind);
            buf_printf(out, "%s;\n", gen_expr(c, e, NULL).s);
            break;
    }
}

// ---------------------------------------------------------------------------
// Constant evaluation (for the identity of constant generic arguments)
// ---------------------------------------------------------------------------

// The value of one enumerator of an enum written in Typed C
static int enum_value(Type* t, const char* name, long long* out) {
    Type* u = type_unq(t);
    if (!u->body || u->from_c) return 0;
    Ctx c = ctx_for_file(u->file);
    c.tenv = u->tenv;
    long long current = -1;
    for (size_t i = 0; i < u->body->blockstmt_size; i++) {
        Expr* item = u->body->blockstmt[i];
        if (item->kind != E_ENUMERATOR) continue;
        if (token_is(item->name, name) && !item->left) {
            *out = current + 1;
            return 1;
        }
        // an enumerator's value may use the ones before it, never itself
        if (item->left) {
            if (!const_eval(&c, item->left, &current)) return 0;
        } else {
            current++;
        }
        if (token_is(item->name, name)) {
            *out = current;
            return 1;
        }
    }
    return 0;
}

// Evaluate an integer constant expression. Returns 0 for anything typedc does
// not model (macros, sizeof, header constants, floating point).
static int const_eval(Ctx* c, Expr* e, long long* out) {
    long long a, b;
    switch (e->kind) {
        case E_IDENT: {
            Token tok = e->token;
            if (tok.kind == TK_PP_NUM) {
                if (number_type(tok)->builtin >= BK_FLOAT) return 0;
                char* end = NULL;
                char* text = tok_str(tok);
                unsigned long long v = strtoull(text, &end, 0);
                for (; *end; end++)
                    if (!strchr("uUlL", *end)) return 0;
                *out = (long long)v;
                return 1;
            }
            if (tok.kind == TK_CHAR) {
                if (tok.length == 3 && tok.start[0] == '\'') { *out = (unsigned char)tok.start[1]; return 1; }
                return 0;
            }
            if (tok.kind != TK_IDENT) return 0;
            const char* name = tok_str(tok);
            Type* k = const_binding(c, name);
            if (k) {
                *out = k->value;
                return k->kind == TY_CONST && k->has_value;
            }
            if (local_find(c, L_VAR | L_ENUMCONST, name)) return 0;
            Entity* ent = scope_entity(c, name);
            return ent && ent->kind == ENT_ENUMCONST && !ent->twin && enum_value(ent->type, name, out);
        }
        case E_PATH: {
            PathRef r = resolve_path(c, e);
            if (!r.enum_type || type_canon(r.enum_type)->kind != TY_ENUM) return 0;
            return enum_value(type_canon(r.enum_type), tok_str(r.constant), out);
        }
        case E_CALL:
            return reflection_constant(c, e, out);
        case E_UNARY: {
            if (e->token.kind != TK_PUNCT || !const_eval(c, e->left, &a)) return 0;
            if (token_is(e->token, "-")) *out = -a;
            else if (token_is(e->token, "+")) *out = a;
            else if (token_is(e->token, "~")) *out = ~a;
            else if (token_is(e->token, "!")) *out = !a;
            else return 0;
            return 1;
        }
        case E_TERNARY:
            if (!const_eval(c, e->cases[1].condition, &a)) return 0;
            return const_eval(c, a ? e->cases[1].block : e->cases[0].block, out);
        case E_BINARY: {
            if (e->token.kind != TK_PUNCT) return 0;
            if (!const_eval(c, e->left, &a) || !const_eval(c, e->right, &b)) return 0;
            char* op = tok_str(e->token);
            if (!strcmp(op, "+")) *out = a + b;
            else if (!strcmp(op, "-")) *out = a - b;
            else if (!strcmp(op, "*")) *out = a * b;
            else if (!strcmp(op, "/")) { if (!b) return 0; *out = a / b; }
            else if (!strcmp(op, "%")) { if (!b) return 0; *out = a % b; }
            else if (!strcmp(op, "<<")) { if (b < 0 || b > 62) return 0; *out = a << b; }
            else if (!strcmp(op, ">>")) { if (b < 0 || b > 62) return 0; *out = a >> b; }
            else if (!strcmp(op, "&")) *out = a & b;
            else if (!strcmp(op, "|")) *out = a | b;
            else if (!strcmp(op, "^")) *out = a ^ b;
            else if (!strcmp(op, "==")) *out = a == b;
            else if (!strcmp(op, "!=")) *out = a != b;
            else if (!strcmp(op, "<")) *out = a < b;
            else if (!strcmp(op, ">")) *out = a > b;
            else if (!strcmp(op, "<=")) *out = a <= b;
            else if (!strcmp(op, ">=")) *out = a >= b;
            else if (!strcmp(op, "&&")) *out = a && b;
            else if (!strcmp(op, "||")) *out = a || b;
            else return 0;
            return 1;
        }
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Whole functions, globals and the checks that are not tied to a use
// ---------------------------------------------------------------------------

static void gen_function(FuncInst* fi) {
    Func* f = fi->fn;
    fi->generated = 1;
    if (!f->def) {
        if (f->generics)
            fatal(f->file->path, f->proto->name, "generic function '%s' is used but has no definition", f->name);
        return;
    }
    Ctx c = ctx_for_file(f->file);
    c.tenv = fi->tenv;
    c.self = fi->self;
    c.ret = fi->type->base;
    c.in_func = 1;
    c.quiet = !fi->first;
    List declared = {0};
    c.declared = &declared;
    for (int i = 0; i < fi->type->nargs; i++) {
        if (!fi->type->pnames[i]) continue;
        Type* pt = fi->type->args[i];
        Type* canon = type_canon(pt);
        // array and function parameters are pointers inside the body
        if (canon->kind == TY_ARRAY) pt = type_ptr(canon->base);
        else if (canon->kind == TY_FUNC) pt = type_ptr(pt);
        local_push(&c, L_VAR, fi->type->pnames[i], pt);
    }
    Buf body = {0};
    FuncInst* outer = g_generating;
    g_generating = fi;
    gen_block(&c, f->def->right, &body, 0);
    g_generating = outer;
    fi->body = buf_str(&body);

    // In C a local hides a function, variable or type of the same name. Typed C
    // resolved these references already (`io::reader`, a method, ...), so the
    // symbols this body uses must not end up with the name of one of its locals.
    for (const char* s = fi->body; *s; s++) {
        if (*s != '\x01') continue;
        Symbol* used = (Symbol*)g_symbols.items[strtol(s + 1, (char**)&s, 10)];
        if (used->space != SP_ORDINARY) continue;
        for (size_t i = 0; i < declared.len; i++)
            map_put(&used->avoid, (const char*)declared.items[i], used);
    }
}

static int syntax_mentions(Expr* s, Token name) {
    if (!s) return 0;
    switch (s->kind) {
        case E_TY_NAME:
            if (s->blockstmt_size == 1 && token_same(s->blockstmt[0]->token, name)) return 1;
            return syntax_mentions(s->left, name);
        case E_LIST:
            for (size_t i = 0; i < s->blockstmt_size; i++)
                if (syntax_mentions(s->blockstmt[i], name)) return 1;
            return 0;
        case E_TY_ARRAY:
            return syntax_mentions(s->left, name) || syntax_mentions(s->right, name);
        case E_TY_PTR: case E_TY_HOLE:
            return syntax_mentions(s->left, name);
        // constant arguments and array sizes are expressions
        case E_IDENT:
            return s->token.kind == TK_IDENT && token_same(s->token, name);
        case E_BINARY: case E_UNARY: case E_CAST:
            return syntax_mentions(s->left, name) || syntax_mentions(s->right, name);
        case E_TY_FUNC:
            if (syntax_mentions(s->left, name)) return 1;
            for (size_t i = 0; i < s->blockstmt_size; i++)
                if (syntax_mentions(s->blockstmt[i]->type, name)) return 1;
            return 0;
        default: return 0;
    }
}

// Every type parameter must appear in the signature it belongs to. A constant
// parameter need not: it is a value the body can use, as in
// `double activate<Activation A>(double x)`, and a call then passes it
// explicitly, `activate<Activation::Tanh>(x)`.
static void check_generics_used(Func* f) {
    for (int i = 0; i < generics_count(f->generics); i++) {
        Token name = f->generics->blockstmt[i]->name;
        if (f->generics->blockstmt[i]->type) {
            const_param_type(f->file, f->generics->blockstmt[i]);
            continue;
        }
        if (!syntax_mentions(f->proto->type, name))
            fatal(f->file->path, name, "generic parameter '%.*s' is not used in the signature of '%s'",
                  (int)name.length, name.start, f->name);
    }
}

// A trait implementation defines exactly the trait's methods, with the trait's signatures
static void check_impl(Impl* im) {
    impl_prepare(im);
    Ctx c = ctx_for_file(im->file);
    for (size_t i = 0; i < im->methods.len; i++)
        check_generics_used((Func*)im->methods.items[i]);
    if (!im->trait) return;

    Expr* td = im->trait->decl;
    for (size_t i = 0; i < td->blockstmt_size; i++) {
        Expr* proto = td->blockstmt[i];
        Func* method = NULL;
        for (size_t j = 0; j < im->methods.len; j++)
            if (token_is(proto->name, ((Func*)im->methods.items[j])->name))
                method = (Func*)im->methods.items[j];
        if (!method)
            FAIL(&c, im->decl->token, "this implementation of trait '%s' for '%s' is missing '%.*s'",
                 im->trait->name, type_show(im->target), (int)proto->name.length, proto->name.start);
        if (generics_count(proto->generics) != generics_count(method->generics))
            FAIL(&c, method->def->name, "'%s' declares %d generic parameter(s), but trait '%s' declares %d",
                 method->name, generics_count(method->generics), im->trait->name, generics_count(proto->generics));

        // compare signatures with Self and the generics of both sides lined up
        int n = generics_count(proto->generics);
        Type** shared = (Type**)xalloc((size_t)(n + 1) * sizeof(Type*));
        for (int k = 0; k < n; k++)
            shared[k] = param_placeholder(im->trait->file, proto->generics->blockstmt[k]);
        Ctx tc = ctx_for_file(im->trait->file);
        tc.tenv = tenv_bind(proto->generics, shared, tenv_bind(td->generics, im->trait_args, NULL));
        tc.self = im->target;
        Ctx ic = ctx_for_file(im->file);
        ic.tenv = tenv_bind(method->generics, shared, im->tenv);
        ic.self = im->target;
        Type* want = resolve_type(&tc, proto->type);
        Type* have = resolve_type(&ic, method->def->type);
        if (!type_eq(want, have, 0))
            FAIL(&c, method->def->name, "'%s' has type '%s', but trait '%s' declares it as '%s'",
                 method->name, type_show(have), im->trait->name, type_show(want));
    }
    for (size_t j = 0; j < im->methods.len; j++) {
        Func* method = (Func*)im->methods.items[j];
        int declared = 0;
        for (size_t i = 0; i < td->blockstmt_size; i++)
            if (token_is(td->blockstmt[i]->name, method->name)) declared = 1;
        if (!declared)
            FAIL(&c, method->def->name, "'%s' is not a member of trait '%s'", method->name, im->trait->name);
    }
}

// Two implementations of one trait that could apply to the same type are
// rejected up front when they live in the same namespace.
static void check_overlaps(void) {
    for (size_t i = 0; i < g_impls.len; i++) {
        Impl* a = (Impl*)g_impls.items[i];
        if (!a->trait) continue;
        for (size_t j = 0; j < i; j++) {
            Impl* b = (Impl*)g_impls.items[j];
            if (b->trait != a->trait || b->mod != a->mod) continue;
            int overlap = types_overlap(a->target, b->target);
            for (int k = 0; k < a->ntrait_args && overlap; k++)
                overlap = types_overlap(a->trait_args[k], b->trait_args[k]);
            if (overlap)
                RECOVER(fatal(a->file->path, a->decl->token, "this implementation of trait '%s' for '%s' overlaps the one for '%s' at %s:%zu",
                              a->trait->name, type_show(a->target), type_show(b->target), b->file->path, b->decl->token.row));
        }
    }
}

// Generate everything queued so far, including what that generation requests
static void drain_functions(void) {
    for (size_t i = 0; i < g_insts.len; i++) {
        FuncInst* fi = (FuncInst*)g_insts.items[i];
        if (!fi->generated) RECOVER(gen_function(fi));
        g_generating = NULL;
        g_resolving_type = NULL;
    }
}

// One file-scope declaration
static void check_entity(Entity* e) {
    Ctx c = ctx_for_file(e->file);
    switch (e->kind) {
        case ENT_FUNC:
            check_generics_used(e->func);
            // generic functions are generated per use; all others always
            if (!e->func->generics) func_inst(&c, e->func, NULL, NULL, NULL, e->tok);
            break;
        case ENT_VAR:
            entity_type(e);
            if (e->decl->left) e->init_text = gen_init(&c, e->decl->left, e->type).s;
            break;
        case ENT_TYPEDEF:
            entity_type(e);
            break;
        case ENT_GTYPEDEF:
            // the body is checked per instance; the parameter list now
            for (int k = 0; k < generics_count(e->decl->generics); k++)
                if (e->decl->generics->blockstmt[k]->type)
                    const_param_type(e->file, e->decl->generics->blockstmt[k]);
            break;
        case ENT_TRAIT: {
            // prototypes are validated against each implementation; here
            // only the bounds need to name traits
            Expr* g = e->decl->generics;
            for (int k = 0; k < generics_count(g); k++)
                for (size_t j = 0; j < g->blockstmt[k]->blockstmt_size; j++) {
                    Entity* tr = lookup_type_entity(&c, g->blockstmt[k]->blockstmt[j]);
                    if (!tr || tr->kind != ENT_TRAIT)
                        FAIL(&c, g->blockstmt[k]->blockstmt[j]->token, "a bound must name a trait");
                }
            break;
        }
        default: break;
    }
}

// Check and lower every namespace that was loaded
static void compile_program(void) {
    for (size_t i = 0; i < g_impls.len; i++) RECOVER(check_impl((Impl*)g_impls.items[i]));
    check_overlaps();

    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->names.len; i++) {
            RECOVER(check_entity((Entity*)mod->names.vals[i]));
            g_resolving_type = NULL;
        }
    }
    // methods of non-generic implementations are part of the namespace too
    for (size_t i = 0; i < g_impls.len; i++) {
        Impl* im = (Impl*)g_impls.items[i];
        if (im->nparams) continue;
        Ctx c = ctx_for_file(im->file);
        for (size_t j = 0; j < im->methods.len; j++) {
            Func* method = (Func*)im->methods.items[j];
            if (!method->generics)
                RECOVER(func_inst(&c, method, NULL, impl_self(im, NULL), NULL, method->def->name));
        }
    }
    drain_functions();
}
