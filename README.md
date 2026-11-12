# csemver

<p align="center">
  <img src="docs/assets/csemver-mascot.png" alt="The csemver clockwork raven release steward" width="200">
</p>

**A native C17 release manager with TOML configuration.**

csemver cuts releases from Conventional Commits: it bumps the version files, writes the changelog, creates the release commit, and creates an annotated Git tag. It needs no scripting runtime and never executes its configuration. This README and the test suite define its behavior. Preview every release with `--dry-run`.

## Build and test

Requirements: a C17 compiler, GNU Make, Git, libxml2 and libyaml development files, and `pkg-config` (for example, `libxml2-dev libyaml-dev pkg-config` on Debian/Ubuntu). The TOML parser is compiled directly from `src/toml.c`.

The source is split by concern: `release.c` holds the CLI, configuration load, and the release transaction; `changelog.c` the commit classification and changelog rendering; `version.c` the version-file readers and updaters; `config.c` the `csemver.toml` schema; `gitignore.c` the ignore matcher; `semver.c` SemVer parsing and bumping; `lifecycle.c` the lifecycle hooks; `tty.c` color and progress output; `common.c` buffers and process capture; and `maven.c` the XML/Maven reader.

```sh
make build
make test
./build/csemver --version
```

`make` alone lists every target. `make check` is the release gate: a clean build plus the full suite. `make install` copies the binary to `PREFIX/bin` (`PREFIX=/usr/local` by default; `DESTDIR` is honored).

`csemver --version` prints its embedded build version.

`make test` exercises the SemVer and TOML code, version-file updates, CLI parsing, and release operations in isolated Git repositories. Set `TMPDIR` to a writable scratch directory if `/tmp` is unavailable.

## Quick start

From a Git working tree with conventional commits:

```sh
./build/csemver --dry-run
./build/csemver
git push --follow-tags origin master
```

The release command updates configured version files and the changelog, creates a release commit, then creates an annotated tag. It does **not** push; review the preview and push the branch and tag yourself. The first release can use `--first-release` to tag the version already in the package file without bumping it.

By default, `feat` selects a minor bump, `fix` selects a patch bump, and a Conventional Commit breaking change selects a major bump. Under the default Conventional Commits preset, versions below `1.0.0` automatically use pre-major rules: `feat` selects a patch bump and a breaking change selects a minor bump. `preMajor = true` also applies those rules explicitly, including with the Angular preset. With no qualifying commits, the default is a patch bump; set `noBumpWhenEmptyChanges = true` to leave the repository unchanged instead.

## TOML configuration

csemver reads `csemver.toml` from the current working directory; `-c FILE` selects another TOML file. Configuration is TOML only: `package.json` sections and `.versionrc` files are not read, and a JSON file passed to `-c` fails as invalid TOML. csemver never executes configuration or updater code, and a string `updater` path in a file entry is rejected before the release starts. Version fields in arbitrary formats can instead use the native `regex` updater below. Within TOML, precedence is the configuration file, then the command line. This repository uses a plain-text `VERSION` file as both its package-version source and bump target:

```toml
tagPrefix = "v"
releaseCommitMessageFormat = "chore(release): {{currentTag}}"
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
```

File lists may contain strings (the type is inferred from the file name) or tables with `filename` and `type`. Updater types are `json`, `plain-text`, `toml`, `python`, `yaml`, `openapi`, `maven`, `gradle`, `csproj`, and `regex`. Gradle files rewrite the matched declaration as `version = "VERSION"` and preserve the rest of the file. C# project files rewrite the matched `<Version>` element and preserve the rest of the file. Maven POM files use libxml2 to resolve direct or `${property}` versions and reserialize the document, keeping its newline convention. JSON package-lock files update only the root package version surfaces. JSON files are reserialized with their detected indentation and newline convention and end with a newline; Python and TOML updates preserve surrounding bytes. YAML and OpenAPI files use libyaml to select the root `version` or `info.version` mapping entry; scalar text and line endings are preserved, and flow maps are rewritten with normalized spacing. Plain-text version contents are read verbatim, surrounding whitespace (including Unicode spaces) is ignored for SemVer parsing, and updates replace the entire file contents with the new version.

