#pragma once
#include "check.h"

// Compile-time reflection through the builtin namespace.

// ---------------------------------------------------------------------------
// Built-in namespace
// ---------------------------------------------------------------------------

static Module* g_reflect;

enum { REFLECT_NONE, REFLECT_FIELDS, REFLECT_ENUM_NAMES, REFLECT_GENERICS, REFLECT_NAME, REFLECT_KIND };

static const char* const reflection_names[] = {
    NULL, "fields_of", "enum_names", "generics_of", "name_of", "kind_of"
};

static const char reflect_source[] =
    "#include <stddef.h>\n"
    "\n"
    "pub typedef enum {\n"
    "    FIELD_OTHER,\n"
    "    FIELD_BOOL,\n"
    "    FIELD_CHAR, FIELD_SCHAR, FIELD_UCHAR,\n"
    "    FIELD_SHORT, FIELD_USHORT,\n"
    "    FIELD_INT, FIELD_UINT,\n"
    "    FIELD_LONG, FIELD_ULONG,\n"
    "    FIELD_LLONG, FIELD_ULLONG,\n"
    "    FIELD_FLOAT, FIELD_DOUBLE, FIELD_LDOUBLE,\n"
    "    FIELD_POINTER,\n"
    "    FIELD_ARRAY,\n"
    "    FIELD_STRUCT,\n"
    "    FIELD_UNION,\n"
    "    FIELD_ENUM,\n"
    "    FIELD_FUNCTION,\n"
    "    FIELD_FUNCTION_POINTER,\n"
    "} FieldKind;\n"
    "\n"
    "pub typedef struct {\n"
    "    const char* name;\n"
    "    const char* type;\n"
    "    size_t offset;\n"
    "    size_t size;\n"
    "    unsigned bits;\n"
    "    FieldKind kind;\n"
    "} FieldInfo;\n"
    "\n"
    "pub typedef struct {\n"
    "    const char* name;\n"
    "    const char* type;\n"
    "    const char* value;\n"
    "} GenericInfo;\n"
    "\n"
    "pub typedef struct {\n"
    "    const char* name;\n"
    "    const char* mangled;\n"
    "} NameInfo;\n";

static Module* reflect_module(void) {
    if (g_reflect) return g_reflect;
    g_reflect = module_new("builtin");
    g_reflect->dir = g_opt.root;
    list_push(&g_reflect->segments, "builtin");
    map_put(&g_module_by_name, g_reflect->name, g_reflect);
    SrcFile* f = file_new(g_reflect, "<builtin>");
    f->stem = f->label = "builtin";
    f->source = xstrn(reflect_source, sizeof reflect_source - 1);
    for (size_t i = 1; i < sizeof reflection_names / sizeof reflection_names[0]; i++)
        map_put(&g_reflect->generics, reflection_names[i], f);
    return g_reflect;
}

// ---------------------------------------------------------------------------
// Reflection calls and arguments
// ---------------------------------------------------------------------------

static int g_reflect_indent;   // indentation of the declaration being initialized

static int reflect_kind(Ctx* c, Expr* e) {
    if (!g_reflect || e->kind != E_CALL) return REFLECT_NONE;
    Expr* callee = e->right;
    if (callee->kind != E_PATH || callee->blockstmt_size != 2 || (callee->flags & F_PAREN)) return REFLECT_NONE;
    const char* ns = tok_str(callee->blockstmt[0]->token);
    if (local_find(c, L_VAR | L_TYPEDEF, ns) || import_find(c->file, ns) != g_reflect) return REFLECT_NONE;
    const char* name = tok_str(callee->blockstmt[1]->token);
    for (size_t i = 1; i < sizeof reflection_names / sizeof reflection_names[0]; i++)
        if (strcmp(name, reflection_names[i]) == 0) return (int)i;
    return REFLECT_NONE;
}

