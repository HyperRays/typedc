#pragma once
#include "check.h"

// Naming and assembly of the generated C.
//
// Layout of the output:
//   1. #include lines, as written, deduplicated
//   2. declarations: types, raw C blocks and extern declarations of globals,
//      ordered so every type is complete where its size is needed
//   3. prototypes of every function
//   4. definitions of globals
//   5. function definitions

// ---------------------------------------------------------------------------
// Item text
// ---------------------------------------------------------------------------

static void item_generate(Item* it) {
    if (it->text) return;
    it->text = "";      // an item may reach itself through its own members
    Ctx c = ctx_for_file(it->file);
    Buf b = {0};

    // the source comments in front of what this item was written as
    Expr* commented = it->node;
    if (it->kind == IT_TAGDEF) commented = NULL;                       // they go with the variable
    if (it->kind == IT_INSTANCE && it->type->quiet) commented = NULL;  // first specialization only
    if (it->kind == IT_VARDECL) {
        Entity* e = it->ent;
        commented = !e->has_def && e->group->blockstmt[0] == e->decl ? e->group : NULL;
    }
    if (commented) put_comments(&b, commented->lead, commented->lead_len, 0, 0, 1, 0);
    if (it->node && it->kind != IT_VARDECL) buf_puts(&b, line_at(it->file, it->node->token));

    switch (it->kind) {
        case IT_RAW:
            buf_printf(&b, "%s\n", raw_text(it->node->token));
            break;
        case IT_TYPEDECL: case IT_TAGDEF: {
            Expr* g = it->node;
            if (it->kind == IT_TAGDEF || !(g->flags & F_TYPEDEF)) {
                // `struct S { ... };`, `enum E { ... };` or a forward declaration
                Type* t = resolve_type(&c, g->type);
                type_spec_into(&b, type_unq(t), type_unq(t), 0);
                buf_puts(&b, ";\n");
                break;
            }
            buf_puts(&b, "typedef ");
            int defines = spec_defines_type(g->type);
            for (size_t i = 0; i < g->blockstmt_size; i++) {
                Entity* e = (Entity*)map_get(&it->file->mod->names, tok_str(g->blockstmt[i]->name));
                Type* t = entity_type(e)->base;
                Type* define = defines && i == 0 ? type_unq(type_spec_root(t)) : NULL;
                if (g->blockstmt_size == 1) {
                    buf_puts(&b, type_decl(t, symbol_ref(e->sym), define, 0));
                } else {
                    Type* leaf = NULL;
                    const char* declarator = decl_build(t, symbol_ref(e->sym), &leaf);
                    if (i == 0) type_spec_into(&b, leaf, define, 0);
                    buf_printf(&b, "%s%s", i ? ", " : " ", declarator);
                }
            }
            buf_puts(&b, ";\n");
            break;
        }
        case IT_INSTANCE: {
            Type* t = it->type;
            if (t->kind == TY_STRUCT) {
                buf_printf(&b, "%s %s", t->is_union ? "union" : "struct", symbol_ref(t->sym));
                struct_body_into(&b, t, 0);
                buf_puts(&b, ";\n");
            } else {
                buf_printf(&b, "typedef %s;\n", type_decl(t->base, symbol_ref(t->sym), NULL, 0));
            }
            break;
        }
        case IT_VARDECL: {
            Entity* e = it->ent;
            buf_printf(&b, "extern %s%s;\n", e->flags & F_THREAD ? "_Thread_local " : "",
                       type_decl(entity_type(e), symbol_ref(e->sym), NULL, 0));
            break;
        }
        case IT_STATIC_ASSERT:
            buf_printf(&b, "_Static_assert(%s, %s);\n", gen_expr(&c, it->node->left, NULL).s,
                       tok_str(it->node->right->token));
            break;
    }
    if (commented) put_trail(&b, commented->trail, commented->trail_len, 0);
    if (it->node && it->kind != IT_VARDECL) buf_puts(&b, line_back());
    it->text = buf_str(&b);
}

// Generating one item can instantiate more types and functions; repeat until
// nothing new appears.
static void generate_items(void) {
    size_t items = 0, funcs = 0;
    while (items < g_items.len || funcs < g_insts.len) {
        for (; items < g_items.len; items++) {
            RECOVER(item_generate((Item*)g_items.items[items]));
            g_resolving_type = NULL;
        }
        funcs = g_insts.len;
        drain_functions();
    }
}

// ---------------------------------------------------------------------------
// Names (SPEC section 11.2)
// ---------------------------------------------------------------------------

