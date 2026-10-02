# csemver

<p align="center">
  <img src="docs/assets/csemver-mascot.png" alt="The csemver clockwork raven release steward" width="200">
</p>

**A native C17 release manager with TOML configuration.**

csemver implements the version bump, changelog, release commit, and annotated Git tag workflow of `commit-and-tag-version` without a Node.js runtime or JavaScript configuration. The current port is under active compatibility work; it is not yet a byte-for-byte replacement. Preview every release with `--dry-run` before using it on a production repository.

## Build and test

Requirements: a C17 compiler, GNU Make, and Git. The TOML parser is compiled directly from `src/toml.c`; there are no runtime package dependencies.

```sh
make
make test
./build/csemver --version
```

`make test` exercises the SemVer and TOML code, version-file updates, CLI parsing, and release operations in isolated Git repositories. Set `TMPDIR` to a writable scratch directory if `/tmp` is unavailable.

## Quick start

From a Git working tree with conventional commits:

```sh
./build/csemver --dry-run
./build/csemver
git push --follow-tags origin main
```

The release command updates configured version files and the changelog, creates a release commit, then creates an annotated tag. It does **not** push; review the preview and push the branch and tag yourself. The first release can use `--first-release` to tag the version already in the package file without bumping it.

By default, `feat` selects a minor bump, `fix` selects a patch bump, and a Conventional Commit breaking change selects a major bump. With no qualifying commits, the legacy default is a patch bump; set `noBumpWhenEmptyChanges = true` to leave the repository unchanged instead. `preMajor = true` changes feature/breaking bumps while the current version is `0.x`.

## TOML configuration

csemver reads `csemver.toml` from the current working directory; `-c FILE` selects another file. This repository uses a plain-text `VERSION` file as both its package-version source and bump target:

```toml
tagPrefix = "v"
releaseCommitMessageFormat = "chore(release): {{currentTag}}"
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
```

File lists may also contain strings (for recognized extensions) or tables with `filename` and `type`. Supported updater types are `json`, `python`, `toml`, `yaml`, `openapi`, and `plain-text`. JSON package-lock files update only the root package version surfaces. Version changes preserve the surrounding file bytes; the plain-text updater writes just the version token.

Other supported keys include `infile`, `header`, `releaseAs`, `prerelease`, `releaseCount`, `tagPrefix`, `issuePrefixes`, `path`, `preMajor`, `noBumpWhenEmptyChanges`, `releaseCommitMessageFormat`, `commitUrlFormat`, `compareUrlFormat`, `issueUrlFormat`, `userUrlFormat`, `packageFiles`, `bumpFiles`, `types`, `skip`, and `scripts`. A custom `types` array replaces the default types; each entry uses `type`, optional `section`, and either `hidden` or `effect = "hidden" | "changelog" | "bump"`.

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
    --packageFiles FILE... Override package version files
    --bumpFiles FILE...    Override version files to update
    --release-count N      Set regenerated sections; 0 rebuilds all history
    --sign                 Sign the release commit and tag
    --signoff              Add a DCO signoff to the commit
-m, --message FORMAT       Deprecated; use the TOML release message format
    --releaseCommitMessageFormat FORMAT
    --header TEXT           Changelog heading
    --issuePrefixes PREFIX... Issue prefixes to link
    --tag-force             Replace an existing tag
    --git-tag-fallback      Read version from a tag if no version file exists
    --noBumpWhenEmptyChanges Do not bump for commits with no release effect
-n, --no-verify            Bypass Git commit hooks
-a, --commit-all           Include all staged and working files
    --silent               Suppress normal progress output
```

`--release-as` accepts `major`, `minor`, `patch`, or an exact SemVer version without the tag prefix. `--skip STEP` can be repeated. CLI flags override the TOML values. `--dry-run` does not execute configured lifecycle scripts. A positive `--release-count` regenerates that many recent release sections and retains older changelog content; `--release-count 0` rebuilds all tagged history.

## Release builds

The build embeds its version in `--version`; the default is `0.1.0`. Set it explicitly for a tagged build:

```sh
make clean all CSEMVER_VERSION=1.2.3
./build/csemver --version
```

## Compatibility status

The implementation is pure C17 plus optional TOML configuration and an integrated C TOML parser. It currently covers SemVer, common version files, conventional-commit bump selection, changelog generation, release lifecycle scripts, release commits, and annotated tags. Non-default changelog presets and Lerna package selection are not implemented. Byte-level comparisons against upstream pass for release counts `0` through `4` in the tested fixtures, as well as the default dry-run fixture; broad behavioral and byte-for-byte compatibility has **not** been established. The upstream Node project is used only as a test reference outside this repository.

The native CLI targets POSIX systems with `/bin/sh`, Git, and a C17 compiler. It never pushes to a remote; inspect the generated release and push the resulting branch and tag explicitly.

## CI and published releases

GitHub Actions builds and tests on Ubuntu and macOS. The matching Forgejo CI workflow uses the repository's `cpp-latest` runner. Pushing a `v*` tag runs the GitHub release workflow: it checks that the tag, `VERSION`, and release commit agree, reruns the full test suite with the tag version embedded, then creates a GitHub Release using the top section of `CHANGELOG.md`. GitHub provides source archives for the tag; the workflow does not publish prebuilt binaries. Forgejo currently runs CI only.

## License

The embedded parser in `src/toml.c` and `src/toml.h` is based on [tomlc99](https://github.com/cktan/tomlc99), revision `29076dfd095bbbbd50a3c1b2760d29f4b83e74ac`. Its MIT copyright and license notices are retained in the source files. The csemver project license has not yet been selected.