static Expr* reflect_arg(Ctx* c, Expr* e, const char* name, const char* target) {
    Expr* callee = e->right;
    for (Expr* a = e->left; a; a = a->right)
        if (a->left)
            FAIL(c, a->left->token, "'%s' takes no arguments inside '()'; put the %s inside '<...>'", name, target);
    Expr* targs = callee->generics;
    if (!targs || targs->blockstmt_size != 1)
        FAIL(c, callee->token, "'%s' takes exactly one %s argument, as in '%s::%s<...>()'",
             name, target, tok_str(callee->blockstmt[0]->token), name);
    return targs->blockstmt[0];
}

static Type* reflect_type_arg(Ctx* c, Expr* arg, const char* name) {
    Expr* type = type_argument(arg);
    if (!type) FAIL(c, arg->token, "the argument of '%s' must be a type", name);
    return resolve_type(c, type);
}

static Entity* reflect_function(Ctx* c, Expr* arg) {
    Entity* ent = NULL;
    if (arg->kind == E_IDENT && arg->token.kind == TK_IDENT) {
        const char* name = tok_str(arg->token);
        if (!local_find(c, L_VAR | L_ENUMCONST | L_TYPEDEF, name) && !const_binding(c, name))
            ent = scope_entity(c, name);
    } else if (arg->kind == E_PATH) {
        ent = resolve_path(c, arg).ent;
    }
    return ent && ent->kind == ENT_FUNC ? ent : NULL;
}

// ---------------------------------------------------------------------------
// Source and generated names
// ---------------------------------------------------------------------------

static Val gen_name_of(Ctx* c, Expr* arg, Type* expected) {
    Entity* ent = reflect_function(c, arg);
    const char* name;
    const char* mangled;
    if (ent) {
        mangled = gen_entity_ref(c, ent, arg).s;
        name = ent->name;
    } else {
        Type* t = type_unq(reflect_type_arg(c, arg, "name_of"));
        name = t->name ? t->name : t->cname;
        if (!name)
            FAIL(c, arg->token, "name_of needs a named type or function, but '%s' has no name", type_show(t));
        mangled = t->sym ? symbol_ref(t->sym) : name;
    }
    Type* info = entity_type((Entity*)map_get(&g_reflect->names, "NameInfo"));
    const char* text = xstrf("{\"%s\", \"%s\"}", name, mangled);
    // Static C11 initializers need braces; expressions use a compound literal.
    if (!expected || !type_eq(expected, info, 0))
        text = xstrf("(%s)%s", type_decl(info, NULL, NULL, 0), text);
    return val(text, info);
}

static const char* reflect_name(const char* name) {
    Entity* e = (Entity*)map_get(&g_reflect->names, name);
    return e && e->sym ? symbol_ref(e->sym) : name;
}

// ---------------------------------------------------------------------------
// Type kinds
// ---------------------------------------------------------------------------

static const char* field_kind_name(Type* t) {
    // In BuiltinKind order.
    static const char* const builtin[BK_COUNT] = {
        NULL, "FIELD_BOOL", "FIELD_CHAR", "FIELD_SCHAR", "FIELD_UCHAR", "FIELD_SHORT", "FIELD_USHORT",
        "FIELD_INT", "FIELD_UINT", "FIELD_LONG", "FIELD_ULONG", "FIELD_LLONG", "FIELD_ULLONG",
        "FIELD_FLOAT", "FIELD_DOUBLE", "FIELD_LDOUBLE",
        NULL, NULL, NULL, // complex types
        NULL,            // opaque types
    };
    Type* u = type_canon(t);
    const char* kind = "FIELD_OTHER";
    switch (u->kind) {
        case TY_BUILTIN: if (builtin[u->builtin]) kind = builtin[u->builtin]; break;
        case TY_PTR:
            kind = type_canon(u->base)->kind == TY_FUNC ? "FIELD_FUNCTION_POINTER" : "FIELD_POINTER";
            break;
        case TY_FUNC:    kind = "FIELD_FUNCTION"; break;
        case TY_ARRAY:   kind = "FIELD_ARRAY"; break;
        case TY_STRUCT:  kind = type_unq(u)->is_union ? "FIELD_UNION" : "FIELD_STRUCT"; break;
        case TY_ENUM:    kind = "FIELD_ENUM"; break;
        default: break;
    }
    return kind;
}

