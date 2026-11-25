#!/bin/sh
# Benchmarks of typedc itself: how long it takes to translate Typed C into C.
# Run from the repository root. Needs perl for timing.
#
#   sh tests/bench.sh                     default: 5 runs, sizes 50 100 200 400
#   RUNS=10 SIZES="250 500 1000" sh tests/bench.sh
#
# It builds an optimized compiler at build/typedc-bench, because `make` builds
# with sanitizers, which would make every number slower than it really is.
#
#   examples   milliseconds per translation of each file in examples/
#   scaling    generated programs of growing size, one per kind of work:
#                functions   plain C functions
#                generics    generic struct and function, one instance per type
#                const       const generic function, one instance per value
#                comptime    a compile-time loop of size * 1000 passes
#              (xN) is the time against the size before it. Doubling the size
#              should about double the time; much more than that points at
#              work that grows faster than the program does.
#
# Every run also calls the C preprocessor (`$CC -E`) to read the C headers, so
# a small program mostly measures that and process startup.

runs=${RUNS:-5}
sizes=${SIZES:-"50 100 200 400"}
bin=build/typedc-bench
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

command -v perl > /dev/null || { echo "bench.sh needs perl for timing" >&2; exit 1; }

mkdir -p build
${CC:-cc} -O2 src/main.c -o "$bin" || exit 1

# bench <command...>: average milliseconds of one run, over $runs runs.
# A first run, with its errors shown, makes sure the command works.
bench() {
    "$@" > /dev/null || { echo "failed: $*" >&2; exit 1; }
    perl -MTime::HiRes=time -e '
        my $n = shift;
        open(my $out, ">&", \*STDOUT);
        open(STDOUT, ">", "/dev/null");
        my $start = time;
        for (1 .. $n) { system(@ARGV) == 0 or die "failed: @ARGV\n" }
        printf $out "%.1f", (time - $start) * 1000 / $n;
    ' "$runs" "$@" || exit 1
}

echo "typedc benchmarks: ms per run, average of $runs runs"
echo
echo "examples"
for f in examples/*.tc; do
    # examples/toml.tc imports `std`, which is found from the repository root
    case "$f" in
        examples/toml.tc) set -- --root . ;;
        *) set -- ;;
    esac
    ms=$(bench "$bin" "$@" "$f") || exit 1
    printf "  %-22s %8s\n" "$(basename "$f" .tc)" "$ms"
done

# gen_<kind> <size>: write a program of that size to stdout

gen_functions() {
    awk -v n="$1" 'BEGIN {
        print "#include <stdio.h>"
        print "int f0(int x) { return x + 1; }"
        for (i = 1; i < n; i++)
            printf "int f%d(int x) { int y = f%d(x); return y %% 1000 * 2 + %d; }\n", i, i - 1, i
        printf "int main(void) { printf(\"%%d\\n\", f%d(1)); return 0; }\n", n - 1
    }'
}

gen_generics() {
    awk -v n="$1" 'BEGIN {
        print "#include <stdio.h>"
        print "typedef<T> struct { T value; int count; } Box<T>;"
        print "T unbox<T>(Box<T> b) { return b.value; }"
        for (i = 0; i < n; i++) printf "typedef struct { int v; } S%d;\n", i
        print "int main(void) {"
        print "    int total = 0;"
        for (i = 0; i < n; i++) {
            printf "    Box<S%d> b%d = {{%d}, 0};\n", i, i, i
            printf "    total += unbox(b%d).v;\n", i
        }
        print "    printf(\"%d\\n\", total);"
        print "    return 0;"
        print "}"
    }'
}

gen_const() {
    awk -v n="$1" 'BEGIN {
        print "#include <stdio.h>"
        print "int scaled<int N>(int x) { return x * N; }"
        print "int main(void) {"
        print "    int total = 0;"
        for (i = 0; i < n; i++) printf "    total += scaled<%d>(1);\n", i
        print "    printf(\"%d\\n\", total);"
        print "    return 0;"
        print "}"
    }'
}

gen_comptime() {
    awk -v n="$1" 'BEGIN {
        print "#include <stdio.h>"
        print "int @sum(int n) { int s = 0; for (int i = 0; i < n; i++) s = (s + i) % 1000; return s; }"
        printf "int main(void) { printf(\"%%d\\n\", @sum(%d)); return 0; }\n", n * 1000
    }'
}

kinds="functions generics const comptime"
echo
echo "scaling"
printf "  %-6s" size
for k in $kinds; do printf " %16s" "$k"; done
echo
for k in $kinds; do eval "prev_$k="; done
for n in $sizes; do
    printf "  %-6s" "$n"
    for k in $kinds; do
        "gen_$k" "$n" > "$tmp/$k.tc"
        ms=$(bench "$bin" "$tmp/$k.tc") || exit 1
        eval "prev=\$prev_$k"
        if [ -n "$prev" ]; then
            cell=$(awk -v a="$ms" -v b="$prev" 'BEGIN { printf "%s (x%.1f)", a, (b > 0 ? a / b : 0) }')
        else
            cell=$ms
        fi
        printf " %16s" "$cell"
        eval "prev_$k=\$ms"
    done
    echo
done
