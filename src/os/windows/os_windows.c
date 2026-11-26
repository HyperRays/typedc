// os.h for Windows. Compiled on its own: <windows.h> defines names (far,
// interface, ...) that the rest of the compiler uses for its own things.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include "../os.h"

const char os_exe_suffix[] = ".exe";
const int os_ms_bitfields = 1;

static void* os_alloc(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) {
        fputs("typedc: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

// A growable string
typedef struct {
    char* data;
    size_t len, cap;
} OsText;

static void os_put(OsText* t, const char* s, size_t n) {
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->data = (char*)realloc(t->data, t->cap);
        if (!t->data) {
            fputs("typedc: out of memory\n", stderr);
            exit(1);
        }
    }
    memcpy(t->data + t->len, s, n);
    t->len += n;
    t->data[t->len] = '\0';
}

static void os_puts(OsText* t, const char* s) {
    os_put(t, s, strlen(s));
}

static void os_putc(OsText* t, char c) {
    os_put(t, &c, 1);
}

// The temporary directory, without a separator at the end
static const char* os_temp_base(void) {
    static char dir[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof dir, dir);
    if (n == 0 || n > MAX_PATH) return ".";
    while (n > 1 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) dir[--n] = '\0';
    return dir;
}

// <base>\<prefix><6 random characters><suffix>
static char* os_temp_name(const char* prefix, const char* suffix) {
    static unsigned counter;
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned x = (unsigned)GetTickCount() ^ ((unsigned)GetCurrentProcessId() << 16) ^ (++counter * 2654435761u);
    char random[7];
    for (int i = 0; i < 6; i++) {
        x = x * 1103515245u + 12345u;
        random[i] = chars[(x >> 16) % 36];
    }
    random[6] = '\0';
    OsText t = {0};
    os_puts(&t, os_temp_base());
    os_putc(&t, '\\');
    os_puts(&t, prefix);
    os_puts(&t, random);
    os_puts(&t, suffix);
    return t.data;
}

// --- paths and files --------------------------------------------------------

int os_is_dir(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int os_exists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int os_is_separator(char c) {
    return c == '/' || c == '\\';
}

size_t os_root_length(const char* path) {
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
        return os_is_separator(path[2]) ? 3 : 2;
    return os_is_separator(path[0]);
}

char** os_list_dir(const char* dir, size_t* count) {
    OsText pattern = {0};
    os_puts(&pattern, dir);
    os_puts(&pattern, "\\*");
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern.data, &found);
    free(pattern.data);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size_t n = 0, cap = 16;
    char** names = (char**)os_alloc(cap * sizeof *names);
    do {
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        if (n == cap) {
            names = (char**)realloc(names, (cap *= 2) * sizeof *names);
            if (!names) break;
        }
        names[n++] = _strdup(found.cFileName);
    } while (FindNextFileA(h, &found));
    FindClose(h);
    *count = n;
    return names;
}

int os_make_dir(const char* path) {
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void os_remove_tree(const char* path) {
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(path);
        return;
    }
    size_t count = 0;
    char** names = os_list_dir(path, &count);
    for (size_t i = 0; names && i < count; i++) {
        OsText child = {0};
        os_puts(&child, path);
        os_putc(&child, '\\');
        os_puts(&child, names[i]);
        os_remove_tree(child.data);
        free(child.data);
    }
    RemoveDirectoryA(path);
}

int os_set_executable(const char* path) {
    (void)path;     // Windows decides by the name (.exe), not by a permission
    return 1;
}

char* os_temp_file(const char* prefix, const char* suffix, const char* contents) {
    for (int tries = 0; tries < 100; tries++) {
        char* path = os_temp_name(prefix, suffix);
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            free(path);
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return NULL;
        }
        DWORD len = (DWORD)strlen(contents), written = 0;
        BOOL ok = WriteFile(h, contents, len, &written, NULL) && written == len;
        if (!CloseHandle(h)) ok = FALSE;
        if (ok) return path;
        DeleteFileA(path);
        free(path);
        return NULL;
    }
    return NULL;
}

char* os_temp_dir(const char* prefix) {
    for (int tries = 0; tries < 100; tries++) {
        char* path = os_temp_name(prefix, "");
        if (CreateDirectoryA(path, NULL)) return path;
        free(path);
        if (GetLastError() != ERROR_ALREADY_EXISTS) return NULL;
    }
    return NULL;
}

