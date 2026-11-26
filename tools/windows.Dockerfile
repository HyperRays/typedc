# A Windows test environment for typedc, without Windows: Windows programs are
# cross-compiled with MinGW and run with Wine, in an x86_64 Linux container.
#
#   docker build --platform linux/amd64 -t typedc-windows -f tools/windows.Dockerfile tools
#   docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src typedc-windows <command>
#
# Inside:
#   x86_64-w64-mingw32-gcc   cross-compiles a Windows .exe on the Linux side
#   wine <program.exe>       runs it
#   gcc                      builds tools that run here (typedcc's build)
#   C:\llvm-mingw\bin        clang for Windows (llvm-mingw), on Wine's PATH, so a
#                            typedc.exe running under Wine finds a C compiler (`clang`)
FROM ubuntu:24.04

ARG LLVM_MINGW=20261117

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        gcc-mingw-w64-x86-64 gcc libc6-dev wine wine64 ca-certificates curl unzip make \
 && rm -rf /var/lib/apt/lists/*

ENV WINEDEBUG=-all WINEPREFIX=/wine
RUN wineboot --init 2>/dev/null; wineserver -w

# clang for Windows, unpacked into Wine's C: drive and put on Wine's PATH
RUN curl -fsSL -o /tmp/llvm-mingw.zip \
        https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW}/llvm-mingw-${LLVM_MINGW}-ucrt-x86_64.zip \
 && unzip -q /tmp/llvm-mingw.zip -d /tmp \
 && mv /tmp/llvm-mingw-${LLVM_MINGW}-ucrt-x86_64 /wine/drive_c/llvm-mingw \
 && rm /tmp/llvm-mingw.zip \
 && wine reg add 'HKCU\Environment' /v PATH /t REG_EXPAND_SZ /d 'C:\llvm-mingw\bin' /f 2>/dev/null; wineserver -w
