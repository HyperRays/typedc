#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "types.h"

// Loading: source files, directory namespaces, C header discovery, and the
// pass that records what every file declares.

#define SOURCE_EXTENSION ".tc"

typedef struct {
    const char* cc;         // C compiler used to preprocess headers
    List cpp_args;          // -I / -D / ... passed through to it
    const char* root;       // directory of the root namespace
    int lines;              // write #line directives pointing back at the source
} Options;

static Options g_opt;
static Map g_module_by_name;
static Map g_cunits;
static Map g_generic_methods;   // method names declared with generic parameters, program-wide

typedef struct {
    int left_bp;
    int right_bp;
    Token tok;
    SrcFile* file;
} DotOp;

// ---------------------------------------------------------------------------
// Files and paths
// ---------------------------------------------------------------------------

static inline char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        buf_putn(&b, chunk, n);
    fclose(f);
    buf_reserve(&b, 1);
    b.data[b.len] = '\0';
    return b.data;
}

static inline const char* path_dir(const char* path) {
    const char* slash = strrchr(path, '/');
    if (!slash) return ".";
    if (slash == path) return "/";
    return xstrn(path, (size_t)(slash - path));
}

static inline const char* path_stem(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    const char* dot = strrchr(name, '.');
    return xstrn(name, dot && dot != name ? (size_t)(dot - name) : strlen(name));
}