static const char* symbol_candidate(Symbol* s) {
    if (s->fixed || !s->level) return s->base;
    Buf b = {0};
    // digit guard (GRAMMAR section 9.3): a context may start with a digit, a name may not
    if (isdigit((unsigned char)s->context[s->level - 1][0])) buf_putc(&b, 'n');
    for (int i = s->level - 1; i >= 0; i--)
        buf_printf(&b, "%s_", s->context[i]);
    buf_puts(&b, s->base);
    return buf_str(&b);
}

static const char* symbol_key(Symbol* s, const char* name) {
    return xstrf("%c%s", s->space == SP_TAG ? 'T' : 'O', name);
}

static int compare_symbols(const void* a, const void* b) {
    Symbol* x = *(Symbol* const*)a;
    Symbol* y = *(Symbol* const*)b;
    int order = strcmp(x->identity ? x->identity : "", y->identity ? y->identity : "");
    return order ? order : x->id - y->id;
}

// Count how many symbols want each name, and how many of those want it as
// their plain source name
static void count_candidates(Map* count, Map* plain) {
    for (size_t i = 0; i < g_symbols.len; i++) {
        Symbol* s = (Symbol*)g_symbols.items[i];
        const char* key = symbol_key(s, symbol_candidate(s));
        map_put(count, key, (void*)((intptr_t)map_get(count, key) + 1));
        if (s->fixed || !s->level) map_put(plain, key, (void*)((intptr_t)map_get(plain, key) + 1));
    }
}

// A name written in the source wins over one built from context: the single
// symbol that has the name as its source name keeps it, and the others move on.
static int symbol_keeps(Symbol* s, const char* key, Map* count, Map* plain, Map* reserved) {
    if (map_has(reserved, key) || map_has(&s->avoid, key + 1)) return 0;
    if ((intptr_t)map_get(count, key) == 1) return 1;
    return !s->level && (intptr_t)map_get(plain, key) == 1;
}

// Every symbol starts with its source name. Symbols that collide take one more
// piece of context each (specialization, owner, file, enclosing namespaces)
// until they are distinct. Whatever still collides goes back to its source name
// plus `_0`, `_1`, ... in a stable order.
static void resolve_names(void) {
    static const char* const c_keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
        "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
        "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
        "switch", "typedef", "union", "unsigned", "void", "volatile", "while"
    };
    Map reserved = {0};
    for (size_t i = 0; i < sizeof(c_keywords) / sizeof(c_keywords[0]); i++)
        map_put(&reserved, xstrf("O%s", c_keywords[i]), &reserved);
    // names the included C headers already use
    for (size_t u = 0; u < g_cunits.len; u++) {
        Module* unit = (Module*)g_cunits.vals[u];
        for (size_t i = 0; i < unit->names.len; i++)
            map_put(&reserved, xstrf("O%s", unit->names.keys[i]), &reserved);
        for (size_t i = 0; i < unit->tags.len; i++)
            map_put(&reserved, xstrf("T%s", unit->tags.keys[i]), &reserved);
    }

    size_t n = g_symbols.len;
    Symbol** syms = (Symbol**)g_symbols.items;
    for (int changed = 1; changed;) {
        changed = 0;
        Map count = {0}, plain = {0};
        count_candidates(&count, &plain);
        for (size_t i = 0; i < n; i++) {
            Symbol* s = syms[i];
            if (s->fixed || s->level >= s->ncontext) continue;
            const char* key = symbol_key(s, symbol_candidate(s));
            if (!symbol_keeps(s, key, &count, &plain, &reserved)) {
                s->level++;
                changed = 1;
            }
        }
    }

    // Names that are settled; then number the rest
    Map taken = {0};
    List left = {0};
    Map count = {0}, plain = {0};
    count_candidates(&count, &plain);
    for (size_t i = 0; i < n; i++) {
        Symbol* s = syms[i];
        const char* name = symbol_candidate(s);
        const char* key = symbol_key(s, name);
        if (s->fixed || symbol_keeps(s, key, &count, &plain, &reserved)) {
            s->final = name;
            map_put(&taken, key, s);
        } else {
            list_push(&left, s);
        }
    }
    qsort(left.items, left.len, sizeof(void*), compare_symbols);
    for (size_t i = 0; i < left.len; i++) {
        Symbol* s = (Symbol*)left.items[i];
        for (int k = 0;; k++) {
            const char* name = xstrf("%s_%d", s->base, k);
            const char* key = symbol_key(s, name);
            if (map_has(&taken, key) || map_has(&reserved, key) || map_has(&s->avoid, name)) continue;
            s->final = name;
            map_put(&taken, key, s);
            break;
        }
    }
}

