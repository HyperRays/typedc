.PHONY: all clean

all: build/main

build/main: build/main.o
	clang $< -o $@ -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g

build/main.o: src/main.c $(wildcard src/*.h) | build
	clang -c $< -o $@ -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g

build:
	mkdir -p $@

clean:
	rm -f build/*
