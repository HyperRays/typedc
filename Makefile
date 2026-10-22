.PHONY: all clean

all: build/main

build/main: build/main.o
	clang $< -o $@

build/main.o: src/main.c src/lexer.h | build
	clang -c $< -o $@

build:
	mkdir -p $@

clean:
	rm -f build/*