// Replace symbol placeholders with the names chosen for them
static const char* substitute_names(const char* text) {
    Buf b = {0};
    for (const char* s = text; *s; s++) {
        if (*s != '\x01') {
            buf_putc(&b, *s);
            continue;
        }
        int id = (int)strtol(s + 1, (char**)&s, 10);
        buf_puts(&b, ((Symbol*)g_symbols.items[id])->final);
    }
    return buf_str(&b);
}

// ---------------------------------------------------------------------------
// Ordering the declaration section
// ---------------------------------------------------------------------------

static void item_place(Item* it, Buf* out);

// Place whatever must come before a use of type `t`. `by_value` is set when
// the use needs the complete type rather than just its name.
static void place_type(Type* t, int by_value, Buf* out) {
    if (!t) return;
    Type* u = type_unq(t);
    switch (t->kind) {
        case TY_ALIAS:
            if (u->item) item_place(u->item, out);
            if (by_value) place_type(u->base, 1, out);
            break;
        case TY_STRUCT:
            // instances are forward declared; a tag can be used before its definition
            if (u->item && (by_value || (!u->is_inst && !u->name))) item_place(u->item, out);
            break;
        case TY_ENUM:
            if (u->item) item_place(u->item, out);
            break;
        case TY_PTR:
            place_type(t->base, 0, out);
            break;
        case TY_ARRAY:
            place_type(t->base, by_value, out);
            break;
        case TY_FUNC:
            place_type(t->base, 0, out);
            for (int i = 0; i < t->nargs; i++) place_type(t->args[i], 0, out);
            break;
        default: break;
    }
}

// A struct that holds itself by value, directly or through other structs,
// would have no finite size. `holder` and `member` say where `t` is used.
static void check_layout(Type* t, Type* holder, const char* member) {
    Type* u = type_unq(type_canon(t));
    while (u->kind == TY_ARRAY) u = type_unq(type_canon(u->base));
    if (u->kind != TY_STRUCT || u->from_c || u->layout == 2) return;
    if (u->layout == 1) {
        const char* where = member ? xstrf("member '%s'", member) : "an unnamed member";
        const char* via = holder == u ? "" : xstrf(" of '%s'", type_show(holder));
        Token at = u->body ? u->body->token : (Token){0};
        fatal(u->file ? u->file->path : NULL, at,
              "'%s' contains itself by value (%s%s), so it has no finite size; use a pointer, as in '%s*'",
              type_show(u), where, via, type_show(u));
    }
    u->layout = 1;
    struct_resolve(u);
    for (int i = 0; i < u->nfields; i++)
        if (u->fields[i].type) check_layout(u->fields[i].type, u, u->fields[i].name);
    u->layout = 2;
}

static void place_members(Type* t, Buf* out) {
    Type* u = type_unq(t);
    if (u->kind != TY_STRUCT) return;
    check_layout(u, NULL, NULL);
    struct_resolve(u);
    for (int i = 0; i < u->nfields; i++)
        if (u->fields[i].type) place_type(u->fields[i].type, 1, out);
}

static void item_place(Item* it, Buf* out) {
    if (it->state) return;
    it->state = 1;
    Ctx c = ctx_for_file(it->file);

    // text of a file keeps its order around raw C, which may define or use anything
    if (it->prev_raw) item_place(it->prev_raw, out);
    switch (it->kind) {
        case IT_RAW:
            for (int i = 0; i < it->file_index; i++)
                item_place((Item*)it->file->items.items[i], out);
            break;
        case IT_TYPEDECL: case IT_TAGDEF: {
            Expr* g = it->node;
            if (spec_defines_type(g->type)) place_members(resolve_type(&c, g->type), out);
            if (it->kind == IT_TYPEDECL && (g->flags & F_TYPEDEF)) {
                for (size_t i = 0; i < g->blockstmt_size; i++) {
                    Entity* e = (Entity*)map_get(&it->file->mod->names, tok_str(g->blockstmt[i]->name));
                    place_type(entity_type(e)->base, 0, out);
                }
            }
            break;
        }
        case IT_INSTANCE:
            if (it->type->kind == TY_STRUCT) place_members(it->type, out);
            else place_type(it->type->base, 0, out);
            break;
        case IT_VARDECL:
            place_type(entity_type(it->ent), 1, out);
            break;
        default: break;
    }
    buf_puts(out, it->text);
    buf_putc(out, '\n');
    it->state = 2;
}

