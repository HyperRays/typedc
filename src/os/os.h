#pragma once
// The operating system: everything typedc and typedcc need from it that
// standard C does not offer. Implemented once for POSIX (posix/os_posix.c: Linux,
// macOS) and once for Windows (windows/os_windows.c), each compiled on its own next to
// main.c; no other file of the compiler calls the system directly.
//
// Only plain C types cross this boundary, so the implementations need nothing
// of the compiler. Strings and arrays they return are allocated with malloc
// and never freed: the compiler is a short-lived process.
#include <stddef.h>
#include <stdio.h>

// What executables are named with: "" or ".exe"
extern const char os_exe_suffix[];

// Whether C compilers here lay out bit-fields by Microsoft's rule (Windows): a
// bit-field shares a storage unit only with bit-fields of a type of the same
// size, and the next member starts after the whole unit
extern const int os_ms_bitfields;

// --- paths and files --------------------------------------------------------

int os_is_dir(const char* path);
int os_exists(const char* path);

// Whether `c` separates directories in a path: '/' (POSIX); '/' and '\' (Windows)
int os_is_separator(char c);

// How many characters at the start of `path` are its root, which belongs to the
// directory part however short it is: "/" (POSIX); "C:\", "C:", "\" (Windows)
size_t os_root_length(const char* path);

// The names in a directory, without "." and "..", in no particular order.
// NULL if the directory cannot be read.
char** os_list_dir(const char* dir, size_t* count);

int os_make_dir(const char* path);          // 1 if it exists afterwards
void os_remove_tree(const char* path);      // a file, or a directory with everything in it
int os_set_executable(const char* path);    // 1 on success; nothing to do on Windows

// Creates a new file in the temporary directory, named <prefix><random><suffix>,
// with `contents`. Returns its path, or NULL.
char* os_temp_file(const char* prefix, const char* suffix, const char* contents);

// Creates a new directory in the temporary directory, named <prefix><random>.
// Returns its path, or NULL.
char* os_temp_dir(const char* prefix);

// POSIX only (the macOS SDK link of typedcc): creates a symbolic link
int os_symlink(const char* target, const char* link);

// --- programs ---------------------------------------------------------------

// Quotes one argument for a command line run by os_shell. NULL if it cannot be
// passed through the shell (Windows: it contains '%', which cmd.exe expands).
const char* os_quote(const char* arg);

// Runs a command line through the shell (sh, or cmd.exe on Windows) and returns
// what it writes to stdout, or NULL if it could not be run or failed. `quiet`
// throws away what it writes to stderr.
char* os_shell(const char* command, int quiet);

// Runs a program with these arguments (argv[0] is reported to it as its name),
// without a shell, and waits for it. Ctrl-C goes to the program; the caller
// keeps running. Returns its exit status, 128 + the signal number if a signal
// ended it (POSIX), or -1 if it could not be started.
int os_run_wait(const char* program, char* const* argv);

// A stream that collects what is written to it; os_capture_close returns that
// text, NUL-terminated, and closes the stream. One at a time.
FILE* os_capture_open(void);
char* os_capture_close(FILE* f);
