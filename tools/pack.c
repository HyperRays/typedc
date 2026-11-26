// Packs directory trees into the one file that typedcc embeds (see src/bundled.h).
//
//   pack <output> <prefix>=<dir>...
//
// Every file below <dir> becomes a record named <prefix>/<path below dir>:
//   name, NUL byte
//   size, 8 bytes little endian
//   executable flag, 1 byte
//   contents
// An empty name ends the pack. Symbolic links are followed.
#define _XOPEN_SOURCE 700
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static FILE* out;
static const char* prefix;
static size_t prefix_len;
static size_t root_len;

static int add(const char* path, const struct stat* st, int type, struct FTW* ftw) {
    (void)ftw;
    if (type == FTW_D || type == FTW_DP) return 0;
    if (type != FTW_F) {
        fprintf(stderr, "pack: cannot read %s\n", path);
        return 1;
    }
    FILE* in = fopen(path, "rb");
    if (!in) {
        perror(path);
        return 1;
    }
    fwrite(prefix, 1, prefix_len, out);
    fputs(path + root_len, out);
    fputc(0, out);
    uint64_t size = (uint64_t)st->st_size;
    for (int i = 0; i < 8; i++) fputc((int)(size >> (8 * i)) & 0xff, out);
    fputc(st->st_mode & 0111 ? 1 : 0, out);
    char chunk[65536];
    size_t n, total = 0;
    while ((n = fread(chunk, 1, sizeof chunk, in)) > 0) {
        fwrite(chunk, 1, n, out);
        total += n;
    }
    fclose(in);
    if (total != size) {
        fprintf(stderr, "pack: %s changed while it was read\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pack <output> <prefix>=<dir>...\n");
        return 2;
    }
    out = fopen(argv[1], "wb");
    if (!out) {
        perror(argv[1]);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (!eq) {
            fprintf(stderr, "pack: '%s' is not <prefix>=<dir>\n", argv[i]);
            return 2;
        }
        *eq = '\0';
        prefix = argv[i];
        prefix_len = strlen(prefix);
        const char* dir = eq + 1;
        root_len = strlen(dir);
        while (root_len > 1 && dir[root_len - 1] == '/') root_len--;
        // the record name is <prefix> followed by the path below <dir>, which starts with '/'
        if (nftw(dir, add, 32, 0) != 0) {
            fprintf(stderr, "pack: packing %s failed\n", dir);
            return 1;
        }
    }
    fputc(0, out);
    if (fclose(out) != 0) {
        perror(argv[1]);
        return 1;
    }
    return 0;
}
