#!/bin/sh
# Builds typedcc for Windows (x86_64), cross-compiled with MinGW, in the
# typedc-windows image (tools/windows.Dockerfile). Run from the repository root:
#   tools/build-typedcc.sh windows <output>
#
# TinyCC is built twice: once here, as a compiler for Windows programs that
# runs on Linux, only to compile TinyCC's runtime library (libtcc1.a) for
# Windows; and once for Windows itself, as the libtcc.a inside typedcc.
# Programs use msvcrt.dll, so no C library is bundled, only TinyCC's own
# headers (with its Windows API headers) and the .def files of the system DLLs.
set -e
out=$1
root=$PWD
work=/tmp/typedcc-windows
mkdir -p "$work/files/tcc/include" "$work/files/tcc/lib"

# 1. TinyCC for Windows programs, running here: libtcc1.a, and tccdefs_.h
#    (TinyCC's predefined macros as a C string, the same for every target)
cp -R third_party/tinycc "$work/cross"
cd "$work/cross"
./configure >/dev/null
make -j8 cross-x86_64-win32 >/dev/null

# 2. TinyCC running on Windows, as a library. Its build would make tccdefs_.h
#    with a helper it compiles for Windows, which cannot run here: the one of
#    step 1 is kept (make -o)
cp -R "$root/third_party/tinycc" "$work/tcc"
cd "$work/tcc"
./configure --cross-prefix=x86_64-w64-mingw32- --targetos=WIN32 --cpu=x86_64 >/dev/null
cp "$work/cross/tccdefs_.h" .
make -j8 -o tccdefs_.h libtcc.a >/dev/null

# 3. The files TinyCC needs at run time, laid out as its {B} directory on Windows
cp -R win32/include/. "$work/files/tcc/include/"
cp include/*.h "$work/files/tcc/include/"
cp win32/lib/*.def "$work/files/tcc/lib/"
cp "$work/cross/x86_64-win32-libtcc1.a" "$work/files/tcc/lib/libtcc1.a"

cd "$root"
gcc -O2 tools/pack.c -o "$work/pack"
"$work/pack" "$work/files.pack" "tcc=$work/files/tcc"
x86_64-w64-mingw32-gcc -O2 -DTYPEDC_BUNDLED -DTYPEDCC_FILES="\"$work/files.pack\"" -I"$work/tcc" \
    src/main.c src/os/windows/os_windows.c src/tcc_glue.c src/typedcc_files.c "$work/tcc/libtcc.a" -o "$out"
