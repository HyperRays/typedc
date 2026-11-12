#pragma once
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern int typedc_esbmc_main(int argc, const char** argv);

static void pipeline_usage(FILE* out) {
    fputs("Usage: typedc build [options] <file.tc>...\n"
          "\nEmit C, verify it with embedded ESBMC/Z3, then invoke the C compiler.\n"
          "\nOptions:\n"
          "  -o <binary>         output executable (default: a.out)\n"
          "  --emit-c <file.c>    retain emitted C (default: <binary>.c)\n"
          "  --log <file>         retain verifier output (default: <binary>.esbmc.log)\n"
          "  --verify-only       emit and verify without compiling a binary\n"
          "  --root <dir>        directory for Typed C imports\n"
          "  --cc <executable>   external C compiler (default: $CC, else cc)\n"
          "  --timeout <time>    verification timeout (default: 30s)\n"
          "  --max-k-step <n>    verification bound (default: 32)\n"
          "  --strategy <name>   incremental-bmc (default), or k-induction\n"
          "  -I <dir> -D <macro> -include <file>\n"
          "                      shared preprocessing options for all stages\n"
          "  -O0/-O1/-O2/-O3/-Os/-Og, -g\n"
          "                      native compilation options (default: -O2)\n"
          "  -h, --help          show this help\n", out);
}

static void pipeline_append(List* to, const List* from) {
    for (size_t i = 0; i < from->len; ++i) list_push(to, from->items[i]);
}

static void pipeline_arg(List* args, const char* value) {
    list_push(args, (void*)value);
}

static const char* pipeline_quote(const char* arg) {
    if (*arg && strspn(arg, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./:=,+-@%") == strlen(arg))
        return arg;
    return shell_quote(arg);
}

static void pipeline_command(const List* args, const char* prefix, const char* log) {
    fputs("+ ", stdout);
    if (prefix) printf("%s ", pipeline_quote(prefix));
    for (size_t i = 0; i < args->len; ++i) {
        const char* arg = args->items[i];
        if (i) putchar(' ');
        fputs(pipeline_quote(arg), stdout);
    }
    if (log) printf(" > %s 2>&1", pipeline_quote(log));
    putchar('\n');
    fflush(NULL);
}

enum PipelineStage { PIPELINE_TRANSPILE, PIPELINE_VERIFY, PIPELINE_COMPILE };

static int pipeline_run(List args, enum PipelineStage stage, const char* log) {
    int fd = -1;
    if (log) {
        fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd < 0) { perror(log); return 1; }
    }
    int argc = (int)args.len;
    list_push(&args, NULL);
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) {
        perror("typedc: fork");
        if (fd >= 0) close(fd);
        return 1;
    }
    if (child == 0) {
        if (fd >= 0) {
            if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(1);
            close(fd);
        }
        // Each stage gets fresh process state, including ESBMC's timers and globals.
        if (stage == PIPELINE_TRANSPILE)
            exit(typedc_transpile(argc, (char**)args.items));
        if (stage == PIPELINE_VERIFY)
            exit(typedc_esbmc_main(argc, (const char**)args.items));
        execvp(args.items[0], (char**)args.items);
        perror(args.items[0]);
        _exit(127);
    }
    if (fd >= 0) close(fd);
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("typedc: waitpid");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int pipeline_directory(const char* dir) {
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 1;
        fprintf(stderr, "typedc: not a directory: %s\n", dir);
        return 0;
    }
    const char* parent = path_dir(dir);
    if (!strcmp(parent, dir)) { perror(dir); return 0; }
    if (!pipeline_directory(parent)) return 0;
    if (mkdir(dir, 0777) == 0 || errno == EEXIST) return 1;
    perror(dir);
    return 0;
}

static int pipeline_same_path(const char* a, const char* b) {
    struct stat sa, sb;
    if (!stat(a, &sa) && !stat(b, &sb) && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino)
        return 1;
    char pa[PATH_MAX], pb[PATH_MAX];
    if (!realpath(path_dir(a), pa) || !realpath(path_dir(b), pb)) return !strcmp(a, b);
    const char* na = strrchr(a, '/');
    const char* nb = strrchr(b, '/');
    return !strcmp(pa, pb) && !strcmp(na ? na + 1 : a, nb ? nb + 1 : b);
}

