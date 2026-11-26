// typedcc: the packed files TinyCC needs at run time (tools/pack.c), built into
// the binary. TYPEDCC_FILES is the path of the pack, as a string literal.
#ifndef TYPEDCC_FILES
#error "build with -DTYPEDCC_FILES='\"<path of the pack>\"' (tools/build-typedcc.sh)"
#endif

#ifdef __APPLE__
#define SYM(name) "_" #name
#define SECTION ".section __TEXT,__const\n"
#elif defined _WIN32
#define SYM(name) #name
#define SECTION ".section .rdata,\"dr\"\n"
#else
#define SYM(name) #name
#define SECTION ".section .rodata\n"
#endif

__asm__(SECTION
        ".globl " SYM(typedcc_files) "\n"
        ".p2align 4\n"
        SYM(typedcc_files) ":\n"
        ".incbin \"" TYPEDCC_FILES "\"\n"
        ".globl " SYM(typedcc_files_end) "\n"
        SYM(typedcc_files_end) ":\n"
        ".byte 0\n"
        ".text\n");
