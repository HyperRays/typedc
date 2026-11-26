#!/bin/sh
# Builds typedcc, the self-contained Typed C compiler: typedc with TinyCC
# (third_party/tinycc) built in, and the files TinyCC needs at run time packed
# into the binary (see src/bundled.h). Run from the repository root:
#   tools/build-typedcc.sh macos <output>   on macOS
#   tools/build-typedcc.sh linux <output>   in alpine (see the Makefile), after
#                                           build/glibc-2.31.tar (tools/glibc-files.sh)
#   tools/build-typedcc.sh windows <output> in the typedc-windows image (tools/windows.Dockerfile),
#                                           cross-compiled with MinGW
# Uses the C compiler $CC, else cc (Linux, macOS).
set -e
os=$1
out=$2
root=$PWD
cc=${CC:-cc}

case $os in
linux)
    apk add --no-cache build-base linux-headers tar >/dev/null
    work=/tmp/typedcc   # in the container: the kernel headers need a case-sensitive file system
    ;;
macos)
    work=$root/build/typedcc-macos
    rm -rf build/typedcc-macos
    ;;
windows)
    exec sh "$root/tools/build-typedcc-windows.sh" "$out"
    ;;
*)
    echo "usage: tools/build-typedcc.sh macos|linux|windows <output>" >&2
    exit 2
    ;;
esac
mkdir -p "$work/files/tcc/include"
cp -R third_party/tinycc "$work/tcc"

# TinyCC, finding everything below its {B} directory: where typedcc unpacks the files.
# Its build compiles its runtime library with itself, so {B}/libc (Linux) or
# {B}/sdk (macOS) is this system's for the build.
cd "$work/tcc"
if [ "$os" = linux ]; then
    ./configure --sysincludepaths='{B}/include:{B}/libc/include' --libpaths='{B}:{B}/libc/lib' \
        --crtprefix='{B}/libc/lib' --elfinterp=/lib64/ld-linux-x86-64.so.2 --config-bcheck=no >/dev/null
    mkdir libc
    ln -s /usr/include libc/include
    ln -s /usr/lib libc/lib
else
    ./configure --sysincludepaths='{B}/include:{B}/sdk/usr/include' --libpaths='{B}:{B}/sdk/usr/lib' \
        --config-bcheck=no >/dev/null
    ln -s "$(xcrun --show-sdk-path)" sdk
fi
make -j8 >/dev/null
cp include/*.h "$work/files/tcc/include/"
cp libtcc1.a "$work/files/tcc/"
packs="tcc=$work/files/tcc"

if [ "$os" = linux ]; then
    # glibc, for dynamic linking
    mkdir -p "$work/files/glibc"
    tar -C "$work/files/glibc" -xf "$root/build/glibc-2.31.tar"
    # musl, for -static: this system's headers (with the kernel headers) and static libraries
    mkdir -p "$work/files/musl/lib"
    apk info -qL musl-dev linux-headers | grep '^usr/include/' | tar -C / -cf - -T - | tar -C "$work/files/musl" -xf -
    for f in crt1.o crti.o crtn.o libc.a libm.a libpthread.a libdl.a librt.a libutil.a libxnet.a libresolv.a libcrypt.a; do
        if [ -e /usr/lib/$f ]; then cp /usr/lib/$f "$work/files/musl/lib/"; fi
    done
    packs="$packs glibc=$work/files/glibc musl/include=$work/files/musl/usr/include musl/lib=$work/files/musl/lib"
fi

cd "$root"
$cc -O2 tools/pack.c -o "$work/pack"
"$work/pack" "$work/files.pack" $packs

flags=
if [ "$os" = linux ]; then flags=-static; fi
$cc -O2 -DTYPEDC_BUNDLED -DTYPEDCC_FILES="\"$work/files.pack\"" -I"$work/tcc" \
    src/main.c src/os/posix/os_posix.c src/tcc_glue.c src/typedcc_files.c "$work/tcc/libtcc.a" $flags -lm -o "$out"
