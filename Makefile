CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -std=c17 -Wall -Wextra -Wpedantic -Werror

.PHONY: all test clean

all: build/csemver

build/csemver: src/main.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ $<

build:
	mkdir -p $@

test: build/csemver
	./tests/test_cli.sh ./build/csemver

clean:
	rm -rf build
