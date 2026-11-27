.PHONY: all clean test bench typedcc test-typedcc test-windows

SRC := src/main.c $(wildcard src/*.h src/os/*.h)
DEBUG := -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g


all: build/main

# typedc is main.c and the implementation of src/os/os.h for this system
build/main: build/main.o build/os_posix.o
	clang $^ -o $@ $(DEBUG)

build/main.o: $(SRC) | build
	clang -c $< -o $@ $(DEBUG)

build/os_posix.o: src/os/posix/os_posix.c src/os/os.h | build
	clang -c $< -o $@ $(DEBUG)

# Docker, for the Linux build of typedcc
LINUX_RUN := docker run --rm --platform linux/amd64 -v "$(CURDIR)":/src -w /src alpine:3.20

# typedcc: the self-contained compiler, with TinyCC and the C libraries built in
# (src/bundled.h, tools/build-typedcc.sh): native macOS arm64, and static x86_64 Linux
TYPEDCC_SRC := $(SRC) src/os/posix/os_posix.c src/tcc_glue.c src/typedcc_files.c tools/pack.c tools/build-typedcc.sh

typedcc: build/typedcc-macos-arm64 build/typedcc-linux-x86_64 build/typedcc-windows-x86_64.exe

build/typedcc-macos-arm64: $(TYPEDCC_SRC) | build
	sh tools/build-typedcc.sh macos $@

build/typedcc-linux-x86_64: $(TYPEDCC_SRC) build/glibc-2.31.tar | build
	$(LINUX_RUN) sh tools/build-typedcc.sh linux $@

build/typedcc-windows-x86_64.exe: $(TYPEDCC_SRC) src/os/windows/os_windows.c tools/build-typedcc-windows.sh build/windows-image.stamp
	$(WINDOWS_RUN) sh tools/build-typedcc.sh windows $@

build/glibc-2.31.tar: tools/glibc-files.sh | build
	docker run --rm --platform linux/amd64 -v "$(CURDIR)":/src -w /src ubuntu:20.04 sh tools/glibc-files.sh $@

# Runs the test suite with typedcc building the programs. Linux: in a system
# without a C compiler, linked against glibc and then statically against musl.
# Windows: under Wine
test-typedcc: typedcc
	TYPEDCC=build/typedcc-macos-arm64 sh tests/run.sh
	docker run --rm --platform linux/amd64 -v "$(CURDIR)":/src -w /src ubuntu:24.04 sh -c \
	  'TYPEDCC=build/typedcc-linux-x86_64 sh tests/run.sh && TYPEDCC="build/typedcc-linux-x86_64 -static" sh tests/run.sh'
	$(WINDOWS_RUN) sh -c 'RUN=wine EXE=.exe TYPEDCC=build/typedcc-windows-x86_64.exe sh tests/run.sh'

# Windows, without Windows: cross-compiled with MinGW and run with Wine, in a
# Docker image (tools/windows.Dockerfile) that also has clang for Windows
WINDOWS_RUN := docker run --rm --platform linux/amd64 -v "$(CURDIR)":/src -w /src typedc-windows

build/windows-image.stamp: tools/windows.Dockerfile | build
	docker build --platform linux/amd64 -t typedc-windows -f $< tools
	touch $@

build/main.exe: $(SRC) src/os/windows/os_windows.c build/windows-image.stamp
	$(WINDOWS_RUN) x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wpedantic src/main.c src/os/windows/os_windows.c -o $@

# Runs the test suite with typedc for Windows, and clang for Windows as its C compiler
test-windows: build/main.exe
	$(WINDOWS_RUN) sh -c 'RUN=wine EXE=.exe CC=clang sh tests/run.sh'

build:
	mkdir -p $@

test: all
	sh tests/run.sh

bench: all
	sh tests/bench.sh


clean:
	rm -f build/*
