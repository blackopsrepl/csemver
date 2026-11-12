# Repository Guidelines

Guidance for coding agents working in this repository.

## Product Contract

- csemver is an independent release tool written in C17. It is not a port, a
  drop-in replacement, or a compatibility layer for any other tool. It began
  as a reimplementation of commit-and-tag-version 12.5.0 (see *Origin* in
  `README.md`); that history explains some defaults and nothing else.
- **The contract is this repository**: `README.md` describes the behavior and
  the test suite pins it. No other implementation's output defines what
  csemver should do.
- Configuration is `csemver.toml` (or `-c FILE`), TOML only. csemver never
  executes configuration or updater code; version fields in arbitrary formats
  use the declarative `regex` updater. Lifecycle `scripts` are the one place
  shell commands run, and only from the user's own config.
- csemver never pushes. A release is local: version files, changelog, release
  commit, annotated tag.
- Real repositories release through this binary: their Makefiles invoke
  `csemver`, and some release workflows parse the changelog headings it writes.
  Treat changelog layout, tag names, file mutations, and exit status as
  user-facing output.

## Changing Behavior

- Decide a change on its merits: correctness, clarity, and what users'
  repositories need. Do not install or run another release tool to decide what
  csemver should do, do not add fixtures captured from one, and do not justify a
  change as "matching upstream".
- Every behavior change is deliberate and visible. Write the failing test first,
  then the code, and commit it with a type that reaches the changelog and the
  bump: `fix:` or `feat:`, and `!` plus a `BREAKING CHANGE:` footer when it
  changes output or options that scripts rely on. A behavior change inside a
  `refactor:` or `chore:` commit ships unannounced and under-bumped.
- Comments and diagnostics state the rule ("an issue id ends in a digit"), never
  where it came from. New diagnostics go through `errorf`, which adds the
  `csemver: ` prefix.
- `csemver --help` and the `## CLI options` block in `README.md` are the same
  text; `tests/test_help.sh` fails when they differ. Change both together.

## Inherited Behavior to Revisit

These came from the original reimplementation and are kept until changed on
purpose. Each fix is its own visible commit with a regression test.

- `--release-as` with a case variant (`Major`) passes validation, selects no
  bump, and writes the literal `null` into the version files, exiting 0.
- A plain-text version file with a trailing newline keeps the newline in the
  version it reads: the progress line breaks (`from 1.0.0` / ` to 1.0.1`), and
  with `--skip.bump` or `--first-release` the tag name carries the newline, so
  `git tag` fails after the release commit already exists.
- `--commit-all` runs `git add` with no pathspec (Git prints hints on stderr),
  and its progress line prints a literal `%s`.
- `--types` on the command line clears every commit type instead of defining
  any; custom types work only from `csemver.toml`.
- Unknown options and positional arguments are silently ignored, so a typo in a
  flag goes unnoticed.
- A `regex` updater whose pattern does not match prints a warning and the
  release continues with exit status 0.
- Malformed JSON diagnostics reproduce the wording of V8's `JSON.parse`
  (`Unexpected token '}', ...`).

## Validation

- Gate: `make clean test`. Plain `make test` can reuse a binary built for
  another `CSEMVER_VERSION` and fail the version check for the wrong reason.
- The build is `-std=c17 -Wall -Wextra -Wpedantic -Werror`. When removing code,
  delete the entry point and rebuild: the compiler names every helper that
  became unused. Then search `tests/` for the same symbols.
- Test scripts run under `/bin/sh`, which is dash on the Ubuntu CI runner. No
  bashisms (`$'...'`, arrays, `[[`); build an escape byte with
  `esc=$(printf '\033')`. Reproduce shell-test changes in `ubuntu:24.04` under
  Podman before pushing, because a local bash `/bin/sh` cannot show the failure.
- Edit test fixtures by hand, one block at a time, and run the suite after each
  batch. Do not rewrite `tests/` with `sed` or a script: a mechanical rewrite
  yields tests that pass while no longer asserting what their names claim.
- CI runs on GitHub Actions (Ubuntu 24.04, macOS 15 Intel) and on Forgejo
  (the `cpp-latest` runner, openSUSE packages).
  Keep the README prerequisites and every workflow's install step in agreement.

## Layout

- `src/release.c`: CLI parsing, help, configuration load, the release
  transaction.
- `src/changelog.c`: commit classification and changelog rendering.
- `src/version.c`, `src/maven.c`: version-file readers and updaters.
- `src/config.c`: the `csemver.toml` schema and defaults.
- `src/semver.c`: SemVer parsing and bumping.
- `src/gitignore.c`: the ignore-pattern matcher.
- `src/lifecycle.c`: lifecycle scripts. `src/tty.c`: color and progress output.
- `src/common.c`: buffers and process capture. `src/toml.c`: embedded tomlc99.
- `tests/`: C unit tests (`test_*.c`) and shell suites (`test_*.sh`);
  `test_release.sh` drives real releases in throwaway Git repositories.

## Releasing

csemver releases itself from its own `csemver.toml`:

1. On a clean, synced `master`, run `make clean test`.
2. `./build/csemver --dry-run`: read the computed version and the rendered
   section against the commits it covers.
3. `./build/csemver`, then `make clean test` again on the release commit.
4. Push `master` and the tag to both remotes (`origin` on GitHub, `forgejo`).
   The tag starts the GitHub release workflow, which checks that the tag,
   `VERSION`, and the release commit agree.

One tag per coherent iteration.
