CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
CSEMVER_VERSION ?= 0.1.0
UPSTREAM_COMPAT_VERSION ?= 13.2.1
VERSION_CPPFLAGS = -DCSEMVER_VERSION='"$(CSEMVER_VERSION)"' \
	-DCSEMVER_COMPAT_VERSION='"$(UPSTREAM_COMPAT_VERSION)"'
WARNINGS = -std=c17 -Wall -Wextra -Wpedantic -Werror

.PHONY: all test clean

all: build/csemver build/commit-and-tag-version

build/csemver: src/main.c src/release.c src/release.h src/common.c src/common.h src/version.c src/version.h src/config.c src/config.h src/semver.c src/semver.h src/toml.c src/toml.h | build
	$(CC) $(CPPFLAGS) $(VERSION_CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ src/main.c src/release.c src/common.c src/version.c src/config.c src/semver.c src/toml.c

build/commit-and-tag-version: build/csemver
	ln -sf csemver $@

build/csemver-version-test: src/main.c src/release.c src/release.h src/common.c src/common.h src/version.c src/version.h src/config.c src/config.h src/semver.c src/semver.h src/toml.c src/toml.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -DCSEMVER_VERSION='"9.8.7"' -o $@ src/main.c src/release.c src/common.c src/version.c src/config.c src/semver.c src/toml.c

build/test_config: tests/test_config.c src/config.c src/config.h src/toml.c src/toml.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_config.c src/config.c src/toml.c

build/test_toml: tests/test_toml.c src/toml.c src/toml.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_toml.c src/toml.c

build/test_version: tests/test_version.c src/version.c src/version.h src/common.c src/common.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_version.c src/version.c src/common.c

build/test_semver: tests/test_semver.c src/semver.c src/semver.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_semver.c src/semver.c

build:
	mkdir -p $@

test: build/csemver build/commit-and-tag-version build/csemver-version-test build/test_semver build/test_toml build/test_config build/test_version
	./build/test_semver
	./build/test_toml
	./build/test_config
	./build/test_version
	./tests/test_cli.sh ./build/csemver $(CSEMVER_VERSION)
	./tests/test_cli.sh ./build/csemver-version-test 9.8.7
	./tests/test_compat_alias.sh ./build/commit-and-tag-version $(UPSTREAM_COMPAT_VERSION)
	./tests/test_help.sh ./build/commit-and-tag-version
	./tests/test_release.sh

clean:
	rm -rf build
