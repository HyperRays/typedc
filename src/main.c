#include <stdio.h>
//#define RETURN_PLAIN_TOKENS
#include "emit.h"

static void usage(FILE* out) {
    fputs(
        "Usage: typedc [options] <file" SOURCE_EXTENSION ">...\n"
        "\n"
        "Translates Typed C into header-only C11. The given files form the root\n"
        "namespace; `import a::b;` loads every " SOURCE_EXTENSION " file in <root>/a/b.\n"
        "\n"
        "Options:\n"
        "  -o <file>          write the generated header to <file> (default: stdout)\n"
        "  --api <file.h>     with --impl: write the public interface (pub declarations)\n"
        "  --impl <file.c>    with --api: write the implementation that includes it\n"
        "  --root <dir>       directory imports are resolved from (default: directory of\n"
        "                     the first input file)\n"
        "  --check            check only, write nothing\n"
        "  --line-directives  write #line directives, so the C compiler reports errors\n"
        "                     at the Typed C source instead of the generated file\n"
        "  --ast              print the syntax tree of the input files and stop\n"
        "  --cc <command>     C compiler used to preprocess #include'd headers\n"
        "                     (default: $CC, else cc)\n"
        "  -I<dir> -D<macro> -U<macro> -include <file> -isystem <dir> -std=<std>\n"
        "                     passed to that compiler; use the flags of your C build\n"
        "  -h, --help         show this help\n",
        out);
}

static int write_output(const char* path, Buf* text) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    const char* name = strrchr(path, '/');
    const char* out = lines_finish(buf_str(text), name ? name + 1 : path);
    int ok = fwrite(out, 1, strlen(out), f) == strlen(out);
    if (fclose(f) != 0) ok = 0;
    if (!ok) perror(path);
    return ok;
}

int main(int argc, char** argv) {
    const char* output = NULL;
    const char* api = NULL;
    const char* impl = NULL;
    int check_only = 0, print_ast = 0;
    List inputs = {0};

    g_opt.cc = getenv("CC");
    if (!g_opt.cc || !*g_opt.cc) g_opt.cc = "cc";

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const char** value = NULL;
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(arg, "--check")) {
            check_only = 1;
        } else if (!strcmp(arg, "--line-directives")) {
            g_opt.lines = 1;
        } else if (!strcmp(arg, "--ast")) {
            print_ast = 1;
        } else if (!strcmp(arg, "-o")) {
            value = &output;
        } else if (!strcmp(arg, "--api")) {
            value = &api;
        } else if (!strcmp(arg, "--impl")) {
            value = &impl;
        } else if (!strcmp(arg, "--root")) {
            value = &g_opt.root;
        } else if (!strcmp(arg, "--cc")) {
            value = &g_opt.cc;
        } else if (!strcmp(arg, "-include") || !strcmp(arg, "-isystem") ||
                   !strcmp(arg, "-I") || !strcmp(arg, "-D") || !strcmp(arg, "-U")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "typedc: '%s' needs a value\n", arg);
                return 2;
            }
            list_push(&g_opt.cpp_args, argv[i]);
            list_push(&g_opt.cpp_args, argv[++i]);
        } else if (!strncmp(arg, "-I", 2) || !strncmp(arg, "-D", 2) || !strncmp(arg, "-U", 2) ||
                   !strncmp(arg, "-std=", 5)) {
            list_push(&g_opt.cpp_args, argv[i]);
        } else if (arg[0] == '-' && arg[1]) {
            fprintf(stderr, "typedc: unknown option '%s'\n\n", arg);
            usage(stderr);
            return 2;
        } else {
            list_push(&inputs, argv[i]);
        }
        if (value) {
            if (i + 1 >= argc) {
                fprintf(stderr, "typedc: '%s' needs a value\n", arg);
                return 2;
            }
            *value = argv[++i];
        }
    }
    if (!inputs.len || (!!api != !!impl) || (api && output)) {
        usage(stderr);
        return 2;
    }

    types_init();
    if (!g_opt.root) g_opt.root = path_dir((const char*)inputs.items[0]);

    // The root namespace is exactly the files named on the command line
    Module* root = module_new("");
    root->is_root = 1;
    root->dir = g_opt.root;
    map_put(&g_module_by_name, "", root);
    for (size_t i = 0; i < inputs.len; i++)
        file_new(root, (const char*)inputs.items[i]);

    // 1. find imports, includes and type names; this loads imported namespaces
    module_load(root);

    // 2. discover C declarations, then parse every file in full
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++) {
            SrcFile* f = (SrcFile*)mod->files.items[i];
            file_collect_dotops(f);
            f->cunit = cunit_for_file(f);
        }
    }
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++)
            file_parse((SrcFile*)mod->files.items[i]);
    }
    errors_stop();
    if (print_ast) {
        for (size_t i = 0; i < root->files.len; i++)
            print_expr(((SrcFile*)root->files.items[i])->ast, 0);
        return 0;
    }

    // 3. record declarations, then check and lower everything
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++)
            declare_file((SrcFile*)mod->files.items[i]);
        for (size_t i = 0; i < mod->files.len; i++)
            RECOVER(file_check_imports((SrcFile*)mod->files.items[i]));
    }
    errors_stop();
    compile_program();

    // 4. write the C
    const char* first = ((SrcFile*)root->files.items[0])->label;
    const char* api_name = api ? strrchr(api, '/') : NULL;
    api_name = api_name ? api_name + 1 : api;
    Output out = emit_program(first, api_name, api != NULL);
    if (check_only) return 0;
    if (api)
        return write_output(api, &out.header) && write_output(impl, &out.impl) ? 0 : 1;
    if (output)
        return write_output(output, &out.header) ? 0 : 1;
    fputs(lines_finish(buf_str(&out.header), "<stdout>"), stdout);
    return 0;
}
