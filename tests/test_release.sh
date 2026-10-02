#!/bin/sh
set -eu
bin=$(cd "$(dirname "$0")/.." && pwd)/build/csemver
tmp=$(mktemp -d "${TMPDIR:-/tmp}/csemver-integration.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$tmp"
git init -q -b main
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '1.0.0\n' > VERSION
cat > csemver.toml <<'TOML'
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
TOML
marker="$tmp/lifecycle.marker"
printf '[scripts]\nprechangelog = "touch %s"\n' "$marker" >> csemver.toml
git add VERSION csemver.toml
git commit -qm 'chore: seed release fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
printf 'feature\n' > feature.txt
git add feature.txt
git commit -qm 'feat(core): add a feature'
printf 'seed fix\n' > seed-fix.txt
git add seed-fix.txt
git commit -qm 'fix: correct seed parsing'
printf 'user work\n' > staged.txt
git add staged.txt
dry_run_output=$("$bin" --dry-run)
printf '%s\n' "$dry_run_output" | grep -q 'bumping version in VERSION from 1.0.0 to 1.1.0'
printf '%s\n' "$dry_run_output" | grep -q '^## \[1.1.0\]'
! printf '%s\n' "$dry_run_output" | grep -q '^# Changelog'
test ! -e "$marker"
[ "$(cat VERSION)" = '1.0.0' ]
[ ! -f CHANGELOG.md ]
[ "$(git tag --list 'v1.1.0')" = '' ]

"$bin" > /dev/null
test -f "$marker"
rm -f "$marker"
[ "$(cat VERSION)" = '1.1.0' ]
grep -q '^## \[1.1.0\]' CHANGELOG.md
grep -q 'add a feature' CHANGELOG.md
grep -q 'correct seed parsing' CHANGELOG.md
grep -q '^chore(release): 1.1.0$' <<EOF
$(git log -1 --format=%s)
EOF
test "$(git cat-file -t refs/tags/v1.1.0)" = tag
[ "$(git diff --cached --name-only)" = 'staged.txt' ]
test -z "$(git show --pretty= --name-only HEAD | grep -Fx staged.txt || true)"
git reset -q HEAD -- staged.txt
rm -f staged.txt
test -z "$(git status --porcelain)"

printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: correct a regression'
next_dry_run=$("$bin" --dry-run)
test ! -e "$marker"
printf '%s\n' "$next_dry_run" | grep -q '^## \[1.1.1\]'
! printf '%s\n' "$next_dry_run" | grep -q '^## \[1.1.0\]'
[ "$(cat VERSION)" = '1.1.0' ]
[ "$(git tag --list 'v1.1.1')" = '' ]
"$bin" > /dev/null
test -f "$marker"
rm -f "$marker"
[ "$(cat VERSION)" = '1.1.1' ]
grep -q '^## \[1.1.1\]' CHANGELOG.md
grep -q 'correct a regression' CHANGELOG.md
test -z "$(git status --porcelain)"
printf 'new major feature\n' > major.txt
git add major.txt
git commit -qm 'feat: add the next generation'
"$bin" --release-as 2.0.0 > /dev/null
test -f "$marker"
rm -f "$marker"
[ "$(cat VERSION)" = '2.0.0' ]
test "$(git cat-file -t refs/tags/v2.0.0)" = tag
grep -q '^## \[2.0.0\]' CHANGELOG.md
test -z "$(git status --porcelain)"
printf 'metadata only\n' > metadata.txt
git add metadata.txt
git commit -qm 'chore: refresh generated metadata'
"$bin" > /dev/null
test -f "$marker"
rm -f "$marker"
[ "$(cat VERSION)" = '2.0.1' ]
test "$(git cat-file -t refs/tags/v2.0.1)" = tag

test -z "$(git status --porcelain)"

mkdir "$tmp/first-release"
cd "$tmp/first-release"
git init -q -b main
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "first-release",\n  "version": "0.5.0"\n}\n' > package.json
git add package.json
git commit -qm 'feat: initialize first release'
first_release_output=$("$bin" --first-release --release-as 2.0.0 --packageFiles package.json --bumpFiles package.json)
printf '%s\n' "$first_release_output" | grep -q 'committing CHANGELOG.md'
! printf '%s\n' "$first_release_output" | grep -q 'npm publish'
grep -q '"version": "0.5.0"' package.json
test "$(git cat-file -t refs/tags/v0.5.0)" = tag

mkdir "$tmp/breaking-changes"
cd "$tmp/breaking-changes"
git init -q -b main
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '1.2.3\n' > VERSION
cat > csemver.toml <<'TOML'
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
TOML
git add VERSION csemver.toml
git commit -qm 'chore: initialize breaking-change fixture'
printf 'breaking change\n' > api.txt
git add api.txt
git commit -qm 'feat(api)!: remove the legacy endpoint'
"$bin" > /dev/null
[ "$(cat VERSION)" = '2.0.0' ]
printf 'breaking footer\n' > footer.txt
git add footer.txt
git commit -qm 'fix: preserve new API contract' -m 'BREAKING CHANGE: callers must migrate to the new API.'
"$bin" > /dev/null
[ "$(cat VERSION)" = '3.0.0' ]
test "$(git cat-file -t refs/tags/v3.0.0)" = tag
grep -q '^### .*BREAKING CHANGES' CHANGELOG.md
grep -q 'callers must migrate to the new API' CHANGELOG.md

test -z "$(git status --porcelain)"
mkdir "$tmp/no-empty-bump"
cd "$tmp/no-empty-bump"
git init -q -b main
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{\n  "name": "no-empty-bump",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize no-empty-bump fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
printf 'metadata only\n' > metadata.txt
git add metadata.txt
git commit -qm 'chore: refresh metadata'
"$bin" --noBumpWhenEmptyChanges
grep -q '"version": "1.0.0"' package.json
test ! -f CHANGELOG.md
test -z "$(git tag --list 'v1.0.1')"
test -z "$(git status --porcelain)"
mkdir "$tmp/lifecycle"
cd "$tmp/lifecycle"
git init -q -b main
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "lifecycle",\n  "version": "1.0.0"\n}\n' > package.json
cat > csemver.toml <<TOML
packageFiles = ["package.json"]
bumpFiles = ["package.json"]
[scripts]
prerelease = "touch $tmp/prerelease-ran"
prebump = "printf 4.2.0"
postbump = "touch $tmp/postbump-ran"
precommit = "printf 'chore(release): from precommit hook'"
TOML
git add package.json csemver.toml
git commit -qm 'chore: initialize lifecycle fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
printf 'feature\n' > feature.txt
git add feature.txt
git commit -qm 'feat: add lifecycle test'
"$bin" > /dev/null
grep -q '"version": "4.2.0"' package.json
test -f "$tmp/prerelease-ran"
test -f "$tmp/postbump-ran"
test "$(git log -1 --format=%s)" = 'chore(release): from precommit hook'
test "$(git cat-file -t refs/tags/v4.2.0)" = tag
test "$(git cat-file -p refs/tags/v4.2.0 | grep -F 'chore(release): from precommit hook')" = 'chore(release): from precommit hook'
rm -f "$tmp/prerelease-ran" "$tmp/postbump-ran"
test -z "$(git status --porcelain)"

printf '%s\n' 'release workflow tests passed'