// --- programs ---------------------------------------------------------------

// Quotes an argument so that a program splitting its command line by the
// Microsoft C rules gets it back unchanged: in "...", n backslashes before a
// quote become 2n + 1 followed by the quote, and n at the end become 2n.
static void os_put_argument(OsText* t, const char* arg) {
    if (*arg && !strpbrk(arg, " \t\n\v\"")) {
        os_puts(t, arg);
        return;
    }
    os_putc(t, '"');
    for (const char* p = arg;; p++) {
        size_t backslashes = 0;
        while (*p == '\\') {
            backslashes++;
            p++;
        }
        if (!*p) {
            for (size_t i = 0; i < backslashes * 2; i++) os_putc(t, '\\');
            break;
        }
        size_t times = *p == '"' ? backslashes * 2 + 1 : backslashes;
        for (size_t i = 0; i < times; i++) os_putc(t, '\\');
        os_putc(t, *p);
    }
    os_putc(t, '"');
}

const char* os_quote(const char* arg) {
    // cmd.exe expands %NAME%, and inside quotes there is no escape for it
    if (strchr(arg, '%')) return NULL;
    OsText quoted = {0}, out = {0};
    os_put_argument(&quoted, arg);
    // Then for cmd.exe, which reads the line first: a ^ before each character
    // it gives a meaning, " included, so that it never starts reading a quoted
    // part, and takes the ^ away before running the program
    for (const char* p = quoted.data; *p; p++) {
        if (strchr("()!^\"<>&|", *p)) os_putc(&out, '^');
        os_putc(&out, *p);
    }
    free(quoted.data);
    return out.data ? out.data : "\"\"";
}

char* os_shell(const char* command, int quiet) {
    // _popen runs cmd.exe /c <line>; when that line starts with a quote,
    // cmd.exe may remove its first and last quote. One more pair around it all
    // is what it removes then.
    OsText line = {0};
    os_putc(&line, '"');
    os_puts(&line, command);
    if (quiet) os_puts(&line, " 2>NUL");
    os_putc(&line, '"');
    fflush(NULL);
    FILE* pipe = _popen(line.data, "rt");
    free(line.data);
    if (!pipe) return NULL;
    OsText text = {0};
    os_put(&text, "", 0);
    char chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, pipe)) > 0;) os_put(&text, chunk, n);
    if (_pclose(pipe) != 0) return NULL;
    return text.data;
}

static BOOL WINAPI os_ignore_ctrl(DWORD event) {
    (void)event;
    return TRUE;    // handled: the program in the same console gets it too
}

int os_run_wait(const char* program, char* const* argv) {
    OsText line = {0};
    for (size_t i = 0; argv[i]; i++) {
        if (i) os_putc(&line, ' ');
        os_put_argument(&line, argv[i]);
    }
    fflush(NULL);
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof startup;
    SetConsoleCtrlHandler(os_ignore_ctrl, TRUE);
    BOOL started = CreateProcessA(program, line.data, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    free(line.data);
    DWORD code = (DWORD)-1;
    if (started) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    } else {
        fprintf(stderr, "%s: cannot run it (error %lu)\n", program, (unsigned long)GetLastError());
    }
    SetConsoleCtrlHandler(os_ignore_ctrl, FALSE);
    return started ? (int)code : -1;
}

// What is written goes to a temporary file that Windows deletes when it is
// closed; os_capture_close reads it back first
FILE* os_capture_open(void) {
    for (int tries = 0; tries < 100; tries++) {
        char* path = os_temp_name("typedc-", ".txt");
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        free(path);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return NULL;
        }
        int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
        FILE* f = fd < 0 ? NULL : _fdopen(fd, "w+b");
        if (!f) CloseHandle(h);
        return f;
    }
    return NULL;
}

char* os_capture_close(FILE* f) {
    fflush(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* text = (char*)os_alloc((size_t)(size > 0 ? size : 0) + 1);
    size_t n = size > 0 ? fread(text, 1, (size_t)size, f) : 0;
    text[n] = '\0';
    fclose(f);
    return text;
}
