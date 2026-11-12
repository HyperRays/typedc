#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "lexer.h"

// The compiler is a short-lived process, just free memory on unwind
static inline void* xalloc(size_t size) {
    void* p = calloc(1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "typedc: out of memory\n");
        exit(1);
    }
    return p;
}

// ---------------------------------------------------------------------------
// Growable text buffer
// ---------------------------------------------------------------------------

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} Buf;

static inline void buf_reserve(Buf* b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    b->data = (char*)realloc(b->data, cap);
    if (!b->data) {
        fprintf(stderr, "typedc: out of memory\n");
        exit(1);
    }
    b->cap = cap;
}

static inline void buf_putn(Buf* b, const char* s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static inline void buf_puts(Buf* b, const char* s) {
    buf_putn(b, s, strlen(s));
}

static inline void buf_putc(Buf* b, char c) {
    buf_putn(b, &c, 1);
}

static inline void buf_printf(Buf* b, const char* fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, args);
    b->len += (size_t)n;
    va_end(args);
}

static inline const char* buf_str(Buf* b) {
    return b->data ? b->data : "";
}

static inline void buf_indent(Buf* b, int level) {
    for (int i = 0; i < level; i++) buf_puts(b, "    ");
}

static inline char* xstrf(const char* fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char* s = (char*)xalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, args);
    va_end(args);
    return s;
}

static inline char* xstrn(const char* s, size_t n) {
    char* out = (char*)xalloc(n + 1);
    memcpy(out, s, n);
    return out;
}

static inline char* tok_str(Token tok) {
    return xstrn(tok.start ? tok.start : "", tok.start ? tok.length : 0);
}

// ---------------------------------------------------------------------------
// String-keyed map that remembers insertion order
// ---------------------------------------------------------------------------

typedef struct {
    const char** keys;
    void** vals;
    size_t len;
    size_t cap;
    int* slots;         // open addressing: index + 1 into keys, 0 = empty
    size_t nslots;
} Map;

static inline size_t map_hash(const char* s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static inline void map_index(Map* m, size_t entry) {
    size_t i = map_hash(m->keys[entry]) & (m->nslots - 1);
    while (m->slots[i]) i = (i + 1) & (m->nslots - 1);
    m->slots[i] = (int)entry + 1;
}

static inline long map_find(const Map* m, const char* key) {
    if (!m->nslots) return -1;
    size_t i = map_hash(key) & (m->nslots - 1);
    while (m->slots[i]) {
        size_t entry = (size_t)m->slots[i] - 1;
        if (strcmp(m->keys[entry], key) == 0) return (long)entry;
        i = (i + 1) & (m->nslots - 1);
    }
    return -1;
}

static inline void* map_get(const Map* m, const char* key) {
    long entry = map_find(m, key);
    return entry < 0 ? NULL : m->vals[entry];
}

static inline int map_has(const Map* m, const char* key) {
    return map_find(m, key) >= 0;
}

static inline void map_put(Map* m, const char* key, void* val) {
    long entry = map_find(m, key);
    if (entry >= 0) {
        m->vals[entry] = val;
        return;
    }
    if (m->len >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->keys = (const char**)realloc(m->keys, m->cap * sizeof(*m->keys));
        m->vals = (void**)realloc(m->vals, m->cap * sizeof(*m->vals));
    }
    m->keys[m->len] = key;
    m->vals[m->len] = val;
    m->len++;
    if (m->len * 2 > m->nslots) {
        m->nslots = m->nslots ? m->nslots * 2 : 32;
        free(m->slots);
        m->slots = (int*)xalloc(m->nslots * sizeof(int));
        for (size_t i = 0; i < m->len; i++) map_index(m, i);
    } else {
        map_index(m, m->len - 1);
    }
}

// ---------------------------------------------------------------------------
// Pointer list
// ---------------------------------------------------------------------------

typedef struct {
    void** items;
    size_t len;
    size_t cap;
} List;

static inline void list_push(List* l, void* item) {
    if (l->len >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = (void**)realloc(l->items, l->cap * sizeof(void*));
    }
    l->items[l->len++] = item;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Declarations taken from C headers are checked leniently: an error while one
// is being resolved unwinds to whoever asked instead of ending the compilation.
#define TRY_MAX 64
static jmp_buf* g_try_stack[TRY_MAX];
static int g_try_depth;

// An error ends the declaration, statement or function it is in, not the
// compilation: `fatal` reports it and continues at the innermost recovery
// point. Without one it exits. Nothing is written once an error was reported.
#define ERROR_LIMIT 20
static jmp_buf* g_recover;
static int g_error_count;
static char* g_error_seen[ERROR_LIMIT];

static inline _Noreturn void error_continue(jmp_buf* at) {
    g_recover = at;
    if (at) longjmp(*at, 1);
    exit(1);
}

static inline void errors_stop(void) {
    if (g_error_count) exit(1);
}

// Run `code`; an error inside it is reported and execution continues after it
#define RECOVER(code) do { \
        jmp_buf recover_here_; \
        jmp_buf* recover_outer_ = g_recover; \
        g_recover = &recover_here_; \
        if (setjmp(recover_here_) == 0) { code; } \
        g_recover = recover_outer_; \
    } while (0)

static inline _Noreturn void fatal(const char* file, Token tok, const char* fmt, ...) {
    if (g_try_depth > 0)
        longjmp(*g_try_stack[g_try_depth - 1], 1);
    char text[2048];
    int n;
    if (file && tok.row)
        n = snprintf(text, sizeof text, "%s:%zu:%zu: error: ", file, tok.row, tok.col);
    else if (file)
        n = snprintf(text, sizeof text, "%s: error: ", file);
    else
        n = snprintf(text, sizeof text, "typedc: error: ");
    va_list args;
    va_start(args, fmt);
    vsnprintf(text + n, sizeof text - (size_t)n, fmt, args);
    va_end(args);

    // generic code is checked once per specialization: report each error once
    int seen = 0;
    for (int i = 0; i < g_error_count && !seen; i++)
        seen = strcmp(g_error_seen[i], text) == 0;
    if (!seen) {
        fprintf(stderr, "%s\n", text);
        g_error_seen[g_error_count++] = strdup(text);
        if (g_error_count == ERROR_LIMIT) {
            fprintf(stderr, "typedc: too many errors, stopping\n");
            exit(1);
        }
    }
    error_continue(g_recover);
}
