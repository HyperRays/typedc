#!/bin/sh
# Collects what typedcc needs to link programs dynamically against glibc 2.31:
# the headers (with the kernel headers), the startup objects and the libraries
# to link against. The programs then run with the glibc of the machine they run
# on, 2.31 or newer. Runs in ubuntu:20.04 (see the Makefile) and writes a tar of
#   include/   headers; the multiarch ones (x86_64-linux-gnu/) merged in
#   lib/       crt*.o, libc_nonshared.a and the shared libraries, with link
#              scripts that name the libraries without absolute paths
# A tar, because the kernel headers have names that differ only in case.
set -e
out=$1
apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends libc6-dev >/dev/null

g=/tmp/glibc
lib=/usr/lib/x86_64-linux-gnu
mkdir -p $g/include $g/lib
tar -C /usr/include --exclude=./x86_64-linux-gnu -cf - . | tar -C $g/include -xf -
cp -a /usr/include/x86_64-linux-gnu/. $g/include/

cp $lib/crt1.o $lib/crti.o $lib/crtn.o $lib/libc_nonshared.a $g/lib/
for f in libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 ld-linux-x86-64.so.2; do
    cp -L /lib/x86_64-linux-gnu/$f $g/lib/
done
echo 'GROUP ( libc.so.6 libc_nonshared.a AS_NEEDED ( ld-linux-x86-64.so.2 ) )' > $g/lib/libc.so
echo 'GROUP ( libm.so.6 )' > $g/lib/libm.so
ln -s libpthread.so.0 $g/lib/libpthread.so
ln -s libdl.so.2 $g/lib/libdl.so
ln -s librt.so.1 $g/lib/librt.so

tar -C $g -cf "$out" .