// ---------------------------------------------------------------------------
// Assembly
// ---------------------------------------------------------------------------

typedef struct {
    Buf header;     // header-only output, or the public interface in wrapper mode
    Buf impl;       // wrapper mode: the implementation file
} Output;

static int func_is_main(FuncInst* fi) {
    return !fi->fn->impl && fi->fn->mod->is_root && fi->fn->def && strcmp(fi->fn->name, "main") == 0;
}

// Wrapper mode exports the root namespace's `pub` functions with external linkage
static int func_is_exported(FuncInst* fi) {
    Func* f = fi->fn;
    return f->def && f->mod->is_root && (f->flags & F_PUB) && !f->generics && !(f->impl && f->impl->nparams);
}

static const char* func_storage(FuncInst* fi, int wrapper) {
    Func* f = fi->fn;
    if (!f->def) return storage_text(f->flags & (F_EXTERN | F_STATIC | F_INLINE | F_NORETURN));
    const char* noreturn = f->flags & F_NORETURN ? "_Noreturn " : "";
    if (func_is_main(fi) || (wrapper && func_is_exported(fi))) return noreturn;
    return xstrf("static inline %s", noreturn);
}

static const char* func_head(FuncInst* fi, int wrapper) {
    return xstrf("%s%s", func_storage(fi, wrapper), type_decl(fi->type, symbol_ref(fi->sym), NULL, 0));
}

static const char* upper_ident(const char* s) {
    char* out = xstrn(s, strlen(s));
    for (char* c = out; *c; c++)
        *c = isalnum((unsigned char)*c) ? (char)toupper((unsigned char)*c) : '_';
    return out;
}

static const char* trim_line(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrn(s, n);
}

