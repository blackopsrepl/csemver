CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -std=c17 -Wall -Wextra -Wpedantic -Werror

.PHONY: all test clean

all: build/csemver

build/csemver: src/main.c src/config.c src/config.h src/semver.c src/semver.h vendor/tomlc99/toml.c vendor/tomlc99/toml.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ src/main.c src/config.c src/semver.c vendor/tomlc99/toml.c

build/test_config: tests/test_config.c src/config.c src/config.h vendor/tomlc99/toml.c vendor/tomlc99/toml.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_config.c src/config.c vendor/tomlc99/toml.c

build/test_toml: tests/test_toml.c vendor/tomlc99/toml.c vendor/tomlc99/toml.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_toml.c vendor/tomlc99/toml.c

build/test_version: tests/test_version.c src/version.c src/version.h src/common.c src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_version.c src/version.c src/common.c

build/test_semver: tests/test_semver.c src/semver.c src/semver.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_semver.c src/semver.c

build:
	mkdir -p $@

test: build/csemver build/test_semver build/test_toml build/test_config build/test_version
	./build/test_semver
	./build/test_toml
	./build/test_config
	./build/test_version
	./tests/test_cli.sh ./build/csemver

clean:
	rm -rf build
