CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -std=c17 -Wall -Wextra -Wpedantic -Werror

.PHONY: all test clean

all: build/csemver

build/csemver: src/main.c src/semver.c src/semver.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ src/main.c src/semver.c

build/test_semver: tests/test_semver.c src/semver.c src/semver.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_semver.c src/semver.c

build:
	mkdir -p $@

test: build/csemver build/test_semver
	./build/test_semver
	./tests/test_cli.sh ./build/csemver

clean:
	rm -rf build
