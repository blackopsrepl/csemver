# csemver

<p align="center">
  <img src="docs/assets/csemver-mascot.png" alt="The csemver clockwork raven release steward" width="200">
</p>

**A native C17 release manager with TOML configuration.**

csemver implements the version bump, changelog, release commit, and annotated Git tag workflow of `commit-and-tag-version` without a Node.js runtime or JavaScript configuration. The current port is under active compatibility work; it is not yet a byte-for-byte replacement. Preview every release with `--dry-run` before using it on a production repository.

## Build and test

Requirements: a C17 compiler, GNU Make, Git, libxml2 and libyaml development files, and `pkg-config` (for example, `libxml2-dev libyaml-dev pkg-config` on Debian/Ubuntu). The TOML parser is compiled directly from `src/toml.c`.

```sh
make
make test
./build/csemver --version
./build/commit-and-tag-version --version
```

`make` emits `build/commit-and-tag-version` as a symlink to the same native C binary, so existing scripts can invoke csemver under the upstream command name without a Node.js wrapper. `csemver --version` prints its embedded build version. The compatibility alias mirrors upstream's package lookup and prints the nearest `package.json` version, or `unknown` when none is found.

`make test` exercises the SemVer and TOML code, version-file updates, CLI parsing, and release operations in isolated Git repositories. Set `TMPDIR` to a writable scratch directory if `/tmp` is unavailable.

## Quick start

From a Git working tree with conventional commits:

```sh
./build/csemver --dry-run
./build/csemver
git push --follow-tags origin master
```

The release command updates configured version files and the changelog, creates a release commit, then creates an annotated tag. It does **not** push; review the preview and push the branch and tag yourself. The first release can use `--first-release` to tag the version already in the package file without bumping it.

By default, `feat` selects a minor bump, `fix` selects a patch bump, and a Conventional Commit breaking change selects a major bump. Under the default Conventional Commits preset, versions below `1.0.0` automatically use upstream's pre-major rules: `feat` selects a patch bump and a breaking change selects a minor bump. `preMajor = true` also applies those rules explicitly, including with the Angular preset. With no qualifying commits, the legacy default is a patch bump; set `noBumpWhenEmptyChanges = true` to leave the repository unchanged instead.

## TOML configuration

csemver reads `csemver.toml` from the current working directory; `-c FILE` selects another file. Existing `package.json` sections named `standard-version` or `commit-and-tag-version` provide compatibility defaults for supported string and boolean options, nested `skip` booleans (`bump`, `changelog`, `commit`, and `tag`), nested `scripts` lifecycle command strings, integer `releaseCount`, object-array `types`, and `packageFiles`/`bumpFiles` arrays of strings or `{ filename, type }` objects; `standard-version` wins when both are present. Precedence is package metadata, then TOML, then CLI. JSON is read as data only; csemver does not execute JavaScript or load JavaScript updater modules. A string `updater` path in a package-file entry is rejected before the release starts. Version fields in arbitrary formats can instead use the native `regex` updater below. String arrays for `issuePrefixes` and `packageFiles` are supported; object-array `types` entries support `type`, optional `section`, and `hidden` or `effect`. `packageFiles` strings and objects without `type`, plus `bumpFiles` strings and objects without `type`, use upstream filename inference. Unsupported package.json `packageFiles` entries warn and stop the release before changes; unsupported `bumpFiles` entries warn and are skipped. Typed package.json updater types outside upstream's built-in set are also warned and handled like upstream. Other numeric and array package settings remain unsupported and should be set in TOML or on the command line. This repository uses a plain-text `VERSION` file as both its package-version source and bump target:

```toml
tagPrefix = "v"
releaseCommitMessageFormat = "chore(release): {{currentTag}}"
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
```

