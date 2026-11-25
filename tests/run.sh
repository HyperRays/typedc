#!/bin/sh
# Tests of typedc. Run from the repository root, after `make`.
#   tests/comptime/*.tc, tests/comptime/*/main.tc and examples/*.tc are compiled and run;
#                               their output must be the lines that follow `//> `
#   tests/comptime/errors/*.tc  must be rejected, with the message that follows
#                               `// expect: ` on their first line
# Either way, typedc must not report undefined behaviour: `make` builds it with
# the sanitizers, which print "runtime error:" and let it carry on.
dir=$(dirname "$0")/comptime
out=$(mktemp -d)
failed=0

for f in "$dir"/*.tc "$dir"/*/main.tc examples/*.tc; do
    name=$(basename "$(dirname "$f")")_$(basename "$f" .tc)
    # examples/toml.tc imports `std`, which is found from the repository root
    case "$f" in
        examples/toml.tc) set -- --root . ;;
        *) set -- ;;
    esac
    if ! ./build/main "$@" "$f" > "$out/$name.c" 2> "$out/$name.err"; then
        echo "FAIL $name: rejected"; cat "$out/$name.err"; failed=1; continue
    fi
    if grep -q 'runtime error:' "$out/$name.err"; then
        echo "FAIL $name: undefined behaviour in typedc"; cat "$out/$name.err"; failed=1; continue
    fi
    if ! cc -std=c11 -w -x c "$out/$name.c" -o "$out/$name"; then
        echo "FAIL $name: the generated C does not compile"; failed=1; continue
    fi
    grep '^//> ' "$f" | sed 's|^//> ||' > "$out/$name.want"
    "$out/$name" > "$out/$name.got"
    if [ -s "$out/$name.want" ] && ! diff "$out/$name.want" "$out/$name.got"; then
        echo "FAIL $name: wrong output"; failed=1; continue
    fi
    echo "ok   $name"
done

for f in "$dir"/errors/*.tc; do
    name=$(basename "$f" .tc)
    want=$(head -1 "$f" | sed 's|^// expect: ||')
    if ./build/main --check "$f" > /dev/null 2> "$out/$name.err"; then
        echo "FAIL $name: accepted"; failed=1
    elif grep -q 'runtime error:' "$out/$name.err"; then
        echo "FAIL $name: undefined behaviour in typedc"; cat "$out/$name.err"; failed=1
    elif ! grep -qF "$want" "$out/$name.err"; then
        echo "FAIL $name: expected '$want'"; cat "$out/$name.err"; failed=1
    else
        echo "ok   $name"
    fi
done

rm -rf "$out"
exit $failed
