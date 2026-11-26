#pragma once
// typedcc: Typed C compiled straight to an executable by a bundled TinyCC
// (built with -DTYPEDC_BUNDLED; tools/build-typedcc.sh).
//
// TinyCC preprocesses C headers in place of `cc -E`, and compiles and links
// the generated C. The files it needs at run time are packed into the binary
// (tools/pack.c, src/typedcc_files.c) and unpacked into a fresh temporary
// directory for each run, which is TinyCC's {B} directory:
//   {B}/include, {B}/libtcc1.a   TinyCC's own headers and runtime library
//   {B}/libc/{include,lib}       Linux: glibc 2.31 (dynamic linking), or
//                                musl with -static (static linking)
//   {B}/sdk                      macOS: link to the SDK, which cannot be bundled
//   {B}/include, {B}/lib         Windows: TinyCC's C and Windows API headers,
//                                and the .def files of the system DLLs; programs
//                                use msvcrt.dll, which every Windows has
// On Linux without -static, the headers and libraries installed on the system
// are found after the bundled glibc, as with any compiler.
#include "libtcc.h"

void typedcc_set_ppfp(TCCState* s, FILE* f);    // tcc_glue.c
void typedcc_add_rpath(TCCState* s, const char* dir);
extern const unsigned char typedcc_files[];     // typedcc_files.c
extern const unsigned char typedcc_files_end[];

static struct {
    const char* dir;        // where the files are unpacked
    int is_static;          // -static: link statically against musl
    List lib_paths;         // -L
    List rpaths;            // -rpath: run-time library search path of the program
    int run;                // -i: run the program instead of writing it
    int run_dashes;         // a -- was given
    char** run_args;        // the arguments after --
    int run_argc;
    List libs;              // -l
    List link_inputs;       // .c .o .a .so files linked into the program
} g_bundle;

// ---------------------------------------------------------------------------
// The packed files
// ---------------------------------------------------------------------------

static void bundle_cleanup(void) {
    if (g_bundle.dir) os_remove_tree(g_bundle.dir);
}

// Creates the directories a file path goes through
static void bundle_mkdirs(char* path) {
    for (char* p = path + os_root_length(path) + 1; *p; p++) {
        if (!os_is_separator(*p)) continue;
        char separator = *p;
        *p = '\0';
        os_make_dir(path);
        *p = separator;
    }
}

// Unpacks TinyCC's files, and those of `libc` ("glibc", "musl" or NULL)
static void bundle_unpack(const char* libc) {
    char* dir = os_temp_dir("typedcc-");
    if (!dir) {
        fprintf(stderr, "typedcc: cannot create a temporary directory\n");
        exit(1);
    }
    g_bundle.dir = dir;
    atexit(bundle_cleanup);

    size_t libc_len = libc ? strlen(libc) : 0;
    const unsigned char* p = typedcc_files;
    while (p < typedcc_files_end && *p) {
        const char* name = (const char*)p;
        p += strlen(name) + 1;
        uint64_t size = 0;
        for (int i = 0; i < 8; i++) size |= (uint64_t)p[i] << (8 * i);
        int exec = p[8];
        const unsigned char* data = p + 9;
        p = data + size;

        char* to = NULL;
        if (!strncmp(name, "tcc/", 4))
            to = xstrf("%s/%s", dir, name + 4);
        else if (libc && !strncmp(name, libc, libc_len) && name[libc_len] == '/')
            to = xstrf("%s/libc/%s", dir, name + libc_len + 1);
        if (!to) continue;
        bundle_mkdirs(to);
        FILE* f = fopen(to, "wb");
        if (!f || fwrite(data, 1, size, f) != size || fclose(f) != 0) {
            perror(to);
            exit(1);
        }
        if (exec) os_set_executable(to);
    }
}

#ifdef __APPLE__
// The macOS SDK: $SDKROOT, else what xcrun reports, else the one of the
// Command Line Tools
static const char* bundle_sdk(void) {
    const char* sdk = getenv("SDKROOT");
    if (sdk && *sdk) return sdk;
    char* out = os_shell("xcrun --show-sdk-path", 1);
    if (out) {
        out[strcspn(out, "\n")] = '\0';
        if (*out) return out;
    }
    return "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
}
#endif

