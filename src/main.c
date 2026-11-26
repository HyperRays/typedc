#include <stdio.h>
//#define RETURN_PLAIN_TOKENS
#include "emit.h"
#ifdef TYPEDC_BUNDLED
#include "bundled.h"
#define PROGRAM "typedcc"
#else
#define PROGRAM "typedc"
#endif

static void usage(FILE* out) {
    fputs(
#ifdef TYPEDC_BUNDLED
        "Usage: typedcc [options] <file" SOURCE_EXTENSION ">... [<file.c|.o|.a|.so>...] -o <executable>\n"
        "       typedcc [options] <file" SOURCE_EXTENSION ">... [<file.c|.o|.a|.so>...] -i [-- <arguments>...]\n"
        "\n"
        "Compiles Typed C into an executable, with the C compiler built into it (TinyCC).\n"
        "\n"
        "Options:\n"
        "  -o <file>          write the executable to <file>\n"
        "  -i                 run the program instead of writing it; the arguments after\n"
        "                     -- are its own. Exits with the program's exit status\n"
        "  --root <dir>       directory imports are resolved from (default: directory of\n"
        "                     the first input file)\n"
        "  --check            check only, write nothing\n"
        "  --ast              print the syntax tree of the input files and stop\n"
        "  -static            Linux: link statically against the bundled musl; the\n"
        "                     default links dynamically against glibc (2.31 or newer)\n"
        "  -L<dir> -l<lib>    where to find libraries, and a library to link\n"
        "  -rpath <dir>       where the program finds dynamic libraries when it runs,\n"
        "                     written as given (@executable_path/..., $ORIGIN/...)\n"
        "  -I<dir> -D<macro> -U<macro> -include <file> -isystem <dir> -std=<std>\n"
        "                     C preprocessor and compiler flags\n"
        "  -h, --help         show this help\n",
#else
        "Usage: typedc [options] <file" SOURCE_EXTENSION ">...\n"
        "\n"
        "Translates Typed C into header-only C11.\n"
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
        "  --cc <command>     C compiler used for preprocessing\n"
        "                     (default: $CC, else cc)\n"
        "  -I<dir> -D<macro> -U<macro> -include <file> -isystem <dir> -std=<std>\n"
        "                     passed to that compiler; use the flags of your C build\n"
        "  -h, --help         show this help\n",
#endif
        out);
}

static int write_output(const char* path, Buf* text) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }
    const char* out = lines_finish(buf_str(text), path_base(path));
    int ok = fwrite(out, 1, strlen(out), f) == strlen(out);
    if (fclose(f) != 0) ok = 0;
    if (!ok) perror(path);
    return ok;
}