static Entity* reflect_type_kind(Ctx* c, Expr* arg) {
    Entity* function = reflect_function(c, arg);
    Type* t = function ? gen_entity_ref(c, function, arg).t : reflect_type_arg(c, arg, "kind_of");
    if (type_has_param(t))
        FAIL(c, arg->token, "kind_of needs a concrete type, but '%s' still has unresolved generic parameters", type_show(t));
    return (Entity*)map_get(&g_reflect->names, field_kind_name(t));
}

static int reflection_constant(Ctx* c, Expr* e, long long* out) {
    if (reflect_kind(c, e) != REFLECT_KIND) return 0;
    Entity* kind = reflect_type_kind(c, reflect_arg(c, e, "kind_of", "type or function"));
    return !out || enum_value(kind->type, kind->name, out);
}

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------

static const char* reflect_type_text(Type* t) {
    Buf s = {0};
    buf_putc(&s, '"');
    for (const char* p = type_show(t); *p; p++) {
        if (*p == '\n') { buf_puts(&s, "\\n"); continue; }
        if (*p == '\r') { buf_puts(&s, "\\r"); continue; }
        if (*p == '\t') { buf_puts(&s, "\\t"); continue; }
        if (*p == '"' || *p == '\\') buf_putc(&s, '\\');
        buf_putc(&s, *p);
    }
    buf_putc(&s, '"');
    return buf_str(&s);
}

static int type_is_anonymous(Type* t) {
    Type* u = type_unq(type_canon(t));
    while (u->kind == TY_ARRAY || u->kind == TY_PTR) u = type_unq(type_canon(u->base));
    return (u->kind == TY_STRUCT || u->kind == TY_ENUM) &&
           !u->name && !u->cname && !u->is_inst && t->kind != TY_ALIAS;
}

// Flatten anonymous structs and unions into the enclosing type's field table.
static void reflect_fields_into(Ctx* c, Buf* out, const char* owner, Type* st) {
    for (int i = 0; i < st->nfields; i++) {
        Field* f = &st->fields[i];
        if (f->verbatim || !f->type) continue;
        if (!f->name) {
            Type* inner = type_unq(type_canon(f->type));
            if (!f->bits && inner->kind == TY_STRUCT) {
                struct_resolve(inner);
                reflect_fields_into(c, out, owner, inner);
            }
            continue;   // Skip unnamed bit-field padding.
        }
        Type* ft = type_canon(f->type);
        const char* offset = xstrf("offsetof(%s, %s)", owner, f->name);
        const char* size;
        if (f->bits) {
            offset = size = "0";    // neither offsetof nor sizeof applies to a bit-field
        } else if (ft->kind == TY_ARRAY && !ft->size) {
            size = "0";             // flexible array member
        } else if (f->defines || type_is_anonymous(f->type)) {
            size = xstrf("sizeof(((%s*)0)->%s)", owner, f->name);
        } else {
            size = xstrf("sizeof(%s)", type_decl(f->type, NULL, NULL, 0));
        }
        buf_indent(out, g_reflect_indent + 1);
        buf_printf(out, "{\"%s\", %s, %s, %s, %s, %s},\n", f->name, reflect_type_text(f->type), offset, size,
                   f->bits ? f->bits : "0", reflect_name(field_kind_name(f->type)));
    }
}

static Val gen_fields_of(Ctx* c, Expr* arg, Type* expected) {
    Type* t = reflect_type_arg(c, arg, "fields_of");
    Type* st = type_unq(type_canon(t));
    if (st->kind != TY_STRUCT)
        FAIL(c, arg->token, "fields_of needs a struct or union type, but '%s' is not one", type_show(t));
    struct_resolve(st);
    if (st->opaque)
        FAIL(c, arg->token, "fields_of cannot list the members of '%s': its definition is not known here",
             type_show(t));

    Entity* info = (Entity*)map_get(&g_reflect->names, "FieldInfo");
    Type* want = type_unq(type_canon(entity_type(info)));
    Type* arr = expected ? type_canon(expected) : NULL;
    if (arr && !type_is_unknown(arr) &&
        (arr->kind != TY_ARRAY || type_unq(type_canon(arr->base)) != want))
        FAIL(c, arg->token, "fields_of<%s>() initializes an array of builtin::FieldInfo, as in "
             "'static const builtin::FieldInfo fields[] = builtin::fields_of<%s>();', not a '%s'",
             type_show(t), type_show(t), type_show(expected));

    Buf out = {0};
    buf_puts(&out, "{\n");
    reflect_fields_into(c, &out, type_decl(t, NULL, NULL, 0), st);
    buf_indent(&out, g_reflect_indent);
    buf_putc(&out, '}');
    return val(buf_str(&out), expected);
}