File lists may also contain strings (for recognized extensions) or tables with `filename` and `type`. Package.json compatibility arrays use upstream's built-in updater types. The native `csemver.toml` configuration additionally supports updater type `toml`; it supports `csproj`, `gradle`, `json`, `maven`, `python`, `yaml`, `openapi`, and `plain-text` as well. Gradle files rewrite the matched declaration as `version = "VERSION"` and preserve the rest of the file. C# project files rewrite the matched `<Version>` element and preserve the rest of the file. Maven POM files use libxml2 to resolve direct or `${property}` versions and reserialize the document with upstream-compatible formatting and newline behavior for the tested fixtures. JSON package-lock files update only the root package version surfaces. JSON files are reserialized with detected indentation and newline conventions and end with a newline, matching upstream behavior; Python and TOML updates preserve surrounding bytes. YAML and OpenAPI files use libyaml to select the root `version` or `info.version` mapping entry; scalar text is preserved while single-line flow spacing, tested multiline root flow-map formatting, and line-ending behavior match upstream. Plain-text version contents are read verbatim, upstream-compatible surrounding whitespace is accepted for SemVer parsing, and updates replace the entire file contents with the new version.

`packageFiles` selects where csemver reads the current version; custom entries are not implicitly added to `bumpFiles`. Declare each file to update in `bumpFiles`. For version fields in files without a built-in updater, configure `type = "regex"` in `csemver.toml` with a POSIX extended regular expression (POSIX ERE, not JavaScript regex syntax) and the capture group containing the version. `versionGroup` defaults to `1`; group `0` selects the full match. The matcher uses line-aware anchors and updates only the selected capture, preserving all surrounding bytes. It requires no runtime from the target repository and does not execute JavaScript config or updater code. The Angular changelog preset is a commit/changelog format, not an Angular-framework integration.

```toml
packageFiles = [
  { filename = "lib/release.rb", type = "regex", pattern = '^(  VERSION = ")([^"]+)(")$', versionGroup = 2 },
]
bumpFiles = [
  { filename = "lib/release.rb", type = "regex", pattern = '^(  VERSION = ")([^"]+)(")$', versionGroup = 2 },
  { filename = "README.md", type = "regex", pattern = '^(## Current release: `v)([^`]+)(`)$', versionGroup = 2 },
]
```

Other supported keys include `preset`, `infile`, `header`, `releaseAs`, `prerelease`, `releaseCount`, `tagPrefix`, `issuePrefixes`, `path`, `lernaPackage`, `preMajor`, `noBumpWhenEmptyChanges`, `releaseCommitMessageFormat`, `commitUrlFormat`, `compareUrlFormat`, `issueUrlFormat`, `userUrlFormat`, `packageFiles`, `bumpFiles`, `types`, `skip`, and `scripts`. A custom `types` array replaces the default types; each entry uses `type`, optional `section`, and either `hidden` or `effect = "hidden" | "changelog" | "bump"`. The built-in presets are `conventional-changelog-conventionalcommits` (default) and Angular (`angular` or `conventional-changelog-angular`).

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
-h, --help                 Show help
-v, --version              Show the build version
-r, --release-as VERSION   Select major, minor, patch, or an exact SemVer
-p, --prerelease [ID]      Create a prerelease
-f, --first-release        Tag current version without bumping version files
-t, --tag-prefix PREFIX    Override the Git tag prefix
-i, --infile FILE          Override the changelog path
-c, --config FILE          Read another TOML configuration file
    --dry-run              Preview without changing files, commits, or tags
    --skip STEP            Skip bump, changelog, commit, or tag
    --path PATH            Limit commit history to a path
    --lerna-package NAME   Use package tags for bump selection
    --packageFiles FILE... Override package version files
    --bumpFiles FILE...    Override version files to update
    --release-count N      Set regenerated sections; 0 rebuilds all history
    --preset NAME           Select conventional or Angular changelog preset
    --sign                 Sign the release commit and tag
    --signoff              Add a DCO signoff to the commit
-m, --message FORMAT       Deprecated; use the TOML release message format
    --releaseCommitMessageFormat FORMAT
    --header TEXT           Changelog heading
    --commitUrlFormat URL   Customize commit links
    --compareUrlFormat URL  Customize compare links
    --issueUrlFormat URL    Customize issue links
    --userUrlFormat URL     Customize user links
    --preMajor              Apply pre-1.0.0 bump rules
    --issuePrefixes PREFIX... Issue prefixes to link
    --tag-force             Replace an existing tag
    --git-tag-fallback      Read version from a tag if no version file exists
    --noBumpWhenEmptyChanges Do not bump for commits with no release effect
    --scripts.EVENT COMMAND Override one lifecycle script
    --npmPublishHint TEXT   Customize the publishing hint
-n, --no-verify            Bypass Git commit hooks
-a, --commit-all           Include all staged and working files
    --silent               Suppress normal progress output
```

