# csemver Makefile
# C17 build, test gate, install and self-release, with colorized output

# ============== Colors & Symbols ==============
# Color only when the terminal is interactive and NO_COLOR is unset.
ifeq ($(NO_COLOR),)
COLOR := $(shell [ -t 2 ] && echo 1)
endif

ifeq ($(COLOR),1)
AMBER   := \033[38;2;255;194;75m
GREEN   := \033[92m
CYAN    := \033[96m
YELLOW  := \033[93m
RED     := \033[91m
GRAY    := \033[90m
BOLD    := \033[1m
RESET   := \033[0m
endif

CHECK    := ✓
CROSS    := ✗
ARROW    := ▸
PROGRESS := →

# ============== Project Metadata ==============
CSEMVER_VERSION ?= $(shell cat VERSION)
BUILD           := build
PREFIX          ?= /usr/local
BINDIR          ?= $(PREFIX)/bin

# ============== Toolchain ==============
CC          ?= cc
CFLAGS      ?= -O2
CPPFLAGS    ?=
XML2_CFLAGS ?= $(shell pkg-config --cflags libxml-2.0 2>/dev/null || xml2-config --cflags)
XML2_LIBS   ?= $(shell pkg-config --libs libxml-2.0 2>/dev/null || xml2-config --libs)
YAML_CFLAGS ?= $(shell pkg-config --cflags yaml-0.1 2>/dev/null)
YAML_LIBS   ?= $(shell pkg-config --libs yaml-0.1 2>/dev/null || echo -lyaml)
WARNINGS    := -std=c17 -Wall -Wextra -Wpedantic -Werror
LIB_CFLAGS  := $(XML2_CFLAGS) $(YAML_CFLAGS)
LIB_LIBS    := $(XML2_LIBS) $(YAML_LIBS)

# V=1 prints the full compiler command lines.
ifeq ($(V),1)
Q :=
else
Q := @
endif

# ============== Sources ==============
MODULES := release common version maven config semver gitignore changelog lifecycle tty toml
SRCS    := src/main.c $(addprefix src/,$(addsuffix .c,$(MODULES)))
HDRS    := $(addprefix src/,$(addsuffix .h,$(MODULES)))

UNIT_TESTS := test_semver test_toml test_config test_version
UNIT_BINS  := $(addprefix $(BUILD)/,$(UNIT_TESTS))

# A fixed version for the second CLI build, proving --version embeds the build value.
PINNED_VERSION := 9.8.7

# ============== Phony Targets ==============
.PHONY: all build help banner check test test-unit test-cli test-release \
        install uninstall version release release-dry release-guard clean FORCE

# ============== Default Target ==============
.DEFAULT_GOAL := help

# ============== Banner ==============
# figlet art goes straight to the terminal: its backslashes never pass through printf.
# NO_BANNER=1 silences it in sub-makes, so one run draws it once.
banner:
ifeq ($(NO_BANNER),)
	@if command -v figlet >/dev/null 2>&1; then \
		figlet -f small csemver | { printf "$(AMBER)$(BOLD)"; cat; printf "$(RESET)"; }; \
	else \
		printf "$(AMBER)$(BOLD)csemver$(RESET)\n"; \
	fi
	@printf "  $(GRAY)v$(CSEMVER_VERSION)$(RESET) $(AMBER)native C17 release manager$(RESET)\n\n"
endif

# ============== Section Header ==============
# Titles stay ASCII: printf pads by bytes, so a multibyte glyph skews the box.
define section
	@printf "\n$(CYAN)$(BOLD)╔══════════════════════════════════════════════════╗$(RESET)\n"
	@printf "$(CYAN)$(BOLD)║ %-48s ║$(RESET)\n" "$(1)"
	@printf "$(CYAN)$(BOLD)╚══════════════════════════════════════════════════╝$(RESET)\n\n"
endef

# $(call step,label,command): print the step, run it, report a failure by name.
define step
	@printf "$(GRAY)  $(PROGRESS) %s$(RESET)\n" "$(1)"
	@$(2) || { printf "$(RED)$(CROSS) %s failed$(RESET)\n" "$(1)"; exit 1; }
endef

# ============== Help ==============
help: banner
	@printf "$(BOLD)Usage:$(RESET) make $(YELLOW)<target>$(RESET)\n\n"
	@printf "$(CYAN)$(BOLD)Build$(RESET)\n"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "build" "compile $(BUILD)/csemver (alias: all)"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "install" "install to $(BINDIR) (PREFIX, DESTDIR)"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "uninstall" "remove $(BINDIR)/csemver"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "clean" "remove $(BUILD)/"
	@printf "\n$(CYAN)$(BOLD)Test$(RESET)\n"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "check" "the gate: clean build plus the full suite"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "test" "the full suite on the current build"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "test-unit" "C unit tests: semver, toml, config, version"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "test-cli" "CLI parsing, --version, --help against README"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "test-release" "real releases in throwaway Git repositories"
	@printf "\n$(CYAN)$(BOLD)Release$(RESET)\n"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "version" "print the version"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "release-dry" "preview the next release of csemver itself"
	@printf "  $(GREEN)%-14s$(RESET) %s\n" "release" "gate, cut the release, gate the release commit"
	@printf "\n$(GRAY)  CC=$(CC)  CFLAGS=$(CFLAGS)  CSEMVER_VERSION=$(CSEMVER_VERSION)$(RESET)\n"
	@printf "$(GRAY)  RELEASE_AS=major|minor|patch|X.Y.Z overrides the bump; V=1 shows commands$(RESET)\n"

# ============== Build ==============
all: build

build: banner $(BUILD)/csemver
	@printf "$(GREEN)$(CHECK) $(BOLD)$(BUILD)/csemver$(RESET) $(GRAY)v$(CSEMVER_VERSION)$(RESET)\n"

# Records the embedded version; rewritten only when CSEMVER_VERSION changes, so a
# build for another version is never reused.
$(BUILD)/version.stamp: FORCE | $(BUILD)/.dir
	@printf '%s\n' '$(CSEMVER_VERSION)' | cmp -s - $@ || printf '%s\n' '$(CSEMVER_VERSION)' > $@

$(BUILD)/csemver: $(SRCS) $(HDRS) $(BUILD)/version.stamp | $(BUILD)/.dir
	@printf "$(GRAY)  $(PROGRESS) %s %s$(RESET)\n" "$(CC)" "$@"
	$(Q)$(CC) $(CPPFLAGS) -DCSEMVER_VERSION='"$(CSEMVER_VERSION)"' $(LIB_CFLAGS) $(CFLAGS) $(WARNINGS) -o $@ $(SRCS) $(LIB_LIBS)

$(BUILD)/csemver-version-test: $(SRCS) $(HDRS) | $(BUILD)/.dir
	@printf "$(GRAY)  $(PROGRESS) %s %s$(RESET)\n" "$(CC)" "$@"
	$(Q)$(CC) $(CPPFLAGS) -DCSEMVER_VERSION='"$(PINNED_VERSION)"' $(LIB_CFLAGS) $(CFLAGS) $(WARNINGS) -o $@ $(SRCS) $(LIB_LIBS)

$(BUILD)/test_semver: tests/test_semver.c src/semver.c src/semver.h | $(BUILD)/.dir
	@printf "$(GRAY)  $(PROGRESS) %s %s$(RESET)\n" "$(CC)" "$@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_semver.c src/semver.c

$(BUILD)/test_toml: tests/test_toml.c src/toml.c src/toml.h | $(BUILD)/.dir
	@printf "$(GRAY)  $(PROGRESS) %s %s$(RESET)\n" "$(CC)" "$@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_toml.c src/toml.c

$(BUILD)/test_config: tests/test_config.c src/config.c src/config.h src/toml.c src/toml.h | $(BUILD)/.dir
	@printf "$(GRAY)  $(PROGRESS) %s %s$(RESET)\n" "$(CC)" "$@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_config.c src/config.c src/toml.c

$(BUILD)/test_version: tests/test_version.c src/version.c src/version.h src/common.c src/common.h src/maven.c src/maven.h | $(BUILD)/.dir
	@printf "$(GRAY)  $(PROGRESS) %s %s$(RESET)\n" "$(CC)" "$@"
	$(Q)$(CC) $(CPPFLAGS) $(LIB_CFLAGS) $(CFLAGS) $(WARNINGS) -o $@ tests/test_version.c src/version.c src/common.c src/maven.c $(LIB_LIBS)

# The directory is reached through a marker file: `build` is also a phony target.
$(BUILD)/.dir:
	@mkdir -p $(BUILD) && touch $@

FORCE:

# ============== Tests ==============
check: banner
	$(call section,Gate: clean build and full suite)
	@rm -rf $(BUILD)
	@$(MAKE) --no-print-directory test-unit test-cli test-release NO_BANNER=1
	@printf "\n$(GREEN)$(BOLD)$(CHECK) gate passed$(RESET) $(GRAY)v$(CSEMVER_VERSION)$(RESET)\n"

test: banner test-unit test-cli test-release
	@printf "\n$(GREEN)$(BOLD)$(CHECK) all tests passed$(RESET)\n"

test-unit: banner $(UNIT_BINS)
	$(call section,Unit Tests)
	$(call step,semver,./$(BUILD)/test_semver)
	$(call step,toml,./$(BUILD)/test_toml)
	$(call step,config,./$(BUILD)/test_config)
	$(call step,version files,./$(BUILD)/test_version)
	@printf "$(GREEN)$(CHECK) unit tests passed$(RESET)\n"

test-cli: banner $(BUILD)/csemver $(BUILD)/csemver-version-test
	$(call section,CLI)
	$(call step,cli v$(CSEMVER_VERSION),./tests/test_cli.sh ./$(BUILD)/csemver $(CSEMVER_VERSION))
	$(call step,cli v$(PINNED_VERSION),./tests/test_cli.sh ./$(BUILD)/csemver-version-test $(PINNED_VERSION))
	$(call step,help matches README,./tests/test_help.sh ./$(BUILD)/csemver)
	@printf "$(GREEN)$(CHECK) CLI tests passed$(RESET)\n"

test-release: banner $(BUILD)/csemver
	$(call section,Release Transactions)
	$(call step,releases in throwaway repositories,./tests/test_release.sh)
	@printf "$(GREEN)$(CHECK) release tests passed$(RESET)\n"

# ============== Install ==============
install: banner $(BUILD)/csemver
	$(call section,Installing)
	@install -d $(DESTDIR)$(BINDIR)
	@install -m 0755 $(BUILD)/csemver $(DESTDIR)$(BINDIR)/csemver
	@printf "$(GREEN)$(CHECK) $(DESTDIR)$(BINDIR)/csemver$(RESET) $(GRAY)v$(CSEMVER_VERSION)$(RESET)\n"

uninstall: banner
	$(call section,Uninstalling)
	@rm -f $(DESTDIR)$(BINDIR)/csemver
	@printf "$(GREEN)$(CHECK) removed $(DESTDIR)$(BINDIR)/csemver$(RESET)\n"

# ============== Release ==============
version:
	@printf "$(CYAN)Version:$(RESET) $(YELLOW)$(BOLD)$(CSEMVER_VERSION)$(RESET)\n"

RELEASE_FLAGS := $(if $(RELEASE_AS),--release-as $(RELEASE_AS))

release-dry: banner $(BUILD)/csemver
	$(call section,Release Preview)
	@./$(BUILD)/csemver --dry-run $(RELEASE_FLAGS)

# A release starts from a committed tree: csemver commits the files it bumps.
release-guard:
	@git diff --quiet && git diff --cached --quiet || \
		{ printf "$(RED)$(CROSS) working tree has uncommitted changes$(RESET)\n"; exit 1; }

release: banner release-guard
	@$(MAKE) --no-print-directory check NO_BANNER=1
	$(call section,Release: $(or $(RELEASE_AS),bump from commits))
	@./$(BUILD)/csemver $(RELEASE_FLAGS)
	@$(MAKE) --no-print-directory check
	@printf "\n$(GREEN)$(BOLD)$(CHECK) released v$$(cat VERSION)$(RESET) $(GRAY)push master and the tag to origin and forgejo$(RESET)\n"

# ============== Clean ==============
clean: banner
	$(call section,Cleaning)
	@rm -rf $(BUILD)
	@printf "$(GREEN)$(CHECK) removed $(BUILD)/$(RESET)\n"