static const char* pipeline_self(const char* fallback) {
    char* path = xalloc(PATH_MAX);
#ifdef __APPLE__
    uint32_t size = PATH_MAX;
    if (_NSGetExecutablePath(path, &size) == 0) return path;
#elif defined(__linux__)
    ssize_t size = readlink("/proc/self/exe", path, PATH_MAX - 1);
    if (size > 0) { path[size] = 0; return path; }
#endif
    return fallback;
}

static int pipeline_outputs(const char** outputs, const List* inputs, const char* self) {
    for (int i = 0; i < 3; ++i) {
        const char* path = outputs[i];
        if (!*path || !pipeline_directory(path_dir(path))) return 0;
        struct stat st;
        if (!lstat(path, &st) && !S_ISREG(st.st_mode)) {
            fprintf(stderr, "typedc: output is not a regular file: %s\n", path);
            return 0;
        }
        if (pipeline_same_path(path, self)) {
            fprintf(stderr, "typedc: output would overwrite the compiler: %s\n", path);
            return 0;
        }
        for (size_t j = 0; j < inputs->len; ++j) {
            if (pipeline_same_path(path, inputs->items[j])) {
                fprintf(stderr, "typedc: output would overwrite an input: %s\n", path);
                return 0;
            }
        }
        for (int j = 0; j < i; ++j) {
            if (pipeline_same_path(path, outputs[j])) {
                fprintf(stderr, "typedc: output paths must be distinct: %s\n", path);
                return 0;
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (unlink(outputs[i]) != 0 && errno != ENOENT) { perror(outputs[i]); return 0; }
    }
    return 1;
}

static int pipeline_verdict(const char* path) {
    FILE* log = fopen(path, "r");
    if (!log) { perror(path); return 0; }
    char* line = NULL;
    size_t capacity = 0;
    int success = 0, bad = 0;
    while (getline(&line, &capacity, log) >= 0) {
        fputs(line, stdout);
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "VERIFICATION SUCCESSFUL")) ++success;
        if (!strcmp(line, "VERIFICATION FAILED") || !strcmp(line, "VERIFICATION UNKNOWN")) bad = 1;
    }
    if (ferror(log)) bad = 1;
    free(line);
    fclose(log);
    fflush(stdout);
    return success == 1 && !bad;
}