`--release-as` accepts `major`, `minor`, `patch`, or an exact SemVer version without the tag prefix. `--skip STEP` can be repeated. CLI flags override the TOML values. `--dry-run` logs configured lifecycle hooks without executing them. With `--lerna-package NAME`, bump recommendation starts after the package's latest stable `NAME@VERSION` tag; changelog ranges and the created release tag continue to use `tagPrefix`. A positive `--release-count` regenerates that many recent release sections and retains older changelog content; `--release-count 0` rebuilds all tagged history.

## Release builds

The build embeds its version in `--version`; the default is `0.1.0`. Set it explicitly for a tagged build:

```sh
make clean all CSEMVER_VERSION=1.2.3
./build/csemver --version
```

## Compatibility status

The implementation is pure C17 plus optional TOML configuration and an integrated C TOML parser. It currently covers SemVer, common version files, conventional-commit bump selection, changelog generation, release lifecycle scripts, release commits, and annotated tags. Text version surfaces without a built-in updater can use declarative POSIX ERE patterns in TOML; arbitrary executable parser/writer extensions and JavaScript config modules are not supported. The built-in changelog presets are `conventional-changelog-conventionalcommits` and Angular (`angular` or `conventional-changelog-angular`); other preset names are rejected rather than silently treated as the default. Angular support implements the common section mapping/order, release heading levels, visible performance commits, breaking-change footer, and suppression of revert pairs when both commits are in the changelog window. Lerna package selection uses the latest stable package tag for bump recommendation; the configured `tagPrefix` still controls the generated release tag and changelog windows. Git-ignore exclusions include tested minimatch-style `@(a|b)` alternatives, translated in C rather than relying on platform-specific `fnmatch` extensions. Alternatives are matched one at a time rather than lowered to brace syntax, so a literal comma inside an alternative (`@(a,b|c).json`) stays literal; multiple groups (`@(a|b)-@(c|d).json`), nested `@()` groups (`@(a|@(b|c)).json`), and `!()` groups (`@(!(a|b)).json`) are covered by regression tests. This replaces the platform `fnmatch` extglob support, which is GNU-only: glibc `FNM_EXTMATCH` does not implement `!()`, so negated groups silently over-matched on macOS. Two minimatch corners are known to diverge and are not claimed: a bare `(` nested inside an alternative (`@(a(b)|c).txt`, which minimatch treats as literal text) and a `!()` group adjacent to a wildcard (`@(!(a|b))*`), where the wildcard changes which part of the name the group absorbs. Byte-level comparisons against upstream pass for release counts `0` through `4` in the tested fixtures, the default dry-run fixture, selected Angular fixtures including revert handling across tag boundaries, UTC-dated headings under a non-UTC `TZ`, and the regex-updater release cases; broad behavioral and byte-for-byte compatibility has **not** been established. The upstream Node project is used only as a test reference outside this repository.

The native CLI targets POSIX systems with `/bin/sh`, Git, a C17 compiler, libxml2, and libyaml. Running `make test` on macOS also requires GNU coreutils' `timeout`; the CI workflow installs it through Homebrew. The CLI never pushes to a remote; inspect the generated release and push the branch and tag explicitly.

## CI and published releases

GitHub Actions builds and tests on Ubuntu 24.04 and macOS 15 Intel. Forgejo CI runs on `master` and version tags with the repository's `cpp-latest` runner. Pushing a `v*` tag runs the GitHub release workflow: it checks that the tag, `VERSION`, and release commit agree, reruns the full test suite with the tag version embedded, then creates a GitHub Release using the top section of `CHANGELOG.md`. GitHub provides source archives for the tag; the workflow does not publish prebuilt binaries. Forgejo currently runs CI only.

## License

The embedded parser in `src/toml.c` and `src/toml.h` is based on [tomlc99](https://github.com/cktan/tomlc99), revision `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac`. Its MIT copyright and license notices are retained in the source files. The csemver project license has not yet been selected.