// ---------------------------------------------------------------------------
// Generic arguments
// ---------------------------------------------------------------------------

static Val gen_generics_of(Ctx* c, Expr* target, Type* expected) {
    Entity* ent = reflect_function(c, target);
    Expr* params = NULL;
    Type** args = NULL;
    SrcFile* file;
    const char* name;
    if (ent) {
        file = ent->func ? ent->func->file : ent->file;
        name = ent->name;
        if (ent->func) {
            args = resolve_func_args(c, ent->func, target->generics, target->token);
            FuncInst* fi = func_inst(c, ent->func, NULL, NULL, args, target->token);
            params = ent->func->generics;
            name = fi->show;
        } else {
            gen_entity_ref(c, ent, target);
        }
    } else {
        Type* t = reflect_type_arg(c, target, "generics_of");
        Type* inst = type_unq(t);
        // Preserve generic aliases; only unwrap aliases without their own arguments.
        while (inst->kind == TY_ALIAS && !inst->is_inst && inst->base) inst = type_unq(inst->base);
        if (!inst->is_inst || !inst->nargs)
            FAIL(c, target->token, "generics_of needs a specialized generic type or a named function, but '%s' is not one",
                 type_show(t));
        params = inst->generic->decl->generics;
        args = inst->args;
        file = inst->generic->file;
        name = type_show(t);
    }

    Entity* info = (Entity*)map_get(&g_reflect->names, "GenericInfo");
    Type* want = type_unq(type_canon(entity_type(info)));
    Type* arr = expected ? type_canon(expected) : NULL;
    if (arr && !type_is_unknown(arr) &&
        (arr->kind != TY_ARRAY || type_unq(type_canon(arr->base)) != want))
        FAIL(c, target->token, "generics_of<%s>() initializes an array of builtin::GenericInfo, as in "
             "'static const builtin::GenericInfo args[] = builtin::generics_of<%s>();', not a '%s'",
             name, name, type_show(expected));

    Buf out = {0};
    buf_puts(&out, "{\n");
    for (int i = 0; i < generics_count(params); i++) {
        Expr* param = params->blockstmt[i];
        Type* arg = args[i];
        Type* type = param->type ? const_param_type(file, param) : arg;
        buf_indent(&out, g_reflect_indent + 1);
        buf_printf(&out, "{\"%s\", %s, %s},\n", tok_str(param->name),
                   reflect_type_text(type), param->type ? reflect_type_text(arg) : "NULL");
    }
    buf_indent(&out, g_reflect_indent);
    buf_putc(&out, '}');
    return val(buf_str(&out), expected);
}

// ---------------------------------------------------------------------------
// Enum names
// ---------------------------------------------------------------------------