`packageFiles` selects where csemver reads the current version; custom entries are not implicitly added to `bumpFiles`. Declare each file to update in `bumpFiles`. For version fields in files without a built-in updater, configure `type = "regex"` in `csemver.toml` with a POSIX extended regular expression (POSIX ERE, not the ECMAScript dialect) and the capture group containing the version. `versionGroup` defaults to `1`; group `0` selects the full match. The matcher uses line-aware anchors and updates only the selected capture, preserving all surrounding bytes. It requires no runtime from the target repository and never executes configuration or updater code. The Angular changelog preset is a commit/changelog format, not an Angular-framework integration.

```toml
packageFiles = [
  { filename = "lib/release.rb", type = "regex", pattern = '^(  VERSION = ")([^"]+)(")$', versionGroup = 2 },
]
bumpFiles = [
  { filename = "lib/release.rb", type = "regex", pattern = '^(  VERSION = ")([^"]+)(")$', versionGroup = 2 },
  { filename = "README.md", type = "regex", pattern = '^(## Current release: `v)([^`]+)(`)$', versionGroup = 2 },
]
```

Other supported keys include `preset`, `infile`, `header`, `releaseAs`, `prerelease`, `releaseCount`, `tagPrefix`, `issuePrefixes`, `path`, `lernaPackage`, `preMajor`, `noBumpWhenEmptyChanges`, `suppressRevertPairs`, `releaseCommitMessageFormat`, `commitUrlFormat`, `compareUrlFormat`, `issueUrlFormat`, `userUrlFormat`, `packageFiles`, `bumpFiles`, `types`, `skip`, and `scripts`. A custom `types` array replaces the default types; each entry uses `type`, optional `section`, and either `hidden` or `effect = "hidden" | "changelog" | "bump"`. The built-in presets are `conventional-changelog-conventionalcommits` (default) and Angular (`angular` or `conventional-changelog-angular`). Under the default preset a reverted commit and its `Revert "…"` commit both stay in the changelog; `suppressRevertPairs = true` drops both when they fall in the same changelog window, which the Angular preset always does. The key affects changelog rendering only, not bump selection.

```toml
types = [
  { type = "feat", section = "Features", effect = "bump" },
  { type = "fix", section = "Bug Fixes", effect = "bump" },
  { type = "docs", section = "Documentation", effect = "changelog" },
  { type = "chore", effect = "hidden" },
]
issuePrefixes = ["#", "GH-"]

[skip]
tag = false

[scripts]
prerelease = "./scripts/pre-release-check.sh"
```

Lifecycle commands run through `/bin/sh -c`; only use scripts from a trusted configuration. Available lifecycle names are `prerelease`, `prebump`, `postbump`, `prechangelog`, `postchangelog`, `precommit`, `postcommit`, `pretag`, and `posttag`.

## CLI options

```text
Usage: csemver [options]

Bump the version, update the changelog, commit, and tag a release from
Conventional Commits. Configuration is read from csemver.toml; command-line
options override it. Nothing is pushed.

