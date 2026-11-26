// os.h for POSIX systems: Linux and macOS
#define _GNU_SOURCE     // mkstemps, mkdtemp, open_memstream, nftw on glibc and musl
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../os.h"

const char os_exe_suffix[] = "";
const int os_ms_bitfields = 0;

static char* os_strf(const char* a, const char* b, const char* c) {
    size_t na = strlen(a), nb = strlen(b), nc = strlen(c);
    char* s = (char*)malloc(na + nb + nc + 1);
    if (!s) {
        fputs("typedc: out of memory\n", stderr);
        exit(1);
    }
    memcpy(s, a, na);
    memcpy(s + na, b, nb);
    memcpy(s + na + nb, c, nc + 1);
    return s;
}

static const char* os_temp_base(void) {
    const char* dir = getenv("TMPDIR");
    return dir && *dir ? dir : "/tmp";
}

// --- paths and files --------------------------------------------------------

int os_is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int os_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int os_is_separator(char c) {
    return c == '/';
}

size_t os_root_length(const char* path) {
    return path[0] == '/';
}

char** os_list_dir(const char* dir, size_t* count) {
    DIR* d = opendir(dir);
    if (!d) return NULL;
    size_t n = 0, cap = 16;
    char** names = (char**)malloc(cap * sizeof *names);
    for (struct dirent* e; names && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) names = (char**)realloc(names, (cap *= 2) * sizeof *names);
        if (names) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    *count = n;
    return names;
}

int os_make_dir(const char* path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static int os_remove_one(const char* path, const struct stat* st, int type, struct FTW* ftw) {
    (void)st, (void)type, (void)ftw;
    remove(path);
    return 0;
}

void os_remove_tree(const char* path) {
    nftw(path, os_remove_one, 32, FTW_DEPTH | FTW_PHYS);
}

int os_set_executable(const char* path) {
    return chmod(path, 0755) == 0;
}

char* os_temp_file(const char* prefix, const char* suffix, const char* contents) {
    char* path = os_strf(os_temp_base(), "/", prefix);
    path = os_strf(path, "XXXXXX", suffix);
    int fd = mkstemps(path, (int)strlen(suffix));
    if (fd < 0) return NULL;
    size_t len = strlen(contents);
    int ok = write(fd, contents, len) == (ssize_t)len;
    if (close(fd) != 0) ok = 0;
    if (!ok) {
        unlink(path);
        return NULL;
    }
    return path;
}

char* os_temp_dir(const char* prefix) {
    char* path = os_strf(os_temp_base(), "/", prefix);
    path = os_strf(path, "XXXXXX", "");
    return mkdtemp(path);
}

int os_symlink(const char* target, const char* link) {
    return symlink(target, link) == 0;
}

// --- programs ---------------------------------------------------------------

const char* os_quote(const char* arg) {
    // '...' keeps everything as it is, except ' itself: end, \', start again
    size_t n = 2;
    for (const char* p = arg; *p; p++) n += *p == '\'' ? 4 : 1;
    char* q = (char*)malloc(n + 1), *out = q;
    if (!q) return NULL;
    *out++ = '\'';
    for (const char* p = arg; *p; p++) {
        if (*p == '\'') {
            memcpy(out, "'\\''", 4);
            out += 4;
        } else {
            *out++ = *p;
        }
    }
    *out++ = '\'';
    *out = '\0';
    return q;
}

char* os_shell(const char* command, int quiet) {
    FILE* pipe = popen(quiet ? os_strf(command, " 2>/dev/null", "") : command, "r");
    if (!pipe) return NULL;
    size_t len = 0, cap = 65536;
    char* text = (char*)malloc(cap + 1);
    for (size_t n; text && (n = fread(text + len, 1, cap - len, pipe)) > 0;) {
        len += n;
        if (len == cap) text = (char*)realloc(text, (cap *= 2) + 1);
    }
    if (pclose(pipe) != 0 || !text) return NULL;
    text[len] = '\0';
    return text;
}

int os_run_wait(const char* program, char* const* argv) {
    fflush(NULL);
    // while the program runs, Ctrl-C and Ctrl-\ are its business: the caller
    // waits for it to end, then carries on
    struct sigaction ignore = {0}, old_int, old_quit;
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore, &old_int);
    sigaction(SIGQUIT, &ignore, &old_quit);
    pid_t pid = fork();
    if (pid == 0) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        execv(program, argv);
        perror(program);
        _exit(127);     // not exit(): that would run the caller's cleanup
    }
    int status = 0;
    if (pid < 0) perror("fork");
    else while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);
    if (pid < 0) return -1;
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
}

static char* os_captured;
static size_t os_captured_len;

FILE* os_capture_open(void) {
    os_captured = NULL;
    os_captured_len = 0;
    return open_memstream(&os_captured, &os_captured_len);
}

char* os_capture_close(FILE* f) {
    fclose(f);
    return os_captured;
}