static inline int is_directory(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static inline const char* shell_quote(const char* s) {
    Buf b = {0};
    buf_putc(&b, '\'');
    for (; *s; s++) {
        if (*s == '\'') buf_puts(&b, "'\\''");
        else buf_putc(&b, *s);
    }
    buf_putc(&b, '\'');
    return buf_str(&b);
}

static inline const char* token_name(Token tok, char* buffer, size_t size) {
    size_t n = tok.length < size - 1 ? tok.length : size - 1;
    memcpy(buffer, tok.start, n);
    buffer[n] = '\0';
    return buffer;
}

static inline SrcFile* file_new(Module* m, const char* path) {
    SrcFile* f = (SrcFile*)xalloc(sizeof(SrcFile));
    f->path = path;
    f->stem = path_stem(path);
    f->mod = m;
    Buf label = {0};
    for (size_t i = 0; i < m->segments.len; i++)
        buf_printf(&label, "%s_", (const char*)m->segments.items[i]);
    buf_puts(&label, f->stem);
    f->label = buf_str(&label);
    list_push(&m->files, f);
    return f;
}

static inline Module* import_find(SrcFile* f, const char* name) {
    for (int i = 0; i < f->nimports; i++)
        if (strcmp(f->imports[i].name, name) == 0) return f->imports[i].mod;
    return NULL;
}

// ---------------------------------------------------------------------------
// What the parser needs to know about names declared elsewhere
// ---------------------------------------------------------------------------

static inline int is_builtin_type_name(const char* name) {
    static const char* const names[] = {
        "__builtin_va_list", "__int128_t", "__uint128_t", "__float128", "__float80",
        "_Float16", "_Float32", "_Float64", "_Float128", "_Float32x", "_Float64x"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

static int env_lookup_type(void* env, const Token* segments, int count) {
    SrcFile* f = (SrcFile*)env;
    char first[256], second[256];
    token_name(segments[0], first, sizeof first);
    if (count == 1) {
        if (map_has(&f->mod->typenames, first)) return 1;
        Module* c = f->mod->is_c ? f->mod : f->cunit;
        if (c) {
            Entity* e = (Entity*)map_get(&c->names, first);
            if (e) return e->kind == ENT_TYPEDEF;
        }
        return is_builtin_type_name(first);
    }
    if (count == 2) {
        Module* m = import_find(f, first);
        return m && map_has(&m->typenames, token_name(segments[1], second, sizeof second));
    }
    return 0;
}

static int env_lookup_dotop(void* env, Token name, int* left_bp, int* right_bp) {
    SrcFile* f = (SrcFile*)env;
    char buffer[256];
    DotOp* op = (DotOp*)map_get(&f->dotops, token_name(name, buffer, sizeof buffer));
    if (!op) return 0;
    *left_bp = op->left_bp;
    *right_bp = op->right_bp;
    return 1;
}

// Is the name before a `<` declared with generic parameters? Only then may the
// list hold constant arguments (see parser_try_type_args).
static int env_lookup_generic(void* env, const Token* segments, int count, int member) {
    SrcFile* f = (SrcFile*)env;
    char first[256], second[256];
    token_name(segments[0], first, sizeof first);
    if (member) return map_has(&g_generic_methods, first);
    if (count == 1) return map_has(&f->mod->generics, first);
    if (count == 2) {
        Module* m = import_find(f, first);
        return m && map_has(&m->generics, token_name(segments[1], second, sizeof second));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Namespaces
// ---------------------------------------------------------------------------

static void module_load(Module* m);

static inline Module* module_new(const char* name) {
    Module* m = (Module*)xalloc(sizeof(Module));
    m->name = name;
    return m;
}

static int compare_strings(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

// The namespace for an import path: every source file directly inside
// <root>/<a>/<b>/...
static inline Module* module_for_import(SrcFile* from, Expr* import) {
    Buf name = {0}, dir = {0};
    buf_puts(&dir, g_opt.root);
    for (size_t i = 0; i < import->blockstmt_size; i++) {
        char* seg = tok_str(import->blockstmt[i]->token);
        buf_printf(&name, "%s%s", i ? "::" : "", seg);
        buf_printf(&dir, "/%s", seg);
    }
    Module* m = (Module*)map_get(&g_module_by_name, buf_str(&name));
    if (m) return m;

    if (!is_directory(buf_str(&dir)))
        fatal(from->path, import->token, "cannot find namespace '%s': '%s' is not a directory",
              buf_str(&name), buf_str(&dir));
    m = module_new(buf_str(&name));
    m->dir = buf_str(&dir);
    for (size_t i = 0; i < import->blockstmt_size; i++)
        list_push(&m->segments, tok_str(import->blockstmt[i]->token));
    map_put(&g_module_by_name, m->name, m);

    List paths = {0};
    DIR* d = opendir(m->dir);
    if (d) {
        for (struct dirent* e; (e = readdir(d));) {
            size_t n = strlen(e->d_name), x = strlen(SOURCE_EXTENSION);
            if (n > x && strcmp(e->d_name + n - x, SOURCE_EXTENSION) == 0 && e->d_name[0] != '.')
                list_push(&paths, xstrf("%s/%s", m->dir, e->d_name));
        }
        closedir(d);
    }
    if (!paths.len)
        fatal(from->path, import->token, "namespace '%s' has no %s files in '%s'",
              m->name, SOURCE_EXTENSION, m->dir);
    // directory order is arbitrary; the output must not be
    qsort(paths.items, paths.len, sizeof(void*), compare_strings);
    for (size_t i = 0; i < paths.len; i++)
        file_new(m, (const char*)paths.items[i]);
    return m;
}

// First pass over a file: find what it imports, includes and declares as types,
// so that every file can then be parsed in full regardless of order.
static inline void file_prescan(SrcFile* f) {
    f->source = read_file(f->path);
    if (!f->source) fatal(f->path, (Token){0}, "cannot read file");

    Parser p = parser_new(f->source);
    p.filename = f->path;
    p.prescan = 1;
    Expr* ast = parse_translation_unit(&p);

    for (size_t i = 0; i < ast->blockstmt_size; i++) {
        Expr* d = ast->blockstmt[i];
        switch (d->kind) {
            case E_IMPORT: {
                Module* m = module_for_import(f, d);
                const char* name = tok_str(d->name);
                Module* bound = import_find(f, name);
                if (bound && bound != m)
                    fatal(f->path, d->name, "'%s' is already bound to namespace '%s'; choose a distinct alias with 'as'",
                          name, bound->name);
                if (!bound) {
                    f->imports = (Import*)realloc(f->imports, (size_t)(f->nimports + 1) * sizeof(Import));
                    f->imports[f->nimports++] = (Import){name, m, d->name};
                }
                module_load(m);
                break;
            }
            case E_INCLUDE:
                list_push(&f->includes, tok_str(d->token));
                break;
            case E_DECLGROUP:
                for (size_t j = 0; j < d->blockstmt_size; j++) {
                    if (d->flags & F_TYPEDEF)
                        map_put(&f->mod->typenames, tok_str(d->blockstmt[j]->name), f);
                    else if (d->blockstmt[j]->generics)
                        map_put(&f->mod->generics, tok_str(d->blockstmt[j]->name), f);
                }
                break;
            case E_FUNCDEF:
                if (d->generics) map_put(&f->mod->generics, tok_str(d->name), f);
                break;
            case E_IMPL:
                // the prescan leaves the names followed by `<` in the impl node
                for (size_t j = 0; j < d->blockstmt_size; j++)
                    map_put(&g_generic_methods, tok_str(d->blockstmt[j]->token), f);
                break;
            case E_GTYPEDEF:
                map_put(&f->mod->typenames, tok_str(d->name), f);
                break;
            case E_TRAIT:
                for (size_t j = 0; j < d->blockstmt_size; j++) {
                    Expr* member = d->blockstmt[j];
                    if (member->generics) map_put(&g_generic_methods, tok_str(member->name), f);
                    if (member->name.kind != TK_DOTOP) continue;
                    DotOp* op = (DotOp*)xalloc(sizeof(DotOp));
                    op->left_bp = member->aux ? (member->aux >> 15) & 0x7fff : DOTOP_LEFT_BP;
                    op->right_bp = member->aux ? member->aux & 0x7fff : DOTOP_RIGHT_BP;
                    op->tok = member->name;
                    op->file = f;
                    const char* name = tok_str(member->name);
                    DotOp* seen = (DotOp*)map_get(&f->mod->dotops, name);
                    if (seen && (seen->left_bp != op->left_bp || seen->right_bp != op->right_bp))
                        fatal(f->path, member->name, "operator '%s' is declared with binding powers (%d, %d) here but (%d, %d) at %s:%zu",
                              name, op->left_bp, op->right_bp, seen->left_bp, seen->right_bp,
                              seen->file->path, seen->tok.row);
                    if (!seen) map_put(&f->mod->dotops, name, op);
                }
                break;
            case E_RAW:
                f->mod->has_raw = 1;
                break;
            default: break;
        }
    }
}

static void module_load(Module* m) {
    if (m->loaded) return;
    m->loaded = 1;
    for (size_t i = 0; i < m->files.len; i++)
        file_prescan((SrcFile*)m->files.items[i]);
    list_push(&g_modules, m);
}

// Every declaration of a dotted operator visible in one file must agree on
// its binding powers: grouping is decided before types are known.
static inline void file_collect_dotops(SrcFile* f) {
    for (int i = -1; i < f->nimports; i++) {
        Module* m = i < 0 ? f->mod : f->imports[i].mod;
        for (size_t j = 0; j < m->dotops.len; j++) {
            DotOp* op = (DotOp*)m->dotops.vals[j];
            DotOp* seen = (DotOp*)map_get(&f->dotops, m->dotops.keys[j]);
            if (seen && (seen->left_bp != op->left_bp || seen->right_bp != op->right_bp))
                fatal(f->path, i < 0 ? op->tok : f->imports[i].tok, "operator '%s' is visible here with conflicting binding powers: (%d, %d) at %s:%zu and (%d, %d) at %s:%zu",
                      m->dotops.keys[j], seen->left_bp, seen->right_bp, seen->file->path, seen->tok.row,
                      op->left_bp, op->right_bp, op->file->path, op->tok.row);
            if (!seen) map_put(&f->dotops, m->dotops.keys[j], op);
        }
    }
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

static inline Entity* entity_new(int kind, SrcFile* f, Token tok, const char* name) {
    Entity* e = (Entity*)xalloc(sizeof(Entity));
    e->kind = kind;
    e->name = name;
    e->mod = f->mod;
    e->file = f;
    e->tok = tok;
    e->from_c = f->mod->is_c;
    e->prev_raw = f->last_raw;
    return e;
}

// A `static` function or variable has internal linkage in C: it belongs to
// its file, and other files of the namespace may reuse the name.
static inline int entity_is_static(Module* m, int kind, int flags) {
    return !m->is_c && (flags & F_STATIC) && (kind == ENT_FUNC || kind == ENT_VAR);
}

// What a file-scope name means in file `f`: its own statics come first
static inline Entity* file_entity(SrcFile* f, const char* name) {
    Entity* e = f->statics.len ? (Entity*)map_get(&f->statics, name) : NULL;
    return e ? e : (Entity*)map_get(&f->mod->names, name);
}

// Register a file-scope name. Headers routinely repeat declarations, so for C
// the first one wins; in Typed C a second declaration is an error.
//
// Two kinds of entity are also kept in the namespace's table under a key no
// identifier can spell, so that whatever walks the table still finds them:
// statics (looked up through their file) and enumerators that share their
// name with one of another enum (looked up through their enum).
static inline Entity* entity_add(Entity* e) {
    Module* m = e->mod;
    if (entity_is_static(m, e->kind, e->flags)) {
        Entity* seen = (Entity*)map_get(&e->file->statics, e->name);
        if (seen)
            fatal(e->file->path, e->tok, "redefinition of '%s' (already declared as a %s at %s:%zu)",
                  e->name, entity_kind_names[seen->kind], seen->file->path, seen->tok.row);
        map_put(&e->file->statics, e->name, e);
        map_put(&m->names, xstrf("%s\x01%s", e->name, e->file->path), e);
        return e;
    }
    Entity* seen = (Entity*)map_get(&m->names, e->name);
    if (seen) {
        if (m->is_c) return seen;
        // Enumerators belong to their enum (`Wire::Double`, `Physical::Double`),
        // so two enums may use one name; only the unqualified name is ambiguous.
        if (e->kind == ENT_ENUMCONST && seen->kind == ENT_ENUMCONST && e->owner && seen->owner && e->type != seen->type) {
            e->twin = seen;
            if (!seen->twin) seen->twin = e;
            map_put(&m->names, xstrf("%s\x01%s", e->name, e->owner), e);
            return e;
        }
        fatal(e->file->path, e->tok, "redefinition of '%s' (already declared as a %s at %s:%zu)",
              e->name, entity_kind_names[seen->kind], seen->file->path, seen->tok.row);
    }
    map_put(&m->names, e->name, e);
    return e;
}

static inline Symbol* entity_symbol(Entity* e) {
    Symbol* s = symbol_new(e->name, SP_ORDINARY);
    symbol_context_file(s, e->file);
    return s;
}

static inline Type* tag_type(SrcFile* f, const char* name, TypeKind kind, int is_union) {
    Module* m = f->mod;
    Type* t = (Type*)map_get(&m->tags, name);
    if (!t) {
        t = type_new(kind);
        t->name = name;
        t->is_union = is_union;
        t->from_c = m->is_c;
        t->file = f;
        if (!m->is_c) {
            t->sym = symbol_new(name, SP_TAG);
            symbol_context_file(t->sym, f);
            // a tag that Typed C never defines names a C type and keeps its name
            t->sym->fixed = 1;
        }
        map_put(&m->tags, name, t);
    }
    return t;
}

// The specifiers a declarator type was built on
static inline Expr* type_syntax_root(Expr* type) {
    while (type && (type->kind == E_TY_PTR || type->kind == E_TY_ARRAY ||
                    type->kind == E_TY_FUNC || type->kind == E_TY_HOLE))
        type = type->left;
    return type;
}

static inline int spec_defines_type(Expr* spec) {
    return spec && (spec->kind == E_TY_STRUCT || spec->kind == E_TY_ENUM) && (spec->flags & F_HAS_BODY);
}

// Create the types that file-scope specifiers define, so they exist before
// anything refers to them, whatever the order of files.
static inline void predeclare_spec(SrcFile* f, Expr* spec, Item* item, const char* owner, int flags) {
    if (!spec_defines_type(spec)) return;
    Module* m = f->mod;
    TypeKind kind = spec->kind == E_TY_ENUM ? TY_ENUM : TY_STRUCT;
    Type* t;
    if (spec->name.length) {
        const char* name = tok_str(spec->name);
        t = tag_type(f, name, kind, (spec->flags & F_UNION) != 0);
        if (t->body) {
            if (!m->is_c)
                fatal(f->path, spec->name, "redefinition of '%s %s'",
                      kind == TY_ENUM ? "enum" : t->is_union ? "union" : "struct", name);
            spec->sema = t;
            return;
        }
        if (t->sym) t->sym->fixed = 0;
        t->is_pub = (flags & F_PUB) != 0;
        if (!owner) owner = name;
    } else {
        t = type_new(kind);
        t->is_union = (spec->flags & F_UNION) != 0;
        t->from_c = m->is_c;
    }
    t->body = spec;
    t->file = f;
    t->item = item;
    spec->sema = t;

    for (size_t i = 0; i < spec->blockstmt_size; i++) {
        Expr* member = spec->blockstmt[i];
        if (member->kind == E_FIELD) {
            predeclare_spec(f, member->right, item, NULL, flags);
        } else if (member->kind == E_ENUMERATOR) {
            Entity* e = entity_new(ENT_ENUMCONST, f, member->name, tok_str(member->name));
            e->decl = member;
            e->type = t;
            e->owner = owner;
            e->state = 2;
            e->flags = flags & F_PUB;
            Entity* added = entity_add(e);
            if (added == e && !m->is_c) {
                e->sym = symbol_new(e->name, SP_ORDINARY);
                symbol_context(e->sym, owner);
                symbol_context_file(e->sym, f);
            }
            member->sema = added;
        }
    }
}

static inline void declare_function(SrcFile* f, Expr* node, int is_def) {
    Module* m = f->mod;
    const char* name = tok_str(node->name);
    if (m->is_c) {
        Entity* e = entity_new(ENT_FUNC, f, node->name, name);
        e->decl = node;
        entity_add(e);
        return;
    }
    // a static declaration only continues a static one of this file
    Entity* e = entity_is_static(m, ENT_FUNC, node->flags) ? (Entity*)map_get(&f->statics, name) : file_entity(f, name);
    if (e && e->kind == ENT_FUNC) {
        Func* fn = e->func;
        size_t had = fn->generics ? fn->generics->blockstmt_size : 0;
        size_t has = node->generics ? node->generics->blockstmt_size : 0;
        if (had != has)
            fatal(f->path, node->name, "'%s' is declared with a different number of generic parameters at %s:%zu",
                  name, e->file->path, e->tok.row);
        if (is_def) {
            if (fn->def)
                fatal(f->path, node->name, "redefinition of function '%s' (already defined at %s:%zu)",
                      name, fn->file->path, fn->def->name.row);
            fn->def = node;
            fn->proto = node;
            fn->generics = node->generics;
            fn->file = f;
        }
        fn->flags |= node->flags;
        e->flags |= node->flags;
        return;
    }
    e = entity_new(ENT_FUNC, f, node->name, name);
    e->decl = node;
    e->flags = node->flags;
    entity_add(e);
    Func* fn = (Func*)xalloc(sizeof(Func));
    fn->name = name;
    fn->ent = e;
    fn->proto = node;
    fn->def = is_def ? node : NULL;
    fn->generics = node->generics;
    fn->file = f;
    fn->mod = m;
    fn->flags = node->flags;
    e->func = fn;
}

static inline void declare_variable(SrcFile* f, Expr* decl, Expr* group) {
    Module* m = f->mod;
    const char* name = tok_str(decl->name);
    int defines = decl->left || !(decl->flags & F_EXTERN);
    Entity* e = entity_is_static(m, ENT_VAR, decl->flags) ? (Entity*)map_get(&f->statics, name) : file_entity(f, name);
    if (e && e->kind == ENT_VAR && !m->is_c) {
        if (decl->left && e->decl->left)
            fatal(f->path, decl->name, "redefinition of '%s' (already defined at %s:%zu)",
                  name, e->file->path, e->tok.row);
        if (defines && (decl->left || !e->has_def)) {
            int pub = e->flags & F_PUB;
            e->decl = decl;
            e->group = group;
            e->flags = decl->flags | pub;
            e->file = f;
            e->tok = decl->name;
            e->has_def = 1;
        }
        e->flags |= decl->flags & F_PUB;
        return;
    }
    e = entity_new(ENT_VAR, f, decl->name, name);
    e->decl = decl;
    e->group = group;
    e->flags = decl->flags;
    e->has_def = defines;
    if (entity_add(e) != e || m->is_c) return;
    e->sym = entity_symbol(e);
    if (!(decl->flags & F_STATIC)) {
        e->item = item_new(IT_VARDECL, f, decl);
        e->item->ent = e;
    }
}

static inline void declare_group(SrcFile* f, Expr* g) {
    Module* m = f->mod;
    Expr* spec = g->type;
    int is_typedef = (g->flags & F_TYPEDEF) != 0;
    Item* item = NULL;
    if (!m->is_c) {
        if (is_typedef || g->blockstmt_size == 0)
            item = item_new(IT_TYPEDECL, f, g);
        else if (spec_defines_type(spec))
            item = item_new(IT_TAGDEF, f, g);
    }
    const char* owner = is_typedef && g->blockstmt_size ? tok_str(g->blockstmt[0]->name) : NULL;
    if (spec->name.length) owner = tok_str(spec->name);
    predeclare_spec(f, spec, item, owner, g->flags);

    for (size_t i = 0; i < g->blockstmt_size; i++) {
        Expr* d = g->blockstmt[i];
        if (is_typedef) {
            Entity* e = entity_new(ENT_TYPEDEF, f, d->name, tok_str(d->name));
            e->decl = d;
            e->group = g;
            e->flags = g->flags;
            e->item = item;
            if (entity_add(e) == e && !m->is_c) e->sym = entity_symbol(e);
        } else if (decl_function_type(d->type)) {
            declare_function(f, d, 0);
        } else {
            if (d->generics)
                fatal(f->path, d->name, "variables cannot declare generic parameters");
            declare_variable(f, d, g);
        }
    }
}

static inline void declare_impl(SrcFile* f, Expr* node) {
    Impl* im = (Impl*)xalloc(sizeof(Impl));
    im->mod = f->mod;
    im->file = f;
    im->decl = node;
    for (size_t i = 0; i < node->blockstmt_size; i++) {
        Expr* def = node->blockstmt[i];
        Func* fn = (Func*)xalloc(sizeof(Func));
        fn->name = tok_str(def->name);
        fn->impl = im;
        fn->proto = def;
        fn->def = def;
        fn->generics = def->generics;
        fn->file = f;
        fn->mod = f->mod;
        fn->flags = def->flags;
        for (size_t j = 0; j < im->methods.len; j++)
            if (strcmp(((Func*)im->methods.items[j])->name, fn->name) == 0)
                fatal(f->path, def->name, "method '%s' is defined twice in this impl", fn->name);
        list_push(&im->methods, fn);
    }
    list_push(&g_impls, im);
}

// Record everything a parsed file declares at file scope
static inline void declare_one(SrcFile* f, Expr* d);

static inline void declare_file(SrcFile* f) {
    for (size_t i = 0; i < f->ast->blockstmt_size; i++) {
        Expr* d = f->ast->blockstmt[i];
        RECOVER(declare_one(f, d));
    }
}

static inline void declare_one(SrcFile* f, Expr* d) {
    Module* m = f->mod;
    {
        switch (d->kind) {
            case E_DECLGROUP:
                declare_group(f, d);
                break;
            case E_FUNCDEF:
                declare_function(f, d, 1);
                break;
            case E_GTYPEDEF: {
                Entity* e = entity_new(ENT_GTYPEDEF, f, d->name, tok_str(d->name));
                e->decl = d;
                e->flags = d->flags;
                entity_add(e);
                break;
            }
            case E_TRAIT: {
                Entity* e = entity_new(ENT_TRAIT, f, d->name, tok_str(d->name));
                e->decl = d;
                e->flags = d->flags;
                entity_add(e);
                for (size_t a = 0; a < d->blockstmt_size; a++)
                    for (size_t b = 0; b < a; b++)
                        if (token_same(d->blockstmt[a]->name, d->blockstmt[b]->name))
                            fatal(f->path, d->blockstmt[a]->name, "trait '%s' declares '%.*s' twice", e->name,
                                  (int)d->blockstmt[a]->name.length, d->blockstmt[a]->name.start);
                break;
            }
            case E_IMPL:
                declare_impl(f, d);
                break;
            case E_RAW:
                item_new(IT_RAW, f, d);
                m->has_raw = 1;
                break;
            case E_STATIC_ASSERT:
                // an assertion in a C header is checked where that header is included
                if (!m->is_c) item_new(IT_STATIC_ASSERT, f, d);
                break;
            default: break;
        }
    }
}

// A namespace binding may not share its name with a declaration of the file's
// own namespace.
static inline void file_check_imports(SrcFile* f) {
    for (int i = 0; i < f->nimports; i++) {
        Entity* e = (Entity*)map_get(&f->mod->names, f->imports[i].name);
        if (e)
            fatal(f->path, f->imports[i].tok, "namespace binding '%s' conflicts with the %s declared at %s:%zu; choose a distinct alias with 'as'",
                  f->imports[i].name, entity_kind_names[e->kind], e->file->path, e->tok.row);
    }
}

static inline void file_parse(SrcFile* f) {
    Parser p = parser_new(f->source);
    p.filename = f->path;
    p.env = f;
    p.lookup_type = env_lookup_type;
    p.lookup_dotop = env_lookup_dotop;
    p.lookup_generic = env_lookup_generic;
    f->ast = parse_translation_unit(&p);
    g_error_count += p.errors;
}

// ---------------------------------------------------------------------------
// C header discovery
// ---------------------------------------------------------------------------

static inline char* run_capture(const char* command) {
    FILE* pipe = popen(command, "r");
    if (!pipe) return NULL;
    Buf b = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, pipe)) > 0)
        buf_putn(&b, chunk, n);
    if (pclose(pipe) != 0) return NULL;
    buf_reserve(&b, 1);
    b.data[b.len] = '\0';
    return b.data;
}

// Some macros stand for a type (`bool`, `complex`, ...). Those must act as type
// names, or declarations using them could not be parsed.
static inline void macro_try_type(SrcFile* f, Entity* e, Parser* p) {
    const char* body = e->macro_body;
    if (!body || !(isalpha((unsigned char)body[0]) || body[0] == '_')) return;
    *p = parser_new(body);
    p->filename = "<macro>";
    p->header_mode = 1;
    p->strict_types = 1;
    p->env = f;
    p->lookup_type = env_lookup_type;
    parser_skip_noise(p);

    jmp_buf attempt;
    Expr* volatile type = NULL;
    p->spec_jmp[p->spec_depth++] = &attempt;
    if (setjmp(attempt) == 0) {
        if (parser_at_type(p)) {
            Expr* parsed = parse_type_name(p);
            if (p->token.kind == TK_EOF) type = parsed;
        }
    }
    if (!type) return;
    Expr* decl = expr_new(E_DECL, type->token, NULL, NULL);
    decl->type = type;
    e->kind = ENT_TYPEDEF;
    e->decl = decl;
}

// Macros are invisible in preprocessed output; `-dM` lists them. They become
// names Typed C accepts without knowing much about them.
static inline void cunit_add_macros(SrcFile* f, const char* dump) {
    Parser* scratch = (Parser*)xalloc(sizeof(Parser));
    for (const char* line = dump; line && *line; ) {
        const char* end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > 8 && strncmp(line, "#define ", 8) == 0) {
            const char* name = line + 8;
            size_t n = 0;
            while (name + n < line + len && (isalnum((unsigned char)name[n]) || name[n] == '_')) n++;
            const char* key = xstrn(name, n);
            if (n && !map_has(&f->mod->names, key)) {
                Entity* e = entity_new(ENT_MACRO, f, (Token){0}, key);
                if (name + n < line + len && name[n] != '(') {
                    const char* body = name + n;
                    while (body < line + len && *body == ' ') body++;
                    e->macro_body = xstrn(body, (size_t)(line + len - body));
                }
                map_put(&f->mod->names, key, e);
                macro_try_type(f, e, scratch);
            }
        }
        line = end ? end + 1 : NULL;
    }
}

// The declarations a file's direct #include lines make visible. Headers are
// run through the real C preprocessor, in source order and sharing state, with
// the same flags as the C build.
static inline Module* cunit_for_file(SrcFile* f) {
    if (!f->includes.len) return NULL;
    const char* dir = path_dir(f->path);
    Buf text = {0};
    for (size_t i = 0; i < f->includes.len; i++)
        buf_printf(&text, "%s\n", (const char*)f->includes.items[i]);
    const char* key = xstrf("%s\n%s", dir, buf_str(&text));
    Module* unit = (Module*)map_get(&g_cunits, key);
    if (unit) return unit;

    const char* tmpdir = getenv("TMPDIR");
    char* tmp = xstrf("%s/typedc-XXXXXX.c", tmpdir && *tmpdir ? tmpdir : "/tmp");
    int fd = mkstemps(tmp, 2);
    if (fd < 0) fatal(f->path, (Token){0}, "cannot create a temporary file for header discovery");
    if (write(fd, text.data, text.len) != (ssize_t)text.len) {
        close(fd);
        unlink(tmp);
        fatal(f->path, (Token){0}, "cannot write a temporary file for header discovery");
    }
    close(fd);

    Buf cmd = {0};
    buf_printf(&cmd, "%s -E -x c", g_opt.cc);
    for (size_t i = 0; i < g_opt.cpp_args.len; i++)
        buf_printf(&cmd, " %s", shell_quote((const char*)g_opt.cpp_args.items[i]));
    buf_printf(&cmd, " -I%s", shell_quote(dir));
    char* declarations = run_capture(xstrf("%s -P %s", buf_str(&cmd), shell_quote(tmp)));
    char* macros = declarations ? run_capture(xstrf("%s -dM %s", buf_str(&cmd), shell_quote(tmp))) : NULL;
    unlink(tmp);
    if (!declarations)
        fatal(f->path, (Token){0}, "header discovery failed: '%s' could not preprocess this file's #include lines", g_opt.cc);

    unit = module_new("<C headers>");
    unit->is_c = 1;
    SrcFile* hf = file_new(unit, "<C headers>");
    hf->source = declarations;

    Parser p = parser_new(declarations);
    p.filename = hf->path;
    p.header_mode = 1;
    p.env = hf;
    p.lookup_type = env_lookup_type;
    hf->ast = parse_translation_unit(&p);
    declare_file(hf);
    if (macros) cunit_add_macros(hf, macros);

    map_put(&g_cunits, key, unit);
    return unit;
}