Options:
  -h, --help                     Show this help
  -v, --version                  Show the build version
  -c, --config FILE              Read another TOML configuration file
      --dry-run                  Preview without writing files, commits, or tags
  -r, --release-as VERSION       Select major, minor, patch, or an exact SemVer
  -p, --prerelease [ID]          Create a prerelease
  -f, --first-release            Tag the current version without bumping files
  -t, --tag-prefix PREFIX        Git tag prefix (default: v)
  -i, --infile FILE              Changelog path (default: CHANGELOG.md)
      --skip STEP                Skip bump, changelog, commit, or tag (repeat)
      --path PATH                Limit commit history to a path
      --lerna-package NAME       Select the bump from NAME@VERSION tags
      --packageFiles FILE...     Files the current version is read from
      --bumpFiles FILE...        Version files to update
      --release-count N          Changelog sections to regenerate (default: 1;
                                 0 rebuilds all history)
      --preset NAME              Changelog preset: angular, or the default
                                 conventional-changelog-conventionalcommits
      --header TEXT              Changelog header
      --releaseCommitMessageFormat FORMAT
                                 Release commit message
                                 (default: chore(release): {{currentTag}})
      --commitUrlFormat URL      Commit link format
      --compareUrlFormat URL     Compare link format
      --issueUrlFormat URL       Issue link format
      --userUrlFormat URL        User link format
      --issuePrefixes PREFIX...  Issue reference prefixes (default: #)
      --preMajor                 Apply pre-1.0.0 bump rules
      --noBumpWhenEmptyChanges   Leave the version unchanged when no commit
                                 selects a bump
      --git-tag-fallback         Read the version from the latest tag when no
                                 version file is found (default: on)
      --tag-force                Replace an existing tag
  -s, --sign                     Sign the release commit and tag
      --signoff                  Add a Signed-off-by trailer to the commit
  -n, --no-verify                Bypass Git commit hooks
  -a, --commit-all               Also commit every staged change
      --scripts.EVENT COMMAND    Override one lifecycle script
      --npmPublishHint TEXT      Publishing hint printed after the release
      --silent                   Suppress progress output
  -m, --message FORMAT           Deprecated; use --releaseCommitMessageFormat
      --changelogHeader TEXT     Deprecated; use --header

Boolean options accept --no-NAME to turn them off.
```

`--release-as` accepts `major`, `minor`, `patch`, or an exact SemVer version without the tag prefix. `--skip STEP` can be repeated. CLI flags override the TOML values. `--dry-run` logs configured lifecycle hooks without executing them. With `--lerna-package NAME`, bump recommendation starts after the package's latest stable `NAME@VERSION` tag; changelog ranges and the created release tag continue to use `tagPrefix`. A positive `--release-count` regenerates that many recent release sections and retains older changelog content; `--release-count 0` rebuilds all tagged history.

## Release builds

The build embeds the version recorded in `VERSION` in `--version`. Override it for a build that should report another version:

```sh
make clean all CSEMVER_VERSION=1.2.3
./build/csemver --version
```

## Scope and limits

csemver is C17 with an embedded TOML parser, libxml2, and libyaml. It covers SemVer bumping and prereleases, the version-file updaters above, Conventional Commit bump selection, changelog generation, lifecycle scripts, release commits, and annotated tags. Version fields in other formats use declarative POSIX ERE patterns; executable updaters and executable configuration are not supported.

The built-in changelog presets are `conventional-changelog-conventionalcommits` (default) and Angular (`angular` or `conventional-changelog-angular`); any other name is rejected. Angular covers its section mapping and order, release heading levels, visible performance commits, the breaking-change footer, and dropping revert pairs inside the changelog window. With `--lerna-package`, bump selection reads the latest stable package tag; `tagPrefix` still controls the release tag and changelog windows.

Version files ignored by Git are not bumped. Ignore patterns support brace expansion and extglob groups, including multiple, nested, and negated `@(...)` groups, with each alternative matched on its own so a literal comma inside an alternative (`@(a,b|c).json`) stays literal. Matching is implemented in C and gives the same result on glibc and macOS. Two shapes are unsupported and their result is unspecified: a bare `(` inside an alternative (`@(a(b)|c).txt`) and a negated group next to a wildcard (`@(!(a|b))*`).

The CLI targets POSIX systems with `/bin/sh` and Git. Running `make test` on macOS also requires GNU coreutils' `timeout`; the CI workflow installs it through Homebrew. csemver never pushes: inspect the release, then push the branch and tag yourself.

## CI and published releases

GitHub Actions builds and tests on Ubuntu 24.04 and macOS 15 Intel. Forgejo CI runs on `master` and version tags with the repository's `cpp-latest` runner. Pushing a `v*` tag runs the GitHub release workflow: it checks that the tag, `VERSION`, and release commit agree, reruns the full test suite with the tag version embedded, then creates a GitHub Release using the top section of `CHANGELOG.md`. GitHub provides source archives for the tag; the workflow does not publish prebuilt binaries. Forgejo currently runs CI only.

## Origin

csemver began as a C reimplementation of [commit-and-tag-version](https://github.com/absolute-version/commit-and-tag-version) 12.5.0, checked against it byte for byte while it replaced that tool. Since 0.4.0 it is an independent tool: its own tests define its behavior, and changes are decided on their merits rather than by reference to another implementation. The defaults it inherited (the `package.json` family of version files, the npm publish hint, the conventional-changelog presets) stay because they are useful.

## License

The embedded parser in `src/toml.c` and `src/toml.h` is based on [tomlc99](https://github.com/cktan/tomlc99), revision `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac`. Its MIT copyright and license notices are retained in the source files. The csemver project license has not yet been selected.