int main(int argc, char** argv) {
    const char* output = NULL;
    int check_only = 0, print_ast = 0;
    List inputs = {0};
#ifdef TYPEDC_BUNDLED
    g_opt.cc = "the bundled TinyCC";
    g_opt.lines = 1;    // TinyCC reports errors at the Typed C source
#else
    const char* api = NULL;
    const char* impl = NULL;
    g_opt.cc = getenv("CC");
    if (!g_opt.cc || !*g_opt.cc) g_opt.cc = "cc";
#endif

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const char** value = NULL;
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(arg, "--check")) {
            check_only = 1;
        } else if (!strcmp(arg, "--ast")) {
            print_ast = 1;
        } else if (!strcmp(arg, "-o")) {
            value = &output;
        } else if (!strcmp(arg, "--root")) {
            value = &g_opt.root;
#ifdef TYPEDC_BUNDLED
        } else if (!strcmp(arg, "-static")) {
            g_bundle.is_static = 1;
        } else if (!strcmp(arg, "-i")) {
            g_bundle.run = 1;
        } else if (!strcmp(arg, "--")) {
            // the rest belongs to the program run with -i
            g_bundle.run_args = argv + i + 1;
            g_bundle.run_argc = argc - i - 1;
            g_bundle.run_dashes = 1;
            break;
        } else if (!strcmp(arg, "-rpath")) {
            if (i + 1 >= argc) {
                fprintf(stderr, PROGRAM ": '%s' needs a value\n", arg);
                return 2;
            }
            const char* dir = argv[++i];
            if (!*dir || strchr(dir, ':')) {
                fprintf(stderr, "typedcc: -rpath '%s': the directory must not be empty or contain ':'\n", dir);
                return 2;
            }
            list_push(&g_bundle.rpaths, (void*)dir);
        } else if (!strcmp(arg, "-L") || !strcmp(arg, "-l")) {
            if (i + 1 >= argc) {
                fprintf(stderr, PROGRAM ": '%s' needs a value\n", arg);
                return 2;
            }
            list_push(arg[1] == 'L' ? &g_bundle.lib_paths : &g_bundle.libs, argv[++i]);
        } else if (!strncmp(arg, "-L", 2) || !strncmp(arg, "-l", 2)) {
            list_push(arg[1] == 'L' ? &g_bundle.lib_paths : &g_bundle.libs, (void*)(arg + 2));
#else
        } else if (!strcmp(arg, "--line-directives")) {
            g_opt.lines = 1;
        } else if (!strcmp(arg, "--api")) {
            value = &api;
        } else if (!strcmp(arg, "--impl")) {
            value = &impl;
        } else if (!strcmp(arg, "--cc")) {
            value = &g_opt.cc;
#endif
        } else if (!strcmp(arg, "-include") || !strcmp(arg, "-isystem") ||
                   !strcmp(arg, "-I") || !strcmp(arg, "-D") || !strcmp(arg, "-U")) {
            if (i + 1 >= argc) {
                fprintf(stderr, PROGRAM ": '%s' needs a value\n", arg);
                return 2;
            }
            list_push(&g_opt.cpp_args, argv[i]);
            list_push(&g_opt.cpp_args, argv[++i]);
        } else if (!strncmp(arg, "-I", 2) || !strncmp(arg, "-D", 2) || !strncmp(arg, "-U", 2) ||
                   !strncmp(arg, "-std=", 5)) {
            list_push(&g_opt.cpp_args, argv[i]);
        } else if (arg[0] == '-' && arg[1]) {
            fprintf(stderr, PROGRAM ": unknown option '%s'\n\n", arg);
            usage(stderr);
            return 2;
        } else {
#ifdef TYPEDC_BUNDLED
            // Typed C sources; everything else is C for the linker
            size_t n = strlen(arg), x = strlen(SOURCE_EXTENSION);
            if (n <= x || strcmp(arg + n - x, SOURCE_EXTENSION)) {
                list_push(&g_bundle.link_inputs, argv[i]);
                continue;
            }
#endif
            list_push(&inputs, argv[i]);
        }
        if (value) {
            if (i + 1 >= argc) {
                fprintf(stderr, PROGRAM ": '%s' needs a value\n", arg);
                return 2;
            }
            *value = argv[++i];
        }
    }
#ifdef TYPEDC_BUNDLED
    if (g_bundle.run && (output || check_only || print_ast)) {
        fprintf(stderr, "typedcc: -i runs the program, so it cannot be combined with %s\n",
                output ? "-o" : check_only ? "--check" : "--ast");
        return 2;
    }
    if (g_bundle.run_dashes && !g_bundle.run) {
        fprintf(stderr, "typedcc: arguments after -- are for the program run with -i\n");
        return 2;
    }
    if (!inputs.len || (!output && !g_bundle.run && !check_only && !print_ast)) {
        usage(stderr);
        return 2;
    }
#if !defined __APPLE__ && !defined _WIN32   // there bundle_init rejects -static itself
    if (g_bundle.is_static && g_bundle.rpaths.len) {
        fprintf(stderr, "typedcc: -rpath has no effect with -static, which loads no dynamic libraries\n");
        return 2;
    }
#endif
    for (size_t i = 0; g_bundle.is_static && i < g_bundle.link_inputs.len; i++) {
        const char* path = (const char*)g_bundle.link_inputs.items[i];
        const char* name = path_base(path);
        size_t n = strlen(name);
        if ((n > 3 && !strcmp(name + n - 3, ".so")) || strstr(name, ".so.")) {
            fprintf(stderr, "typedcc: '%s' is a shared library, which -static cannot link\n", path);
            return 2;
        }
    }
    bundle_init();
#else
    if (!inputs.len || (!!api != !!impl) || (api && output)) {
        usage(stderr);
        return 2;
    }
#endif

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
#ifdef TYPEDC_BUNDLED
    Output program = emit_program(first, NULL, 0);
    if (check_only) return 0;
    const char* code = lines_finish(buf_str(&program.header), "<generated>");
    if (g_bundle.run) return bundle_run(code, (const char*)inputs.items[0]);
    return bundle_compile(code, output);
#else
    const char* api_name = api ? path_base(api) : NULL;
    Output out = emit_program(first, api_name, api != NULL);
    if (check_only) return 0;
    if (api)
        return write_output(api, &out.header) && write_output(impl, &out.impl) ? 0 : 1;
    if (output)
        return write_output(output, &out.header) ? 0 : 1;
    fputs(lines_finish(buf_str(&out.header), "<stdout>"), stdout);
    return 0;
#endif
}