static void bundle_init(void) {
#ifdef __APPLE__
    if (g_bundle.is_static) {
        fprintf(stderr, "typedcc: -static is not supported on macOS\n");
        exit(2);
    }
    const char* sdk = bundle_sdk();
    if (!os_exists(xstrf("%s/usr/include/stdio.h", sdk))) {
        fprintf(stderr,
                "typedcc: no macOS SDK at '%s'; install the Command Line Tools "
                "(xcode-select --install) or set SDKROOT\n", sdk);
        exit(1);
    }
    bundle_unpack(NULL);
    const char* link = xstrf("%s/sdk", g_bundle.dir);
    if (!os_symlink(sdk, link)) {
        fprintf(stderr, "typedcc: cannot link the SDK into %s\n", link);
        exit(1);
    }
#elif defined _WIN32
    if (g_bundle.is_static || g_bundle.rpaths.len) {
        fprintf(stderr, "typedcc: %s is not supported on Windows\n", g_bundle.is_static ? "-static" : "-rpath");
        exit(2);
    }
    bundle_unpack(NULL);
#else
    bundle_unpack(g_bundle.is_static ? "musl" : "glibc");
#endif
}

// ---------------------------------------------------------------------------
// TinyCC
// ---------------------------------------------------------------------------

static void bundle_quiet(void* opaque, const char* message) {
    (void)opaque, (void)message;
}

// A TinyCC state with the -I / -D / ... flags of the command line
static TCCState* bundle_tcc(int output_type, int quiet) {
    TCCState* s = tcc_new();
    if (!s) {
        fprintf(stderr, "typedcc: out of memory\n");
        exit(1);
    }
    if (quiet) tcc_set_error_func(s, NULL, bundle_quiet);
    tcc_set_lib_path(s, g_bundle.dir);
    if (g_bundle.is_static) tcc_set_options(s, "-static");
    // bit-fields as the system's C compilers lay them out, and as typedc does at compile time
    if (os_ms_bitfields) tcc_set_options(s, "-mms-bitfields");
    List* args = &g_opt.cpp_args;
    for (size_t i = 0; i < args->len; i++) {
        const char* arg = (const char*)args->items[i];
        // the flags main() takes with a separate value
        int separate = !strcmp(arg, "-I") || !strcmp(arg, "-D") || !strcmp(arg, "-U") ||
                       !strcmp(arg, "-include") || !strcmp(arg, "-isystem");
        const char* value = separate ? (const char*)args->items[++i] : arg + 2;
        if (!strcmp(arg, "-include")) {
            // see bundle_source
        } else if (!strcmp(arg, "-isystem")) {
            tcc_add_sysinclude_path(s, value);
        } else if (!strncmp(arg, "-I", 2)) {
            tcc_add_include_path(s, value);
        } else if (!strncmp(arg, "-D", 2)) {
            const char* eq = strchr(value, '=');
            tcc_define_symbol(s, eq ? xstrn(value, (size_t)(eq - value)) : value, eq ? eq + 1 : "1");
        } else if (!strncmp(arg, "-U", 2)) {
            tcc_undefine_symbol(s, value);
        } else if (tcc_set_options(s, arg) < 0) {   // -std=
            exit(2);
        }
    }
    for (size_t i = 0; i < g_bundle.lib_paths.len; i++)
        tcc_add_library_path(s, (const char*)g_bundle.lib_paths.items[i]);
    tcc_set_output_type(s, output_type);    // adds the bundled directories
#if !defined __APPLE__ && !defined _WIN32
    // After the bundled C library: the libraries installed on this system, which
    // are built against glibc, so not with -static
    if (!g_bundle.is_static) {
        static const char* include_dirs[] = {
            "/usr/local/include", "/usr/include/x86_64-linux-gnu", "/usr/include",
        };
        static const char* lib_dirs[] = {
            "/usr/local/lib", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
            "/lib64", "/usr/lib64", "/lib", "/usr/lib",
        };
        for (size_t i = 0; i < sizeof include_dirs / sizeof *include_dirs; i++)
            tcc_add_sysinclude_path(s, include_dirs[i]);
        for (size_t i = 0; i < sizeof lib_dirs / sizeof *lib_dirs; i++)
            tcc_add_library_path(s, lib_dirs[i]);
    }
#endif
    return s;
}