// `wrapper` splits the output into a public interface (types and `pub`
// functions of the root namespace) and an implementation file including it.
static Output emit_program(const char* guard_name, const char* api_include, int wrapper) {
    generate_items();
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->names.len; i++) {
            Entity* e = (Entity*)mod->names.vals[i];
            if (e->kind == ENT_VAR && e->sym && !e->has_def) e->sym->fixed = 1;
        }
    }
    resolve_names();

    Output o = {0};
    Buf* head = &o.header;
    const char* guard = xstrf("TYPEDC_%s_H", upper_ident(guard_name));
    buf_printf(head, "/* Generated by typedc. */\n#ifndef %s\n#define %s\n\n", guard, guard);

    // 1. includes
    // Comments written in front of imports stay here as well: an import leaves
    // nothing else behind in the C.
    // The root namespace comes first so its file header opens the output.
    Map seen = {0};
    for (size_t m = g_modules.len; m-- > 0;) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++) {
            SrcFile* f = (SrcFile*)mod->files.items[i];
            int started = 0;
            for (size_t k = 0; k < f->ast->blockstmt_size; k++) {
                Expr* d = f->ast->blockstmt[k];
                if (d->kind == E_IMPORT) {
                    size_t before = head->len;
                    put_comments(head, d->lead, d->lead_len, 0, started, 0, 0);
                    if (head->len != before) started = 1;
                    continue;
                }
                if (d->kind != E_INCLUDE) continue;
                const char* line = trim_line(tok_str(d->token));
                if (map_has(&seen, line)) continue;
                map_put(&seen, line, &seen);
                put_comments(head, d->lead, d->lead_len, 0, started, 1, 0);
                buf_printf(head, "%s\n", line);
                put_trail(head, d->trail, d->trail_len, 0);
                started = 1;
            }
        }
    }
    // size_t and ptrdiff_t can appear in generated signatures. Headers that
    // already declare size_t are left alone: a freestanding build (a kernel,
    // -nostdinc) brings its own and has no <stddef.h> to include.
    int has_size_t = map_has(&seen, "#include <stddef.h>");
    for (size_t u = 0; u < g_cunits.len && !has_size_t; u++)
        has_size_t = map_has(&((Module*)g_cunits.vals[u])->names, "size_t");
    if (!has_size_t) buf_puts(head, "#include <stddef.h>\n");
    buf_putc(head, '\n');

    // 2. declarations
    Buf decls = {0};
    for (size_t i = 0; i < g_type_insts.len; i++) {
        Type* t = (Type*)g_type_insts.items[i];
        if (t->kind == TY_STRUCT)
            buf_printf(&decls, "typedef %s %s %s;\n", t->is_union ? "union" : "struct",
                       symbol_ref(t->sym), symbol_ref(t->sym));
    }
    if (decls.len) buf_putc(&decls, '\n');
    for (size_t i = 0; i < g_items.len; i++)
        RECOVER(item_place((Item*)g_items.items[i], &decls));
    errors_stop();
    if (decls.len && decls.data[decls.len - 1] != '\n') buf_putc(&decls, '\n');
    buf_puts(head, substitute_names(buf_str(&decls)));

    // 3. prototypes
    Buf* body = wrapper ? &o.impl : head;
    if (wrapper)
        buf_printf(&o.impl, "/* Generated by typedc. */\n#include \"%s\"\n\n", api_include);
    Buf exported = {0}, internal = {0};
    int has_main = 0;
    for (size_t i = 0; i < g_insts.len; i++) {
        FuncInst* fi = (FuncInst*)g_insts.items[i];
        if (func_is_main(fi)) has_main = 1;
        // header functions keep the prototype their header gives them
        Buf* to = wrapper && func_is_exported(fi) ? &exported : &internal;
        buf_printf(to, "%s;\n", func_head(fi, wrapper));
    }
    if (exported.len) buf_printf(head, "%s\n", substitute_names(buf_str(&exported)));
    if (internal.len) buf_printf(body, "%s\n", substitute_names(buf_str(&internal)));

    // 4. globals: one definition per program. A header-only library guards
    // them with the file's label; a program (or an implementation file) is
    // the single place already.
    int guarded = !wrapper && !has_main;
    Map labels = {0};
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++) {
            SrcFile* f = (SrcFile*)mod->files.items[i];
            Buf statics = {0}, defs = {0};
            for (size_t k = 0; k < mod->names.len; k++) {
                Entity* e = (Entity*)mod->names.vals[k];
                if (e->kind != ENT_VAR || e->file != f || !e->has_def) continue;
                Ctx c = ctx_for_file(f);
                Buf* to = e->flags & F_STATIC ? &statics : &defs;
                int commented = e->group->blockstmt[0] == e->decl;
                if (commented) put_comments(to, e->group->lead, e->group->lead_len, 0, 0, 1, 0);
                buf_puts(to, line_at(f, e->decl->token));
                buf_printf(to, "%s%s%s", storage_text(e->flags & (F_STATIC | F_THREAD)),
                           alignas_text(&c, e->group->type),
                           type_decl(entity_type(e), symbol_ref(e->sym), NULL, 0));
                if (e->init_text) buf_printf(to, " = %s", e->init_text);
                buf_puts(to, ";\n");
                if (commented) put_trail(to, e->group->trail, e->group->trail_len, 0);
                buf_puts(to, line_back());
            }
            if (statics.len) buf_printf(body, "%s\n", substitute_names(buf_str(&statics)));
            if (!defs.len) continue;
            // flattened paths can collide (a_b/c and a/b_c): number them in order
            const char* label = f->label;
            for (int k = 0; map_has(&labels, label); k++) label = xstrf("%s_%d", f->label, k);
            map_put(&labels, label, f);
            if (guarded)
                buf_printf(body, "#if defined(TYPEDC_GLOBALS) || defined(TYPEDC_GLOBALS_%s)\n", label);
            buf_puts(body, substitute_names(buf_str(&defs)));
            if (guarded) buf_puts(body, "#endif\n");
            buf_putc(body, '\n');
        }
    }

    // 5. functions
    for (size_t i = 0; i < g_insts.len; i++) {
        FuncInst* fi = (FuncInst*)g_insts.items[i];
        if (!fi->body) continue;
        Func* f = fi->fn;
        if (fi->first) {
            // comments on an impl block introduce its first method
            if (f->impl && f->impl->methods.items[0] == f)
                put_comments(body, f->impl->decl->lead, f->impl->decl->lead_len, 0, 0, 1, 0);
            put_comments(body, f->def->lead, f->def->lead_len, 0, 0, 1, 0);
        }
        buf_puts(body, line_at(f->file, f->def->name));
        buf_puts(body, substitute_names(xstrf("%s %s\n", func_head(fi, wrapper), fi->body)));
        if (fi->first) put_trail(body, f->def->trail, f->def->trail_len, 0);
        buf_puts(body, line_back());
        buf_putc(body, '\n');
    }

    // comments after the last declaration of a file
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++) {
            Expr* ast = ((SrcFile*)mod->files.items[i])->ast;
            size_t before = body->len;
            put_comments(body, ast->inner, ast->inner_len, 0, 0, 0, 0);
            if (body->len != before) buf_putc(body, '\n');
        }
    }

    buf_printf(head, "#endif /* %s */\n", guard);
    return o;
}