static int pipeline_main(int argc, char** argv, const char* self) {
    const char *binary = "a.out", *emitted = NULL, *log = NULL, *root = NULL;
    const char *cc = getenv("CC"), *timeout = "30s", *max_k = "32";
    const char *strategy = "incremental-bmc", *optimization = "-O2";
    int verify_only = 0, debug = 0;
    List inputs = {0}, cpp = {0}, verify_cpp = {0};
    if (!cc || !*cc) cc = "cc";
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char** value = NULL;
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { pipeline_usage(stdout); return 0; }
        if (!strcmp(arg, "-o")) value = &binary;
        else if (!strcmp(arg, "--emit-c")) value = &emitted;
        else if (!strcmp(arg, "--log")) value = &log;
        else if (!strcmp(arg, "--root")) value = &root;
        else if (!strcmp(arg, "--cc")) value = &cc;
        else if (!strcmp(arg, "--timeout")) value = &timeout;
        else if (!strcmp(arg, "--max-k-step")) value = &max_k;
        else if (!strcmp(arg, "--strategy")) value = &strategy;
        else if (!strcmp(arg, "--verify-only")) verify_only = 1;
        else if (!strcmp(arg, "-g")) debug = 1;
        else if (!strcmp(arg, "-O0") || !strcmp(arg, "-O1") || !strcmp(arg, "-O2") ||
                 !strcmp(arg, "-O3") || !strcmp(arg, "-Os") || !strcmp(arg, "-Og")) optimization = arg;
        else if (!strcmp(arg, "-I") || !strcmp(arg, "-D") || !strcmp(arg, "-include")) {
            if (++i == argc) { fprintf(stderr, "typedc: '%s' needs a value\n", arg); return 2; }
            pipeline_arg(&cpp, arg);
            pipeline_arg(&cpp, argv[i]);
            pipeline_arg(&verify_cpp, !strcmp(arg, "-include") ? "--include-file" : arg);
            pipeline_arg(&verify_cpp, argv[i]);
        } else if ((!strncmp(arg, "-I", 2) || !strncmp(arg, "-D", 2)) && arg[2]) {
            pipeline_arg(&cpp, arg);
            pipeline_arg(&verify_cpp, arg);
        } else if (!strcmp(arg, "-std=c11")) {
            // The pipeline uses C11 in all three stages.
        } else if (arg[0] == '-') {
            fprintf(stderr, "typedc: unknown build option '%s'\n", arg);
            return 2;
        } else pipeline_arg(&inputs, arg);
        if (value) {
            if (++i == argc) { fprintf(stderr, "typedc: '%s' needs a value\n", arg); return 2; }
            *value = argv[i];
        }
    }
    if (!inputs.len) { pipeline_usage(stderr); return 2; }
    if (strcmp(strategy, "incremental-bmc") && strcmp(strategy, "k-induction")) {
        fputs("typedc: strategy must be incremental-bmc or k-induction\n", stderr);
        return 2;
    }
    if (!*max_k || strspn(max_k, "0123456789") != strlen(max_k) || strtoull(max_k, NULL, 10) == 0) {
        fputs("typedc: --max-k-step must be a positive integer\n", stderr);
        return 2;
    }
    if (!root) root = path_dir(inputs.items[0]);
    if (!emitted) emitted = xstrf("%s.c", binary);
    if (!log) log = xstrf("%s.esbmc.log", binary);
    const char* outputs[] = {binary, emitted, log};
    if (!pipeline_outputs(outputs, &inputs, pipeline_self(self))) return 2;

    pipeline_arg(&cpp, "-I.");
    pipeline_arg(&verify_cpp, "-I.");
    for (size_t i = 0; i < inputs.len; ++i) {
        const char* include = xstrf("-I%s", path_dir(inputs.items[i]));
        pipeline_arg(&cpp, include);
        pipeline_arg(&verify_cpp, include);
    }
    List emit = {0};
    pipeline_arg(&emit, self);
    pipeline_arg(&emit, "--root"); pipeline_arg(&emit, root);
    pipeline_arg(&emit, "--line-directives");
    pipeline_arg(&emit, "--cc"); pipeline_arg(&emit, pipeline_quote(cc));
    pipeline_arg(&emit, "-std=c11");
    pipeline_append(&emit, &cpp);
    pipeline_append(&emit, &inputs);
    pipeline_arg(&emit, "-o"); pipeline_arg(&emit, emitted);
    pipeline_command(&emit, NULL, NULL);
    int status = pipeline_run(emit, PIPELINE_TRANSPILE, NULL);
    if (status) return status;

    List verify = {0};
    pipeline_arg(&verify, "esbmc");
    pipeline_arg(&verify, emitted);
    pipeline_append(&verify, &verify_cpp);
    const char* flags[] = {"--std", "c11", sizeof(void*) == 8 ? "--64" : "--32", "--z3",
        "--memory-leak-check", "--overflow-check", "--uninitialised-vars-check",
        "--restrict-check", "--data-races-check", NULL};
    for (size_t i = 0; flags[i]; ++i) pipeline_arg(&verify, flags[i]);
    pipeline_arg(&verify, xstrf("--%s", strategy));
    pipeline_arg(&verify, "--max-k-step"); pipeline_arg(&verify, max_k);
    pipeline_arg(&verify, "--timeout"); pipeline_arg(&verify, timeout);
    pipeline_command(&verify, self, log);
    status = pipeline_run(verify, PIPELINE_VERIFY, log);
    int proved = pipeline_verdict(log);
    if (status || !proved) {
        fputs("typedc: no complete proof; binary was not built\n", stderr);
        return status ? status : 1;
    }
    if (verify_only) return 0;

    char* temporary = xstrf("%s.tmp.XXXXXX", binary);
    int fd = mkstemp(temporary);
    if (fd < 0) { perror(temporary); return 1; }
    close(fd);
    List compile = {0};
    pipeline_arg(&compile, cc);
    pipeline_arg(&compile, "-std=c11");
    pipeline_append(&compile, &cpp);
    pipeline_arg(&compile, optimization);
    if (debug) pipeline_arg(&compile, "-g");
    pipeline_arg(&compile, "-Wall"); pipeline_arg(&compile, "-Wextra");
    pipeline_arg(&compile, "-x"); pipeline_arg(&compile, "c");
    pipeline_arg(&compile, emitted);
    pipeline_arg(&compile, "-o"); pipeline_arg(&compile, temporary);
    pipeline_command(&compile, NULL, NULL);
    status = pipeline_run(compile, PIPELINE_COMPILE, NULL);
    if (!status && rename(temporary, binary) != 0) { perror(binary); status = 1; }
    if (status) unlink(temporary);
    return status;
}
