#!/bin/sh
# Tests of typedc. Run from the repository root, after `make`.
#   tests/comptime/*.tc, tests/comptime/*/main.tc and examples/*.tc are compiled and run;
#                               their output must be the lines that follow `//> `
#   tests/comptime/errors/*.tc  must be rejected, with the message that follows
#                               `// expect: ` on their first line
# Either way, typedc must not report undefined behaviour: `make` builds it with
# the sanitizers, which print "runtime error:" and let it carry on.
# With TYPEDCC set (a typedcc binary, with any flags), typedcc builds the
# programs instead of typedc and cc, and checks the errors.
# For another system's programs (make test-windows):
#   RUN   runs typedc, typedcc, the C compiler and the programs, e.g. `wine`
#   EXE   what executables are named with, e.g. `.exe`
#   CC    the C compiler that compiles typedc's output (default: cc)
typedc=./build/main$EXE
dir=$(dirname "$0")/comptime
out=$(mktemp -d)
failed=0

for f in "$dir"/*.tc "$dir"/*/main.tc examples/*.tc; do
    ts=$(date +%s%N)
    name=$(basename "$(dirname "$f")")_$(basename "$f" .tc)
    # examples/toml.tc imports `std`, which is found from the repository root
    case "$f" in
        examples/toml.tc) set -- --root . ;;
        *) set -- ;;
    esac
    if [ -n "$TYPEDCC" ]; then
        # TinyCC has no complex numbers
        case "$f" in
            */complex.tc) echo "skip $name: TinyCC has no complex numbers"; continue ;;
        esac
        if ! $RUN $TYPEDCC "$@" "$f" -o "$out/$name$EXE" 2> "$out/$name.err"; then
            echo "FAIL $name: rejected"; cat "$out/$name.err"; failed=1; continue
        fi
    else
        if ! $RUN "$typedc" "$@" "$f" > "$out/$name.c" 2> "$out/$name.err"; then
            echo "FAIL $name: rejected"; cat "$out/$name.err"; failed=1; continue
        fi
        if grep -q 'runtime error:' "$out/$name.err"; then
            echo "FAIL $name: undefined behaviour in typedc"; cat "$out/$name.err"; failed=1; continue
        fi
        # the generated C includes the headers of the source file, which are found next to it
        if ! $RUN ${CC:-cc} -std=c11 -w -I"$(dirname "$f")" -x c "$out/$name.c" -o "$out/$name$EXE"; then
            echo "FAIL $name: the generated C does not compile"; failed=1; continue
        fi
    fi
    grep '^//> ' "$f" | sed 's|^//> ||' > "$out/$name.want"
    # programs on Windows end their lines with \r\n
    $RUN "$out/$name$EXE" | tr -d '\r' > "$out/$name.got"
    if [ -s "$out/$name.want" ] && ! diff "$out/$name.want" "$out/$name.got"; then
        echo "FAIL $name: wrong output"; failed=1; continue
    fi
    echo "ok   $name ($((($(date +%s%N) - $ts)/1000000))ms)"
done

for f in "$dir"/errors/*.tc; do
    ts=$(date +%s%N)
    name=$(basename "$f" .tc)
    want=$(head -1 "$f" | sed 's|^// expect: ||')
    if $RUN ${TYPEDCC:-"$typedc"} --check "$f" > /dev/null 2> "$out/$name.err"; then
        echo "FAIL $name: accepted"; failed=1
    elif grep -q 'runtime error:' "$out/$name.err"; then
        echo "FAIL $name: undefined behaviour in typedc"; cat "$out/$name.err"; failed=1
    elif ! grep -qF "$want" "$out/$name.err"; then
        echo "FAIL $name: expected '$want'"; cat "$out/$name.err"; failed=1
    else
        echo "ok   $name ($((($(date +%s%N) - $ts)/1000000))ms)"
    fi
done

rm -rf "$out"
exit $failed
