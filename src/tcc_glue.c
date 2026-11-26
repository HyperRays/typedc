// typedcc: what it needs from TinyCC beyond libtcc.h. This file sees TinyCC's
// internal state, so it is kept apart from the rest of the compiler.
#include "tcc.h"

// Where the output of the preprocessor (TCC_OUTPUT_PREPROCESS) goes; libtcc
// otherwise always writes it to stdout
void typedcc_set_ppfp(TCCState* s, FILE* f) {
    s->ppfp = f;
}

// Adds a directory to the run-time search path of the output (LC_RPATH on
// macOS, DT_RUNPATH/DT_RPATH on Linux). TinyCC keeps them as one string
// separated by ':', which is why a directory cannot contain ':'
void typedcc_add_rpath(TCCState* s, const char* dir) {
    size_t old = s->rpath ? strlen(s->rpath) : 0, n = strlen(dir);
    char* path = (char*)tcc_malloc(old + n + 2);
    if (old) {
        memcpy(path, s->rpath, old);
        path[old++] = ':';
    }
    memcpy(path + old, dir, n + 1);
    tcc_free(s->rpath);
    s->rpath = path;
}