static Val gen_enum_names(Ctx* c, Expr* arg, Type* expected) {
    Type* t = reflect_type_arg(c, arg, "enum_names");
    Type* et = type_unq(type_canon(t));
    if (et->kind != TY_ENUM)
        FAIL(c, arg->token, "enum_names needs an enum type, but '%s' is not one", type_show(t));
    struct_resolve(et);

    Type* arr = expected ? type_canon(expected) : NULL;
    if (arr && !type_is_unknown(arr)) {
        Type* elem = arr->kind == TY_ARRAY ? type_canon(arr->base) : NULL;
        Type* ch = elem && elem->kind == TY_PTR ? type_canon(elem->base) : NULL;
        if (!ch || ch->kind != TY_BUILTIN || ch->builtin != BK_CHAR)
            FAIL(c, arg->token, "enum_names<%s>() initializes an array of strings, as in "
                 "'static const char* const names[] = builtin::enum_names<%s>();', not a '%s'",
                 type_show(t), type_show(t), type_show(expected));
    }

    // Index by enum value; gaps are NULL and duplicate values keep the first name.
    Buf out = {0};
    List seen = {0};
    long long* values = (long long*)xalloc((size_t)(et->nfields + 1) * sizeof(long long));
    int* known = (int*)xalloc((size_t)(et->nfields + 1) * sizeof(int));
    long long current = -1;
    int current_known = 1;
    buf_puts(&out, "{\n");
    for (int i = 0; i < et->nfields; i++) {
        Field* f = &et->fields[i];
        if (!f->name) continue;
        long long v;
        int has = enum_value(et, f->name, &v);
        if (!has) {
            // Resolve C-header enum values from literals or earlier enumerators.
            if (f->bits) {
                char* end;
                long long n = strtoll(f->bits, &end, 0);
                current_known = 0;
                if (*f->bits && !*end) {
                    current = n;
                    current_known = 1;
                }
                for (int k = 0; k < i && !current_known; k++)
                    if (known[k] && et->fields[k].name && strcmp(et->fields[k].name, f->bits) == 0) {
                        current = values[k];
                        current_known = 1;
                    }
            } else {
                current++;
            }
            has = current_known;
            v = current;
        } else {
            current = v;
            current_known = 1;
        }
        values[i] = v;
        known[i] = has;
        if (has) {
            if (v < 0)
                FAIL(c, arg->token, "enum_names<%s>() is indexed by value, but '%s' is negative (%lld)",
                     type_show(t), f->name, v);
            int dup = 0;
            for (size_t k = 0; k < seen.len && !dup; k++) dup = *(long long*)seen.items[k] == v;
            if (dup) continue;
            long long* keep = (long long*)xalloc(sizeof v);
            *keep = v;
            list_push(&seen, keep);
        }
        buf_indent(&out, g_reflect_indent + 1);
        buf_printf(&out, "[%s] = \"%s\",\n", f->sym ? symbol_ref(f->sym) : f->name, f->name);
    }
    buf_indent(&out, g_reflect_indent);
    buf_putc(&out, '}');
    return val(buf_str(&out), expected);
}

// ---------------------------------------------------------------------------
// Expression and initializer generation
// ---------------------------------------------------------------------------

static int reflection_set_indent(int indent) {
    int outer = g_reflect_indent;
    g_reflect_indent = indent;
    return outer;
}

static int gen_reflection(Ctx* c, Expr* e, Type* expected, int initializer, Val* out) {
    int kind = reflect_kind(c, e);
    if (!kind) return 0;
    const char* name = reflection_names[kind];
    Expr* arg = reflect_arg(c, e, name,
        kind == REFLECT_NAME || kind == REFLECT_GENERICS || kind == REFLECT_KIND ? "type or function" : "type");
    if (kind == REFLECT_NAME) {
        *out = gen_name_of(c, arg, initializer ? expected : NULL);
        return 1;
    }
    if (kind == REFLECT_KIND) {
        *out = gen_entity_ref(c, reflect_type_kind(c, arg), arg);
        return 1;
    }
    if (!initializer) {
        Expr* callee = e->right;
        const char* ns = tok_str(callee->blockstmt[0]->token);
        const char* example = strcmp(name, "fields_of") == 0 ?
            xstrf("static const %s::FieldInfo fields[] = %s::fields_of<T>();", ns, ns) :
            strcmp(name, "generics_of") == 0 ?
            xstrf("static const %s::GenericInfo args[] = %s::generics_of<T>();", ns, ns) :
            xstrf("static const char* const names[] = %s::enum_names<T>();", ns);
        FAIL(c, callee->token, "'%s<...>()' stands for an initializer list, so it can only initialize an array, as in '%s'",
             name, example);
    }
    switch (kind) {
        case REFLECT_FIELDS:     *out = gen_fields_of(c, arg, expected); break;
        case REFLECT_ENUM_NAMES: *out = gen_enum_names(c, arg, expected); break;
        case REFLECT_GENERICS:   *out = gen_generics_of(c, arg, expected); break;
        default: return 0;
    }
    return 1;
}