// `source`, after an #include line for each -include file
static const char* bundle_source(const char* source) {
    Buf b = {0};
    List* args = &g_opt.cpp_args;
    for (size_t i = 0; i < args->len; i++) {
        if (strcmp((const char*)args->items[i], "-include")) continue;
        buf_puts(&b, "#include \"");
        for (const char* p = (const char*)args->items[++i]; *p; p++) {
            if (*p == '"' || *p == '\\') buf_putc(&b, '\\');
            buf_putc(&b, *p);
        }
        buf_puts(&b, "\"\n");
    }
    if (!b.len) return source;
    buf_puts(&b, "#line 1\n");
    buf_puts(&b, source);
    return buf_str(&b);
}

// Runs the C preprocessor on `source` (preprocess_c_try in load.h)
static char* bundled_preprocess(SrcFile* f, const char* source, const char* option, int quiet) {
    TCCState* s = bundle_tcc(TCC_OUTPUT_PREPROCESS, quiet);
    tcc_add_include_path(s, path_dir(f->path));
    FILE* out = os_capture_open();
    if (!out || tcc_set_options(s, option) < 0) {
        fprintf(stderr, "typedcc: cannot start the C preprocessor\n");
        exit(1);
    }
    typedcc_set_ppfp(s, out);
    int ok = tcc_compile_string(s, bundle_source(source)) == 0;
    char* text = os_capture_close(out);
    tcc_delete(s);
    return ok ? text : NULL;
}

// Compiles the generated C and links it with the other inputs into `output`
static int bundle_compile(const char* code, const char* output) {
    TCCState* s = bundle_tcc(TCC_OUTPUT_EXE, 0);
    // the generated C includes the headers of its source files, which are found next to them
    for (size_t m = 0; m < g_modules.len; m++) {
        Module* mod = (Module*)g_modules.items[m];
        for (size_t i = 0; i < mod->files.len; i++)
            tcc_add_include_path(s, path_dir(((SrcFile*)mod->files.items[i])->path));
    }
    for (size_t i = 0; i < g_bundle.rpaths.len; i++)
        typedcc_add_rpath(s, (const char*)g_bundle.rpaths.items[i]);
    int ok = tcc_compile_string(s, bundle_source(code)) == 0;
    for (size_t i = 0; ok && i < g_bundle.link_inputs.len; i++)
        ok = tcc_add_file(s, (const char*)g_bundle.link_inputs.items[i]) == 0;
    for (size_t i = 0; ok && i < g_bundle.libs.len; i++)
        ok = tcc_add_library(s, (const char*)g_bundle.libs.items[i]) == 0;
    ok = ok && tcc_output_file(s, output) == 0;
    tcc_delete(s);
    return ok ? 0 : 1;
}

// -i: builds the program into the temporary directory, runs it there with the
// arguments after --, and returns its exit status (os_run_wait). The directory
// is removed afterwards, by bundle_cleanup.
static int bundle_run(const char* code, const char* first_input) {
    const char* exe = xstrf("%s/program%s", g_bundle.dir, os_exe_suffix);
    if (bundle_compile(code, exe) != 0) return 1;

    char** argv = (char**)xalloc(sizeof(char*) * (size_t)(g_bundle.run_argc + 2));
    argv[0] = (char*)first_input;
    for (int i = 0; i < g_bundle.run_argc; i++) argv[i + 1] = g_bundle.run_args[i];
    int status = os_run_wait(exe, argv);
    return status < 0 ? 1 : status;
}
