#!/bin/sh
set -eu
bin=$(cd "$(dirname "$0")/.." && pwd)/build/csemver
tmp=$(mktemp -d "${TMPDIR:-/tmp}/csemver-integration.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$tmp"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '1.0.0\n' > VERSION
printf '{\n  "name": "csemver-fixture",\n  "repository": {"type": "git", "url": "https://github.com/example/csemver.git"}\n}\n' > package.json
cat > csemver.toml <<'TOML'
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
TOML
marker="$tmp/lifecycle.marker"
printf '[scripts]\nprechangelog = "touch %s"\n' "$marker" >> csemver.toml
git add VERSION csemver.toml
git add package.json
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
utc_hour=$(date -u '+%H')
if [ "$utc_hour" -lt 12 ]; then
  release_timezone=Etc/GMT+12
else
  release_timezone=Pacific/Kiritimati
fi
expected_release_date=$(date -u '+%Y-%m-%d')
dry_run_output=$(TZ="$release_timezone" "$bin" --dry-run)
printf '%s\n' "$dry_run_output" | grep -Fq "($expected_release_date)" || {
  printf 'release heading date must use UTC (%s):\n%s\n' \
    "$expected_release_date" "$dry_run_output" >&2
  exit 1
}
printf '%s\n' "$dry_run_output" | grep -Fq 'bumping version in VERSION from 1.0.0'
printf '%s\n' "$dry_run_output" | grep -Fxq ' to 1.1.0'
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
grep -q '^## \[1.1.0\](https://github.com/example/csemver/compare/v1.0.0...v1.1.0)' CHANGELOG.md
grep -q 'https://github.com/example/csemver/commit/' CHANGELOG.md
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
"$bin" --release-as 2.0.0 --release-count 2 > /dev/null
test -f "$marker"
rm -f "$marker"
[ "$(cat VERSION)" = '2.0.0' ]
test "$(git cat-file -t refs/tags/v2.0.0)" = tag
grep -q '^## \[2.0.0\]' CHANGELOG.md
grep -q '^## \[2.0.0\](https://github.com/example/csemver/compare/v1.1.1...v2.0.0)' CHANGELOG.md
[ "$(grep -c '^## \[1.1.1\]' CHANGELOG.md)" -eq 2 ]
grep -q '^## \[1.1.1\](https://github.com/example/csemver/compare/v1.1.0...v1.1.1)' CHANGELOG.md
test -z "$(git status --porcelain)"
printf 'metadata only\n' > metadata.txt
git add metadata.txt
git commit -qm 'chore: refresh generated metadata'
"$bin" > /dev/null
test -f "$marker"
rm -f "$marker"
[ "$(cat VERSION)" = '2.0.1' ]
test "$(git cat-file -t refs/tags/v2.0.1)" = tag

printf 'stale generated history\n' > CHANGELOG.md
"$bin" --release-count 0 --skip bump --skip commit --skip tag > /dev/null
grep -q '^## \[2.0.1\]' CHANGELOG.md
header_line=$(grep -n 'for commit guidelines\.' CHANGELOG.md | cut -d: -f1)
release_line=$(grep -n '^## \[2.0.1\]' CHANGELOG.md | cut -d: -f1)
[ "$((release_line - header_line))" -eq 3 ]
grep -q '^## \[2.0.0\]' CHANGELOG.md
grep -q '^## \[1.1.1\]' CHANGELOG.md
grep -q '^## \[1.1.0\]' CHANGELOG.md
! grep -q 'stale generated history' CHANGELOG.md
rm -f "$marker"
git checkout -- CHANGELOG.md

"$bin" --release-count 6 --skip bump --skip commit --skip tag > /dev/null
test -f "$marker"
rm -f "$marker"
initial_release_line=$(grep -n '^## 1\.0\.0 (' CHANGELOG.md | cut -d: -f1)
preserved_release_line=$(grep -n '^## \[2\.0\.1\]' CHANGELOG.md | tail -n 1 | cut -d: -f1)
[ "$((preserved_release_line - initial_release_line))" -eq 2 ]
git checkout -- CHANGELOG.md

release_count_zero_preview=$("$bin" --dry-run --release-count 0)
candidate_heading=$(printf '%s\n' "$release_count_zero_preview" | grep -m 1 '^## \[2\.0\.2\]')
latest_tag_heading=$(printf '%s\n' "$release_count_zero_preview" | grep -m 1 '^## \[2\.0\.1\]')
section_boundary=$(printf '%s\n' "$release_count_zero_preview" | grep -F -A2 "$candidate_heading")
expected_section_boundary=$(printf '%s\n\n%s' "$candidate_heading" "$latest_tag_heading")
[ "$section_boundary" = "$expected_section_boundary" ] || {
  printf 'empty latest section has wrong spacing\nexpected:\n%s\nactual:\n%s\n' \
    "$expected_section_boundary" "$section_boundary" >&2
  exit 1
}

test -z "$(git status --porcelain)"

mkdir "$tmp/frontmatter-changelog"
cd "$tmp/frontmatter-changelog"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "frontmatter-changelog",\n  "version": "1.0.0"\n}\n' > package.json
printf '%s\n' '---' 'status: new' '---' '' '# Changelog' '' 'All notable changes to this project will be documented in this file. See [commit-and-tag-version](https://github.com/absolute-version/commit-and-tag-version) for commit guidelines.' '' '## [1.0.0](https://example.invalid/compare/v0.0.1...v1.0.0) (2026-01-01)' '' '### Features' '' '* existing feature' > CHANGELOG.md
git add package.json CHANGELOG.md
git commit -qm 'chore: initialize front matter fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: update changelog with front matter'
"$bin" > /dev/null
IFS= read -r changelog_first_line < CHANGELOG.md
if [ "$changelog_first_line" != '---' ]; then
  printf 'expected front matter at changelog start, got: %s\n' \
    "$changelog_first_line" >&2
  exit 1
fi
test "$(grep -Fc 'status: new' CHANGELOG.md)" -eq 1
test "$(grep -c '^# Changelog$' CHANGELOG.md)" -eq 1
grep -q '^## \[1.0.1\]' CHANGELOG.md
grep -q '^## \[1.0.0\]' CHANGELOG.md
test -z "$(git status --porcelain)"

mkdir "$tmp/first-release"
cd "$tmp/first-release"
git init -q -b master
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

mkdir "$tmp/first-release-skip-bump"
cd "$tmp/first-release-skip-bump"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "first-release-skip-bump",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize first release skip-bump fixture'
first_release_skip_bump_head=$(git rev-parse HEAD)
if first_release_skip_bump_output=$("$bin" --first-release --skip.bump --skip.changelog --skip.commit --skip.tag 2>&1); then
  first_release_skip_bump_status=0
else
  first_release_skip_bump_status=$?
fi
test "$first_release_skip_bump_status" -eq 0
test -z "$first_release_skip_bump_output"
test "$(git rev-parse HEAD)" = "$first_release_skip_bump_head"
grep -q '"version": "1.0.0"' package.json
test -z "$(git tag --list)"
test -z "$(git status --porcelain)"

mkdir "$tmp/breaking-changes"
cd "$tmp/breaking-changes"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '1.2.3\n' > VERSION
printf '{\n  "name": "breaking-change-fixture",\n  "version": "1.2.3",\n  "repository": {"type": "git", "url": "https://github.com/example/breaking.git"}\n}\n' > package.json
cat > csemver.toml <<'TOML'
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
TOML
git add VERSION csemver.toml package.json
git commit -qm 'chore: initialize breaking-change fixture'
printf 'breaking change\n' > api.txt
git add api.txt
git commit -qm 'feat(api)!: remove the legacy endpoint'
printf 'breaking output change\n' > output.txt
git add output.txt
git commit -qm 'feat(cli)!: change output format #990' -m 'BREAKING CHANGE: scripts must update to the new output.' -m 'References #99' -m 'Fixes #98'
"$bin" > /dev/null
[ "$(cat VERSION)" = '2.0.0' ]
grep -Fq '* **api:** remove the legacy endpoint' CHANGELOG.md
grep -Fq '* **cli:** scripts must update to the new output.' CHANGELOG.md
grep -Fq '* **cli:** change output format' CHANGELOG.md
grep -Fq 'references [#99](https://github.com/example/breaking/issues/99)' CHANGELOG.md
grep -Fq 'closes [#98](https://github.com/example/breaking/issues/98)' CHANGELOG.md
api_note_line=$(grep -m 1 -nF '* **api:** remove the legacy endpoint' CHANGELOG.md | cut -d: -f1)
cli_note_line=$(grep -m 1 -nF '* **cli:** scripts must update to the new output.' CHANGELOG.md | cut -d: -f1)
test "$api_note_line" -lt "$cli_note_line"
api_feature_line=$(grep -nF '* **api:** remove the legacy endpoint (' CHANGELOG.md | cut -d: -f1)
cli_feature_line=$(grep -nF '* **cli:** change output format [#990]' CHANGELOG.md | cut -d: -f1)
test "$api_feature_line" -lt "$cli_feature_line"
printf 'breaking footer\n' > footer.txt
git add footer.txt
git commit -qm 'fix: preserve new API contract' -m 'BREAKING CHANGE: callers must migrate to the new API.'
"$bin" > /dev/null
[ "$(cat VERSION)" = '3.0.0' ]
test "$(git cat-file -t refs/tags/v3.0.0)" = tag
grep -q '^### .*BREAKING CHANGES' CHANGELOG.md
grep -q 'callers must migrate to the new API' CHANGELOG.md

mkdir "$tmp/prerelease-window"
cd "$tmp/prerelease-window"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "prerelease-window",\n  "version": "1.0.0",\n  "repository": {"type": "git", "url": "https://github.com/example/prerelease.git"}\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize prerelease window fixture'
"$bin" --first-release > /dev/null
first_release_heading=$(grep -m 1 '^## ' CHANGELOG.md)
case "$first_release_heading" in
  '## 1.0.0 ('*) ;;
  *) printf 'first release heading is not in upstream format: %s\n' "$first_release_heading" >&2; exit 1 ;;
esac
printf 'feature\n' > feature.txt
git add feature.txt
git commit -qm 'feat: add prerelease feature'
"$bin" --prerelease dev > /dev/null
test "$(git tag --list 'v1.1.0-dev.0')" = 'v1.1.0-dev.0'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: add prerelease fix'
"$bin" --prerelease rc > /dev/null
test "$(git tag --list 'v1.1.0-rc.0')" = 'v1.1.0-rc.0'
rc_heading=$(grep -F '## [1.1.0-rc.0]' CHANGELOG.md)
case "$rc_heading" in
  *'compare/v1.0.0...v1.1.0-rc.0'*) ;;
  *) printf 'rc changelog range starts from the wrong tag: %s\n' "$rc_heading" >&2; exit 1 ;;
esac
feature_count=$(grep -Fc '* add prerelease feature' CHANGELOG.md)
[ "$feature_count" -eq 2 ] || {
  printf 'expected the prerelease feature in both dev and rc sections; got %s copies\n' \
    "$feature_count" >&2
  exit 1
}

test -z "$(git status --porcelain)"

mkdir "$tmp/prerelease-escalation"
cd "$tmp/prerelease-escalation"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "prerelease-escalation",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize prerelease escalation fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: add initial fix'
"$bin" --prerelease rc > /dev/null
grep -q '"version": "1.0.1-rc.0"' package.json
git commit --allow-empty -qm 'feat: add feature during prerelease'
"$bin" --prerelease rc > /dev/null
grep -q '"version": "1.1.0-rc.0"' package.json
test "$(git tag --list v1.1.0-rc.0)" = v1.1.0-rc.0
test -z "$(git status --porcelain)"

mkdir "$tmp/prerelease-channel-collision"
cd "$tmp/prerelease-channel-collision"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "prerelease-channel-collision",\n  "version": "1.4.3-abc.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize prerelease channel collision fixture'
git tag -a v1.4.3-xyz.0 -m 'chore(release): 1.4.3-xyz.0'
git tag -a v1.4.3-xyz.2 -m 'chore(release): 1.4.3-xyz.2'
git commit --allow-empty -qm 'fix: change prerelease channel'
"$bin" --prerelease xyz > /dev/null
grep -q '"version": "1.4.3-xyz.3"' package.json
test "$(git tag --list v1.4.3-xyz.3)" = v1.4.3-xyz.3
test -z "$(git status --porcelain)"

mkdir "$tmp/release-as-leading-v"
cd "$tmp/release-as-leading-v"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "release-as-leading-v",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize release-as leading-v fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: exercise v-prefixed release-as'
"$bin" --release-as v2.0.0 > /dev/null
grep -q '"version": "2.0.0"' package.json
test "$(git tag --list v2.0.0)" = v2.0.0
test -z "$(git status --porcelain)"

mkdir "$tmp/release-as-invalid"
cd "$tmp/release-as-invalid"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "release-as-invalid",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize invalid release-as fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: exercise invalid release-as'
invalid_release_head=$(git rev-parse HEAD)
if invalid_release_output=$("$bin" --release-as invalid 2>&1); then
  printf '%s\n' 'csemver accepted an invalid release-as value' >&2
  exit 1
else
  invalid_release_status=$?
fi
test "$invalid_release_status" -eq 1
test "$invalid_release_output" = "releaseAs must be one of 'major', 'minor' or 'patch', or a valid semvar version."
test "$(git rev-parse HEAD)" = "$invalid_release_head"
grep -q '"version": "1.0.0"' package.json
test "$(git tag --list)" = v1.0.0
test ! -e CHANGELOG.md
if silent_invalid_release_output=$("$bin" --silent --release-as invalid 2>&1); then
  printf '%s\n' 'csemver accepted an invalid release-as value in silent mode' >&2
  exit 1
else
  silent_invalid_release_status=$?
fi
test "$silent_invalid_release_status" -eq 1
test -z "$silent_invalid_release_output"
"$bin" --release-as invalid --skip.bump --skip.changelog --skip.commit --skip.tag --silent
test "$(git rev-parse HEAD)" = "$invalid_release_head"
test -z "$(git status --porcelain)"

mkdir "$tmp/release-as-uppercase"
cd "$tmp/release-as-uppercase"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"release-as-uppercase","version":"1.0.0"}\n' > package.json
printf '{"name":"release-as-uppercase","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"release-as-uppercase","version":"1.0.0"}}}\n' > package-lock.json
git add package.json package-lock.json
git commit -qm 'chore: initialize uppercase release-as fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: exercise uppercase release-as'
uppercase_release_head=$(git rev-parse HEAD)
if uppercase_release_output=$("$bin" --release-as MAJOR 2>&1); then
  uppercase_release_status=0
else
  uppercase_release_status=$?
fi
test "$uppercase_release_status" -eq 0
expected_uppercase_release_output=$(printf '%s\n%s' \
  '✔ bumping version in package.json from 1.0.0 to null' \
  '✔ bumping version in package-lock.json from 1.0.0 to null')
test "$uppercase_release_output" = "$expected_uppercase_release_output"
printf '{\n  "name": "release-as-uppercase",\n  "version": null\n}\n' > "$tmp/release-as-uppercase.expected"
cmp "$tmp/release-as-uppercase.expected" package.json
printf '{\n  "name": "release-as-uppercase",\n  "version": null,\n  "lockfileVersion": 3,\n  "packages": {\n    "": {\n      "name": "release-as-uppercase",\n      "version": null\n    }\n  }\n}\n' > "$tmp/release-as-uppercase-lock.expected"
cmp "$tmp/release-as-uppercase-lock.expected" package-lock.json
test "$(git rev-parse HEAD)" = "$uppercase_release_head"
test "$(git tag --list)" = v1.0.0
test "$(git status --porcelain)" = ' M package-lock.json
 M package.json'

mkdir "$tmp/release-as-prerelease"
cd "$tmp/release-as-prerelease"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "release-as-prerelease",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize release-as prerelease fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: exercise release-as prerelease'
"$bin" --release-as 1.2.3 --prerelease alpha > /dev/null
grep -q '"version": "1.2.3-alpha.0"' package.json
test "$(git tag --list v1.2.3-alpha.0)" = v1.2.3-alpha.0
git commit --allow-empty -qm 'fix: exercise matching release-as prerelease identifier'
"$bin" --release-as 1.2.3-alpha.2 --prerelease alpha > /dev/null
grep -q '"version": "1.2.3-alpha.2"' package.json
test "$(git tag --list v1.2.3-alpha.2)" = v1.2.3-alpha.2
test -z "$(git status --porcelain)"

mkdir "$tmp/release-as-prerelease-conflict"
cd "$tmp/release-as-prerelease-conflict"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "release-as-prerelease-conflict",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize prerelease conflict fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: exercise conflicting prerelease identifier'
conflict_head=$(git rev-parse HEAD)
if conflict_output=$("$bin" --release-as 1.2.3-beta.0 --prerelease alpha 2>&1); then
  printf '%s\n' 'csemver accepted conflicting release-as and prerelease identifiers' >&2
  exit 1
else
  conflict_status=$?
fi
test "$conflict_status" -eq 1
printf '%s\n' "$conflict_output" | grep -Fq 'releaseAs and prerelease have conflicting prerelease identifiers'
test "$(git rev-parse HEAD)" = "$conflict_head"
grep -q '"version": "1.0.0"' package.json
test "$(git tag --list)" = v1.0.0
test ! -e CHANGELOG.md
if unlabeled_output=$("$bin" --release-as 1.2.3 --prerelease 2>&1); then
  printf '%s\n' 'csemver accepted an empty prerelease identifier with an exact release version' >&2
  exit 1
else
  unlabeled_status=$?
fi
test "$unlabeled_status" -eq 1
printf '%s\n' "$unlabeled_output" | grep -Fxq 'Invalid Version: 1.2.3-.0'
test "$(git rev-parse HEAD)" = "$conflict_head"
grep -q '"version": "1.0.0"' package.json
test "$(git tag --list)" = v1.0.0
test ! -e CHANGELOG.md
test -z "$(git status --porcelain)"

mkdir "$tmp/no-empty-bump"
cd "$tmp/no-empty-bump"
git init -q -b master
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

mkdir "$tmp/tag-force"
cd "$tmp/tag-force"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "tag-force",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize tag-force fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
printf 'first change\n' > feature.txt
git add feature.txt
git commit -qm 'fix: first tag-force change'
"$bin" --release-as 1.0.1 > /dev/null
old_tag_target=$(git rev-parse 'refs/tags/v1.0.1^{}')
printf 'second change\n' > followup.txt
git add followup.txt
git commit -qm 'fix: second tag-force change'
"$bin" --skip bump --tag-force > /dev/null
new_tag_target=$(git rev-parse 'refs/tags/v1.0.1^{}')
[ "$new_tag_target" != "$old_tag_target" ]
[ "$new_tag_target" = "$(git rev-parse HEAD)" ]
[ "$(grep -c '"version": "1.0.1"' package.json)" -eq 1 ]
test -z "$(git status --porcelain)"

mkdir "$tmp/no-verify"
cd "$tmp/no-verify"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "no-verify",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize no-verify fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
printf 'feature\n' > feature.txt
git add feature.txt
git commit -qm 'fix: add no-verify test feature'
hook_marker="$tmp/no-verify-hook-ran"
printf '#!/bin/sh\ntouch "%s"\nexit 1\n' "$hook_marker" > .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
if git commit --allow-empty -qm 'test: confirm pre-commit hook is active'; then
  printf '%s\n' 'pre-commit hook control unexpectedly succeeded' >&2
  exit 1
fi
test -f "$hook_marker"
rm -f "$hook_marker"
"$bin" --no-verify > /dev/null
test ! -e "$hook_marker"
grep -q '"version": "1.0.1"' package.json
test "$(git cat-file -t refs/tags/v1.0.1)" = tag
test -z "$(git status --porcelain)"

mkdir "$tmp/commit-all"
cd "$tmp/commit-all"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "commit-all",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize commit-all fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
printf 'feature\n' > feature.txt
git add feature.txt
git commit -qm 'fix: trigger commit-all release'
printf 'user-staged work\n' > STUFF.md
git add STUFF.md
"$bin" --commit-all > /dev/null
test -z "$(git status --porcelain)"
git show --pretty= --name-only HEAD | grep -Fxq STUFF.md
grep -q '"version": "1.0.1"' package.json
test "$(git cat-file -t refs/tags/v1.0.1)" = tag

mkdir "$tmp/lifecycle"
cd "$tmp/lifecycle"
git init -q -b master
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

mkdir "$tmp/lerna-package"
cd "$tmp/lerna-package"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '1.0.0\n' > VERSION
cat > csemver.toml <<'TOML'
packageFiles = [{ filename = "VERSION", type = "plain-text" }]
bumpFiles = [{ filename = "VERSION", type = "plain-text" }]
TOML
git add VERSION csemver.toml
git commit -qm 'feat: add the package baseline feature'
git tag -a '@scope/pkg@1.0.0' -m 'package release 1.0.0'
printf 'patch\n' > fix.txt
git add fix.txt
git commit -qm 'fix: correct package behavior'
lerna_dry_run=$("$bin" --dry-run --lerna-package '@scope/pkg')
printf '%s\n' "$lerna_dry_run" | grep -Fq 'bumping version in VERSION from 1.0.0' || {
  printf 'lerna package tag was not used as the bump boundary:\n%s\n' \
    "$lerna_dry_run" >&2
  exit 1
}
printf '%s\n' "$lerna_dry_run" | grep -Fxq ' to 1.0.1' || {
  printf 'lerna package bump report omitted the new version:\n%s\n' \
    "$lerna_dry_run" >&2
  exit 1
}
printf '%s\n' "$lerna_dry_run" | grep -q 'add the package baseline feature' || {
  printf 'lerna package bump boundary leaked into the global changelog:\n%s\n' \
    "$lerna_dry_run" >&2
  exit 1
}
printf 'feature\n' > feature.txt
git add feature.txt
git commit -qm 'feat: add feature since package release'
git tag -a '@scope/pkg@1.1.0-beta.0' -m 'package prerelease 1.1.0-beta.0'
printf 'follow-up fix\n' > follow-up.txt
git add follow-up.txt
git commit -qm 'fix: correct package after prerelease'
lerna_prerelease_dry_run=$("$bin" --dry-run --lerna-package '@scope/pkg')
printf '%s\n' "$lerna_prerelease_dry_run" | \
  grep -Fq 'bumping version in VERSION from 1.0.0' || {
    printf 'unstable package tag replaced the last stable bump boundary:\n%s\n' \
      "$lerna_prerelease_dry_run" >&2
    exit 1
  }
printf '%s\n' "$lerna_prerelease_dry_run" | grep -Fxq ' to 1.1.0' || {
  printf 'unstable package bump report omitted the new version:\n%s\n' \
    "$lerna_prerelease_dry_run" >&2
  exit 1
}

mkdir "$tmp/angular-preset"
cd "$tmp/angular-preset"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "angular-preset",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize angular preset fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git remote add origin 'https://github.com/blackopsrepl/angular-preset-fixture.git'
printf 'fast mode\n' > fast.txt
git add fast.txt
git commit -qm 'feat(api): add fast mode (#42)'
angular_feature_hash=$(git rev-parse HEAD)
git commit --allow-empty -qm 'fix(core): handle empty input (#52)'
git commit --allow-empty -qm 'perf(parser): cache tokens'
git commit --allow-empty -qm 'docs: explain the new mode'
git commit --allow-empty -m 'feat(api): change the public contract' \
  -m 'BREAKING CHANGE: callers must migrate'
git commit --allow-empty -qm 'feat!: Angular does not parse bang headers'
git revert --no-edit "$angular_feature_hash" > /dev/null
"$bin" --preset angular --release-as 1.1.0 --skip commit --skip tag > /dev/null
grep -q '^# \[1.1.0\]' CHANGELOG.md || {
  printf 'angular preset version heading should be level one for a minor release\n' >&2
  exit 1
}
if grep -q 'Angular does not parse bang headers' CHANGELOG.md; then
  printf 'angular preset included a commit that its header parser rejects\n' >&2
  exit 1
fi
if grep -q 'add fast mode' CHANGELOG.md; then
  printf 'angular preset failed to suppress the reverted commit pair\n' >&2
  exit 1
fi
for section in 'Bug Fixes' 'Features' 'Performance Improvements'; do
  grep -q "^### $section$" CHANGELOG.md || {
    printf 'angular preset omitted section: %s\n' "$section" >&2
    exit 1
  }
done
bug_line=$(grep -n '^### Bug Fixes$' CHANGELOG.md | cut -d: -f1)
feature_line=$(grep -n '^### Features$' CHANGELOG.md | cut -d: -f1)
perf_line=$(grep -n '^### Performance Improvements$' CHANGELOG.md | cut -d: -f1)
breaking_line=$(grep -n '^### BREAKING CHANGES$' CHANGELOG.md | cut -d: -f1)
test "$bug_line" -lt "$feature_line"
test "$feature_line" -lt "$perf_line"
test "$perf_line" -lt "$breaking_line"

mkdir "$tmp/angular-revert-after-release"
cd "$tmp/angular-revert-after-release"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "angular-revert-range",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize angular revert fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git remote add origin 'https://github.com/blackopsrepl/angular-revert-range-fixture.git'
printf 'fast mode\n' > fast.txt
git add fast.txt
git commit -qm 'feat(api): add fast mode'
angular_range_feature_hash=$(git rev-parse HEAD)
printf '{\n  "name": "angular-revert-range",\n  "version": "1.1.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore(release): 1.1.0'
git tag -a v1.1.0 -m 'chore(release): 1.1.0'
git revert --no-edit "$angular_range_feature_hash" > /dev/null
"$bin" --preset angular --release-as 1.1.1 --skip commit --skip tag > /dev/null
grep -q '^### Reverts$' CHANGELOG.md || {
  printf 'angular preset omitted a revert whose target is outside the changelog window\n' >&2
  exit 1
}
grep -q 'Revert "feat(api): add fast mode"' CHANGELOG.md || {
  printf 'angular preset changed the standard revert subject\n' >&2
  exit 1
}

if "$bin" --preset csemver-unsupported --dry-run > "$tmp/unsupported-preset.out" 2>&1; then
  printf 'unsupported changelog preset unexpectedly succeeded\n' >&2
  exit 1
fi
grep -q "unsupported changelog preset 'csemver-unsupported'" \
  "$tmp/unsupported-preset.out"

mkdir "$tmp/angular-bump"
cd "$tmp/angular-bump"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "angular-bump",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize angular bump fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
printf 'types = [{ type = "chore", effect = "hidden" }]\n' > csemver.toml
git add csemver.toml
git commit -qm 'chore: restrict the configured conventional types'
git commit --allow-empty -qm 'feat(core): add support for custom rules'
angular_bump_preview=$("$bin" --preset angular --dry-run)
printf '%s\n' "$angular_bump_preview" | \
  grep -q 'bumping version in package.json from 1.0.0 to 1.1.0' || {
    printf 'angular preset should ignore custom conventional types when bumping:\n%s\n' \
      "$angular_bump_preview" >&2
    exit 1
  }

mkdir "$tmp/cli-scripts"
cd "$tmp/cli-scripts"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "cli-scripts",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize lifecycle script fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: exercise CLI lifecycle scripts'
cli_script_marker="$tmp/cli-script.marker"
"$bin" --release-as 1.0.1 --scripts.posttag="touch $cli_script_marker" > /dev/null
test -f "$cli_script_marker"
git commit --allow-empty -qm 'fix: exercise an unknown lifecycle key'
unknown_script_marker="$tmp/unknown-script.marker"
"$bin" --release-as 1.0.2 --scripts.unknown="touch $unknown_script_marker" > /dev/null
test ! -e "$unknown_script_marker"

mkdir "$tmp/url-formats"
cd "$tmp/url-formats"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "url-formats",\n  "version": "1.0.0",\n  "repository": {"type": "git", "url": "https://github.com/example/url-formats.git"}\n}\n' > package.json
printf '%s\n' \
  'commitUrlFormat = "https://links.invalid/c/{{hash}}"' \
  'compareUrlFormat = "https://links.invalid/d/{{previousTag}}...{{currentTag}}"' \
  'issueUrlFormat = "https://links.invalid/i/{{id}}/{{prefix}}"' \
  'userUrlFormat = "https://links.invalid/u/{{user}}"' > csemver.toml
git add package.json csemver.toml
git commit -qm 'chore: initialize URL format fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -m 'feat(api): thank @alice for #42' -m 'Closes #99.'
url_format_commit_hash=$(git rev-parse HEAD)
"$bin" --release-as 1.1.0 --skip.commit --skip.tag > /dev/null
grep -Fq 'https://links.invalid/d/v1.0.0...v1.1.0' CHANGELOG.md
grep -Fq "https://links.invalid/c/$url_format_commit_hash" CHANGELOG.md
grep -Fq '[#42](https://links.invalid/i/42/#)' CHANGELOG.md
grep -Fq '[@alice](https://links.invalid/u/alice)' CHANGELOG.md
grep -Fq 'closes [#99](https://links.invalid/i/99/#)' CHANGELOG.md

mkdir "$tmp/pre-major"
cd "$tmp/pre-major"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "pre-major",\n  "version": "0.1.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize pre-major fixture'
git tag -a v0.1.0 -m 'chore(release): 0.1.0'
git commit --allow-empty -qm 'feat: exercise pre-major bump rules'
pre_major_preview=$("$bin" --dry-run --preMajor)
printf '%s\n' "$pre_major_preview" | \
  grep -q 'bumping version in package.json from 0.1.0 to 0.1.1' || {
    printf 'preMajor should apply pre-1.0.0 bump rules:\n%s\n' \
      "$pre_major_preview" >&2
    exit 1
  }
default_pre_major_preview=$("$bin" --dry-run)
printf '%s\n' "$default_pre_major_preview" | \
  grep -q 'bumping version in package.json from 0.1.0 to 0.1.1' || {
    printf 'upstream should automatically apply pre-major rules below 1.0.0:\n%s\n' \
      "$default_pre_major_preview" >&2
    exit 1
  }
camel_case_preview=$("$bin" --dryRun --releaseAs=patch)
printf '%s\n' "$camel_case_preview" | \
  grep -q 'bumping version in package.json from 0.1.0 to 0.1.1' || {
    printf 'camelCase dry-run and release-as options were not applied:\n%s\n' \
      "$camel_case_preview" >&2
    exit 1
  }
grep -Fq '"version": "0.1.0"' package.json
types_override_preview=$("$bin" --dry-run --types=feat --silent)
printf '%s\n' "$types_override_preview" | grep -Fq '## [0.1.1]' || {
    printf 'types array should replace the default commit types:\n%s\n' \
      "$types_override_preview" >&2
    exit 1
}
if printf '%s\n' "$types_override_preview" | grep -Fq '### Features'; then
    printf 'string entries in upstream types array should not match commit objects\n' >&2
    exit 1
fi
git commit --allow-empty -m 'feat!: change the contract' -m 'BREAKING CHANGE: incompatible API.'
breaking_preview=$("$bin" --dry-run)
printf '%s\n' "$breaking_preview" | \
  grep -q 'bumping version in package.json from 0.1.0 to 0.2.0' || {
    printf 'upstream should bump a 0.x breaking change to minor:\n%s\n' \
      "$breaking_preview" >&2
    exit 1
  }

mkdir "$tmp/pre-major-1x"
cd "$tmp/pre-major-1x"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "pre-major-1x",\n  "version": "1.0.0"\n}\n' > package.json
git add package.json
git commit -qm 'chore: initialize pre-major 1.x fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'feat: test explicit pre-major option'
printf 'firstRelease = true\n' > csemver.toml
no_first_release_preview=$("$bin" --dry-run --no-firstRelease)
printf '%s\n' "$no_first_release_preview" | \
  grep -q 'bumping version in package.json from 1.0.0 to 1.1.0' || {
    printf 'negative firstRelease option should override TOML true:\n%s\n' \
      "$no_first_release_preview" >&2
    exit 1
  }
explicit_pre_major_preview=$("$bin" --dry-run --no-firstRelease --preMajor)
printf '%s\n' "$explicit_pre_major_preview" | \
  grep -q 'bumping version in package.json from 1.0.0 to 1.0.1' || {
    printf 'explicit preMajor should apply below a future major release:\n%s\n' \
      "$explicit_pre_major_preview" >&2
    exit 1
  }

mkdir "$tmp/repeated-bump-files"
cd "$tmp/repeated-bump-files"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "repeated-bump-files",\n  "version": "1.0.0"\n}\n' > package.json
printf '{\n  "name": "repeated-bump-files",\n  "version": "1.0.0"\n}\n' > bower.json
git add package.json bower.json
git commit -qm 'chore: initialize repeated bump files fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'feat: exercise repeated bumpFiles flags GH-42 #8'
repeated_bump_preview=$("$bin" --dry-run --bumpFiles package.json --bumpFiles bower.json)
printf '%s\n' "$repeated_bump_preview" | \
  grep -q 'bumping version in package.json from 1.0.0 to 1.1.0' || {
    printf 'repeated bumpFiles options should retain the first file:\n%s\n' \
      "$repeated_bump_preview" >&2
    exit 1
  }
printf '%s\n' "$repeated_bump_preview" | \
  grep -q 'bumping version in bower.json from 1.0.0 to 1.1.0' || {
    printf 'repeated bumpFiles options should retain the second file:\n%s\n' \
      "$repeated_bump_preview" >&2
    exit 1
  }
mkdir "$tmp/plain-text-bump-file"
cd "$tmp/plain-text-bump-file"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "plain-text-bump-file",
  "version": "1.0.0"
}
JSON
printf '2.0.0\n' > version.txt
git add package.json version.txt
git commit -qm 'chore: initialize plain-text bump file fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'fix: exercise plain-text bump file'
plain_text_bump_preview=$("$bin" --dry-run --bumpFiles version.txt)
printf '%s\n' "$plain_text_bump_preview" | \
  grep -Fq 'bumping version in version.txt from 2.0.0' || {
    printf '%s\n' 'plain-text bumpFiles should print the original file contents' >&2
    exit 1
  }
printf '%s\n' "$plain_text_bump_preview" | grep -Fxq ' to 1.0.1' || {
  printf '%s\n' 'plain-text bumpFiles should preserve its embedded newline in output' >&2
  exit 1
}

cd "$tmp/repeated-bump-files"
repeated_prefix_preview=$("$bin" --dry-run --issuePrefixes GH- --issuePrefixes '#' \
  --issueUrlFormat='https://issues.example/{{id}}')
for issue_url in 'https://issues.example/42' 'https://issues.example/8'; do
  printf '%s\n' "$repeated_prefix_preview" | grep -Fq "$issue_url" || {
    printf 'repeated issuePrefixes options should retain both prefixes (%s):\n%s\n' \
      "$issue_url" "$repeated_prefix_preview" >&2
    exit 1
  }
done

mkdir "$tmp/repeated-package-files"
cd "$tmp/repeated-package-files"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{\n  "name": "repeated-package-files",\n  "version": "1.0.0"\n}\n' > package.json
printf '{\n  "name": "repeated-package-files",\n  "version": "2.0.0"\n}\n' > bower.json
git add package.json bower.json
git commit -qm 'chore: initialize repeated package files fixture'
git tag -a v1.0.0 -m 'chore(release): 1.0.0'
git commit --allow-empty -qm 'feat: test repeated packageFiles'
repeated_package_preview=$("$bin" --dry-run --packageFiles package.json --packageFiles bower.json)
printf '%s\n' "$repeated_package_preview" | \
  grep -q 'bumping version in package.json from 1.0.0 to 1.1.0' || {
    printf 'repeated packageFiles should read the first package version:\n%s\n' \
      "$repeated_package_preview" >&2
    exit 1
  }
printf '%s\n' "$repeated_package_preview" | \
  grep -q 'bumping version in bower.json from 2.0.0 to 1.1.0' || {
    printf 'repeated packageFiles should update the second package file:\n%s\n' \
      "$repeated_package_preview" >&2
    exit 1
  }
printf '%s\n' "$repeated_package_preview" | \
  grep -Fq '✔ committing bower.json and package.json and CHANGELOG.md' || {
    printf 'commit path summary should reverse updated files before the changelog:\n%s\n' \
      "$repeated_package_preview" >&2
    exit 1
  }
legacy_warning_output=$("$bin" --dry-run --message 'chore(release): %s' \
  --changelogHeader 'Custom header' 2>&1)
for warning in \
  '[commit-and-tag-version]: --message (-m) will be removed in the next major release. Use --releaseCommitMessageFormat.' \
  '[commit-and-tag-version]: --changelogHeader will be removed in the next major release. Use --header.'; do
  printf '%s\n' "$legacy_warning_output" | grep -Fq "$warning" || {
    printf 'upstream compatibility warning was missing (%s):\n%s\n' \
      "$warning" "$legacy_warning_output" >&2
    exit 1
  }
done
silent_legacy_output=$("$bin" --dry-run --message 'chore(release): %s' \
  --changelogHeader 'Custom header' --silent 2>&1)
if printf '%s\n' "$silent_legacy_output" | grep -Fq '[commit-and-tag-version]:'; then
  printf 'silent should suppress legacy-option warnings:\n%s\n' \
    "$silent_legacy_output" >&2
  exit 1
fi
header_precedence_output=$("$bin" --changelogHeader 'Legacy header' \
  --header 'Modern header' --skip.commit --skip.tag 2>&1)
grep -Fq 'Legacy header' CHANGELOG.md || {
  printf 'changelogHeader should override header regardless of CLI order:\n%s\n' \
    "$header_precedence_output" >&2
  exit 1
}
if grep -Fq 'Modern header' CHANGELOG.md; then
  printf 'the later header option should not override changelogHeader:\n%s\n' \
    "$header_precedence_output" >&2
  exit 1
fi

mkdir "$tmp/pkg-config"
cd "$tmp/pkg-config"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-config-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-config.git"},
  "standard-version": {"tagPrefix": "legacy-"},
  "commit-and-tag-version": {"tagPrefix": "release-"}
}
JSON
git add package.json
git commit -qm 'chore: seed package config fixture'
git tag -a legacy-1.0.0 -m 'legacy release'
git tag -a release-1.0.0 -m 'release'
git commit --allow-empty -qm 'feat: use package config'
pkg_config_output=$("$bin" --skip.commit --skip.tag 2>&1)
grep -Fq 'compare/legacy-1.0.0...legacy-1.1.0' CHANGELOG.md || {
  printf 'package.json config should preserve upstream section precedence:\n%s\n' \
    "$pkg_config_output" >&2
  exit 1
}

mkdir "$tmp/pkg-array-config"
cd "$tmp/pkg-array-config"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-array-config-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-array-config.git"},
  "commit-and-tag-version": {
    "packageFiles": ["package.json", "bower.json"],
    "issuePrefixes": ["JIRA-", "GH-"],
    "issueUrlFormat": "https://issues.example/{{prefix}}{{id}}"
  }
}
JSON
cat > bower.json <<'JSON'
{
  "name": "pkg-array-config-fixture",
  "version": "2.0.0"
}
JSON
git add package.json bower.json
git commit -qm 'chore: seed package array config fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: address JIRA-7 and GH-42'
pkg_array_output=$("$bin" --skip.commit --skip.tag 2>&1)
grep -Fq '"version": "1.0.1"' bower.json || {
  printf '%s\n' 'package.json packageFiles should update bower.json to the release version' >&2
  exit 1
}
for issue_ref in '[JIRA-7](https://issues.example/JIRA-7)' \
  '[GH-42](https://issues.example/GH-42)'; do
  grep -Fq "$issue_ref" CHANGELOG.md || {
    printf 'package.json issuePrefixes missing link: %s\n' "$issue_ref" >&2
    exit 1
  }
done

mkdir "$tmp/pkg-bool-config"
cd "$tmp/pkg-bool-config"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-bool-config-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {"dryRun": true}
}
JSON
git add package.json
git commit -qm 'chore: seed package boolean config fixture'
git commit --allow-empty -qm 'feat: verify package boolean config'
package_bool_output=$("$bin" 2>&1)
test ! -e CHANGELOG.md || {
  printf 'package.json dryRun should prevent release writes:\n%s\n' \
    "$package_bool_output" >&2
  exit 1
}
printf '%s\n' "$package_bool_output" | grep -Fq '## 1.1.0 (' || {
  printf 'release preview without a prior tag should omit compare-link brackets:\n%s\n' \
    "$package_bool_output" >&2
  exit 1
}
grep -Fq '"version": "1.0.0"' package.json || {
  printf 'package.json dryRun should leave the package version unchanged:\n%s\n' \
    "$package_bool_output" >&2
  exit 1
}

mkdir "$tmp/pkg-number-config"
cd "$tmp/pkg-number-config"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-number-config-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-number-config.git"},
  "commit-and-tag-version": {"releaseCount": 0}
}
JSON
git add package.json
git commit -qm 'chore: seed package number config fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
cat > package.json <<'JSON'
{
  "name": "pkg-number-config-fixture",
  "version": "1.1.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-number-config.git"},
  "commit-and-tag-version": {"releaseCount": 0}
}
JSON
git add package.json
git commit -qm 'feat: release 1.1.0'
git tag -a v1.1.0 -m 'release 1.1.0'
git commit --allow-empty -qm 'fix: current change'
"$bin" --skip.commit --skip.tag > /dev/null
grep -Fq '## 1.0.0 (' CHANGELOG.md || {
  printf '%s\n' 'package.json releaseCount should retain full history' >&2
  exit 1
}
package_header_gap=$(awk '
  /^All notable changes/ { in_header = 1; next }
  in_header && NF == 0 { blanks++; next }
  in_header { print blanks; exit }
' CHANGELOG.md)
test "$package_header_gap" -eq 2 || {
  printf '%s\n' 'releaseCount=0 should leave two blank lines before the first heading' >&2
  exit 1
}

mkdir "$tmp/pkg-bumpfiles-config"
cd "$tmp/pkg-bumpfiles-config"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-bumpfiles-config-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-bumpfiles-config.git"},
  "commit-and-tag-version": {
    "bumpFiles": [{"filename": "VERSION", "type": "plain-text"}]
  }
}
JSON
printf '1.0.0\n' > VERSION
git add package.json VERSION
git commit -qm 'chore: seed package bumpFiles config fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
printf 'new feature\n' > feature.txt
git add feature.txt
git commit -qm 'feat: add configured version file'
"$bin" --skip.commit --skip.tag > /dev/null
test "$(cat VERSION)" = '1.1.0' || {
  printf '%s\n' 'package.json bumpFiles object should update its configured plain-text file' >&2
  exit 1
}
grep -Fq '"version": "1.0.0"' package.json || {
  printf '%s\n' 'package.json bumpFiles override should not rewrite the package file' >&2
  exit 1
}

mkdir "$tmp/pkg-bumpfiles-strings"
cd "$tmp/pkg-bumpfiles-strings"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-bumpfiles-strings-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-bumpfiles-strings.git"},
  "commit-and-tag-version": {"bumpFiles": ["version.txt"]}
}
JSON
printf '1.0.0\n' > version.txt
git add package.json version.txt
git commit -qm 'chore: seed package bumpFiles string fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
printf 'new feature\n' > feature.txt
git add feature.txt
git commit -qm 'feat: add configured string version file'
"$bin" --skip.commit --skip.tag > /dev/null
test "$(cat version.txt)" = '1.1.0' || {
  printf '%s\n' 'package.json bumpFiles string should update its recognized plain-text file' >&2
  exit 1
}
grep -Fq '"version": "1.0.0"' package.json || {
  printf '%s\n' 'package.json bumpFiles string should not rewrite the package file' >&2
  exit 1
}

mkdir "$tmp/pkg-bumpfiles-unsupported"
cd "$tmp/pkg-bumpfiles-unsupported"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-bumpfiles-unsupported-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-bumpfiles-unsupported.git"},
  "commit-and-tag-version": {"bumpFiles": ["metadata.json"]}
}
JSON
cat > metadata.json <<'JSON'
{
  "version": "2.0.0"
}
JSON
git add package.json metadata.json
git commit -qm 'chore: seed unsupported package bump file fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: change with unsupported bump file'
if unsupported_output=$("$bin" --skip.commit --skip.tag 2>&1); then
  :
else
  printf '%s\n' 'unsupported package bumpFiles should be skipped like upstream' >&2
  printf '%s\n' "$unsupported_output" >&2
  exit 1
fi
printf '%s\n' "$unsupported_output" | grep -Fxq 'Unable to obtain updater for: "metadata.json"'
printf '%s\n' "$unsupported_output" | grep -Fxq ' - Error: Unsupported file (metadata.json) provided for bumping.'
printf '%s\n' "$unsupported_output" | grep -Fxq ' Please specify the updater `type` or use a custom `updater`.'
printf '%s\n' "$unsupported_output" | grep -Fxq ' - Skipping...'
grep -Fq '"version": "1.0.0"' package.json
grep -Fq '"version": "2.0.0"' metadata.json
test -f CHANGELOG.md

mkdir "$tmp/pkg-custom-types"
cd "$tmp/pkg-custom-types"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-custom-types-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-custom-types.git"},
  "commit-and-tag-version": {
    "types": [
      {"type": "feature", "section": "Custom Features", "hidden": false},
      {"type": "docs", "section": "Documentation", "hidden": false}
    ]
  }
}
JSON
git add package.json
git commit -qm 'chore: seed package custom types fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
for message in 'feature: custom feature' 'docs: update manual' 'feat: default feature'; do
  git commit --allow-empty -qm "$message"
done
"$bin" --skip.commit --skip.tag >/dev/null
grep -Fq '### Custom Features' CHANGELOG.md
grep -Fq '### Documentation' CHANGELOG.md
if grep -Fq '### Features' CHANGELOG.md; then
  printf '%s\n' 'package-defined commit types should replace the default type list' >&2
  exit 1
fi
if grep -Fq 'default feature' CHANGELOG.md; then
  printf '%s\n' 'commit types absent from the package-defined list should be omitted' >&2
  exit 1
fi

mkdir "$tmp/pkg-package-files-object"
cd "$tmp/pkg-package-files-object"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-package-files-object-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-package-files-object.git"},
  "commit-and-tag-version": {
    "packageFiles": [{"filename": "manifest.data", "type": "json"}]
  }
}
JSON
cat > manifest.data <<'JSON'
{
    "name": "manifest",
    "version": "2.3.4"
}
JSON
git add package.json manifest.data
git commit -qm 'chore: seed packageFiles object fixture'
git tag -a v2.3.4 -m 'release 2.3.4'
git commit --allow-empty -qm 'feat: update object package file'
"$bin" --skip.commit --skip.tag >/dev/null
grep -Fq '"version": "2.4.0"' package.json
grep -Fq '"version": "2.3.4"' manifest.data
grep -Fq 'compare/v2.3.4...v2.4.0' CHANGELOG.md

mkdir "$tmp/pkg-package-files-cli"
cd "$tmp/pkg-package-files-cli"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '%s\n' '{"name":"pkg-package-files-cli-fixture","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/pkg-package-files-cli.git"}}' > package.json
printf '%s\n' '2.3.4' > VERSION.txt
git add package.json VERSION.txt
git commit -qm 'chore: seed packageFiles CLI fixture'
git tag -a v2.3.4 -m 'release 2.3.4'
git commit --allow-empty -qm 'feat: update command line package file'
"$bin" --skip.commit --skip.tag --packageFiles VERSION.txt >/dev/null
grep -Fq '"version": "2.4.0"' package.json
test "$(cat VERSION.txt)" = '2.3.4'
grep -Fq 'compare/v2.3.4...v2.4.0' CHANGELOG.md

mkdir "$tmp/pkg-skip-config"
cd "$tmp/pkg-skip-config"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-skip-config-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-skip-config.git"},
  "commit-and-tag-version": {"skip": {"changelog": true}}
}
JSON
git add package.json
git commit -qm 'chore: seed package skip fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: release with changelog skipped'
"$bin" --skip.commit --skip.tag >/dev/null
test ! -e CHANGELOG.md

mkdir "$tmp/pkg-skip-precedence"
cd "$tmp/pkg-skip-precedence"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-skip-precedence-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-skip-precedence.git"},
  "commit-and-tag-version": {"skip": {"changelog": true}},
  "standard-version": {"skip": {"changelog": false}}
}
JSON
git add package.json
git commit -qm 'chore: seed package skip precedence fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: release with package config precedence'
"$bin" --skip.commit --skip.tag >/dev/null
test -e CHANGELOG.md

mkdir "$tmp/pkg-lifecycle-scripts"
cd "$tmp/pkg-lifecycle-scripts"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
package_script_marker="$tmp/package-script-ran"
package_script_superseded_marker="$tmp/package-script-superseded"
package_script_command="touch $package_script_marker"
package_script_superseded_command="touch $package_script_superseded_marker"
cat > package.json <<JSON
{
  "name": "pkg-lifecycle-scripts-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {"prerelease": "$package_script_superseded_command"}
  },
  "standard-version": {
    "scripts": {"prerelease": "$package_script_command"}
  }
}
JSON
git add package.json
git commit -qm 'chore: seed package lifecycle script fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise package lifecycle script'
package_script_output=$("$bin" --skip.commit --skip.tag)
test -f "$package_script_marker"
test ! -e "$package_script_superseded_marker"
printf '%s\n' "$package_script_output" |
  grep -Fxq '✔ Running lifecycle script "prerelease"'
printf '%s\n' "$package_script_output" |
  grep -Fxq "ℹ - execute command: \"$package_script_command\""
rm -f "$package_script_marker" "$package_script_superseded_marker"
package_script_output=$("$bin" --silent --skip.commit --skip.tag)
test -f "$package_script_marker"
test -z "$package_script_output"

mkdir "$tmp/dry-run-lifecycle-scripts"
cd "$tmp/dry-run-lifecycle-scripts"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<JSON
{
  "name": "dry-run-lifecycle-scripts-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {
      "prerelease": "touch $tmp/dryrun-prerelease",
      "prebump": "touch $tmp/dryrun-prebump",
      "postbump": "touch $tmp/dryrun-postbump",
      "prechangelog": "touch $tmp/dryrun-prechangelog",
      "postchangelog": "touch $tmp/dryrun-postchangelog",
      "precommit": "touch $tmp/dryrun-precommit",
      "postcommit": "touch $tmp/dryrun-postcommit",
      "pretag": "touch $tmp/dryrun-pretag",
      "posttag": "touch $tmp/dryrun-posttag"
    }
  }
}
JSON
git add package.json
git commit -qm 'chore: seed dry-run lifecycle fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise dry-run lifecycle scripts'
dry_run_script_output=$("$bin" --dry-run)
expected_dry_run_hooks=$(printf '✔ Running lifecycle script "%s"\n' \
  prerelease prebump postbump prechangelog postchangelog precommit postcommit \
  pretag posttag)
actual_dry_run_hooks=$(printf '%s\n' "$dry_run_script_output" |
  command grep -F '✔ Running lifecycle script')
test "$actual_dry_run_hooks" = "$expected_dry_run_hooks"
expected_dry_run_stages=$(printf '%s\n' \
  '✔ Running lifecycle script "prerelease"' \
  '✔ Running lifecycle script "prebump"' \
  '✔ Running lifecycle script "postbump"' \
  '✔ Running lifecycle script "prechangelog"' \
  '✔ Running lifecycle script "postchangelog"' \
  '✔ Running lifecycle script "precommit"' \
  '✔ committing package.json and CHANGELOG.md' \
  '✔ Running lifecycle script "postcommit"' \
  '✔ Running lifecycle script "pretag"' \
  '✔ tagging release v1.1.0' \
  'ℹ Run `git push --follow-tags origin master && npm publish` to publish' \
  '✔ Running lifecycle script "posttag"')
actual_dry_run_stages=$(printf '%s\n' "$dry_run_script_output" |
  command grep -E 'Running lifecycle script|✔ committing|✔ tagging release|Run `git push')
test "$actual_dry_run_stages" = "$expected_dry_run_stages"
for hook in prerelease prebump postbump prechangelog postchangelog precommit \
  postcommit pretag posttag; do
  printf '%s\n' "$dry_run_script_output" |
    grep -Fq "✔ Running lifecycle script \"$hook\""
  test ! -e "$tmp/dryrun-$hook"
done
test -z "$(git status --porcelain)"

mkdir "$tmp/normal-lifecycle-scripts"
cd "$tmp/normal-lifecycle-scripts"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "normal-lifecycle-scripts-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {
      "prerelease": "true",
      "prebump": "true",
      "postbump": "true",
      "prechangelog": "true",
      "postchangelog": "true",
      "precommit": "true",
      "postcommit": "true",
      "pretag": "true",
      "posttag": "true"
    }
  }
}
JSON
git add package.json
git commit -qm 'chore: seed normal lifecycle fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise normal lifecycle scripts'
normal_lifecycle_output=$("$bin")
actual_normal_stages=$(printf '%s\n' "$normal_lifecycle_output" |
  command grep -E 'Running lifecycle script|✔ committing|✔ tagging release|Run `git push')
test "$actual_normal_stages" = "$expected_dry_run_stages"
test -z "$(git status --porcelain)"

mkdir "$tmp/no-package-fallback-disabled"
cd "$tmp/no-package-fallback-disabled"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf 'seed\n' > README.md
git add README.md
git commit -qm 'chore: seed without a version file'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise disabled tag fallback'
head_before=$(git rev-parse HEAD)
if fallback_error=$("$bin" --no-git-tag-fallback 2>&1); then
  printf '%s\n' 'csemver must reject missing version files when tag fallback is disabled' >&2
  exit 1
else
  fallback_status=$?
fi
test "$fallback_status" -eq 1
test "$fallback_error" = 'no package file found'
if fallback_silent_output=$("$bin" --silent --no-git-tag-fallback 2>&1); then
  printf '%s\n' 'csemver must still fail with silent output suppressed' >&2
  exit 1
else
  fallback_silent_status=$?
fi
test "$fallback_silent_status" -eq 1
test -z "$fallback_silent_output"
test "$(git rev-parse HEAD)" = "$head_before"
test "$(git tag --list)" = 'v1.0.0'
test ! -e CHANGELOG.md
fallback_preview=$("$bin" --dry-run)
printf '%s\n' "$fallback_preview" |
  grep -Fq 'ℹ Run `git push --follow-tags origin master` to publish' || {
  printf '%s\n' 'fallback without package.json must still show the Git push hint' >&2
  exit 1
}
if printf '%s\n' "$fallback_preview" | grep -Fq 'npm publish'; then
  printf '%s\n' 'fallback without package.json must not suggest npm publish' >&2
  exit 1
fi
skip_bump_preview=$("$bin" --dry-run --skip.bump)
printf '%s\n' "$skip_bump_preview" |
  grep -Fq 'ℹ Run `git push --follow-tags origin master` to publish' || {
  printf '%s\n' 'a release that skips version updates must still show the Git push hint' >&2
  exit 1
}
if printf '%s\n' "$skip_bump_preview" | grep -Fq 'npm publish'; then
  printf '%s\n' 'a release that skips version updates must not suggest npm publish' >&2
  exit 1
fi
if printf '%s\n' "$skip_bump_preview" | grep -Fq 'exercise disabled tag fallback' ||
   printf '%s\n' "$skip_bump_preview" | grep -Fq '## [1.0.0]'; then
  printf '%s\n' 'a preview reusing the latest tag must not duplicate its changelog section' >&2
  exit 1
fi

mkdir "$tmp/tag-fallback-enabled"
cd "$tmp/tag-fallback-enabled"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
git remote add origin https://github.com/example/csemver.git
printf 'seed\n' > README.md
git add README.md
git commit -qm 'chore: seed without a version file'
git tag -a android/production/v1.0.0 -m 'release 1.0.0'
git tag -a android/production/v1.2.0 -m 'release 1.2.0'
git commit --allow-empty -qm 'feat: release from the latest prefixed tag'
"$bin" --tag-prefix android/production/v > /dev/null
test "$(git tag --list 'android/production/v1.3.0')" = 'android/production/v1.3.0'
test "$(git rev-parse 'refs/tags/android/production/v1.3.0^{}')" = "$(git rev-parse HEAD)"
test ! -e package.json
grep -Fq '## [1.3.0](https://github.com/example/csemver/compare/android/production/v1.2.0...android/production/v1.3.0)' CHANGELOG.md
test -z "$(git status --porcelain)"

mkdir "$tmp/tag-fallback-no-tags"
cd "$tmp/tag-fallback-no-tags"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
git commit --allow-empty -qm 'feat: release without package or tags'
"$bin" > /dev/null
test "$(git tag --list v1.1.0)" = v1.1.0
test "$(git cat-file -t refs/tags/v1.1.0)" = tag
test ! -e package.json
test -z "$(git status --porcelain)"

mkdir "$tmp/malformed-package-lock"
cd "$tmp/malformed-package-lock"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{\n  "name": "malformed-lock-fixture",\n  "version": "1.0.0"\n}\n' > package.json
: > package-lock.json
git add package.json package-lock.json
git commit -qm 'chore: initialize malformed lock fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise malformed lock handling'
malformed_lock_preview=$("$bin" --dry-run 2>&1)
printf '%s\n' "$malformed_lock_preview" | grep -Fq 'Unexpected end of JSON input' || {
  printf '%s\n' 'dry-run must report and skip an invalid package-lock.json' >&2
  exit 1
}
printf '{\n' > package-lock.json
truncated_lock_preview=$("$bin" --dry-run 2>&1)
printf '%s\n' "$truncated_lock_preview" |
  grep -Fq "Expected property name or '}' in JSON at position 2 (line 2 column 1)" || {
  printf '%s\n' 'dry-run must preserve the JSON parser error for a truncated lockfile' >&2
  exit 1
}
: > package-lock.json
if printf '%s\n' "$malformed_lock_preview" | grep -Fq 'committing package-lock.json'; then
  printf '%s\n' 'dry-run must not include an invalid package-lock.json in the commit path' >&2
  exit 1
fi
malformed_lock_output=$("$bin" --skip.commit --skip.tag 2>&1) || {
  printf '%s\n' 'a malformed package-lock.json must not abort the release' >&2
  printf '%s\n' "$malformed_lock_output" >&2
  exit 1
}
printf '%s\n' "$malformed_lock_output" | grep -Fq 'Unexpected end of JSON input' || {
  printf '%s\n' 'release must report and skip an invalid package-lock.json' >&2
  exit 1
}
grep -Fq '"version": "1.1.0"' package.json
test ! -s package-lock.json

mkdir "$tmp/missing-package-lock-version"
cd "$tmp/missing-package-lock-version"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{\n  "name": "missing-lock-version-fixture",\n  "version": "1.0.0"\n}\n' > package.json
cat > package-lock.json <<'JSON'
{
  "name": "missing-lock-version-fixture",
  "lockfileVersion": 3,
  "requires": true,
  "packages": {
    "": {
      "name": "missing-lock-version-fixture"
    }
  }
}
JSON
git add package.json package-lock.json
git commit -qm 'chore: initialize missing lock version fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise missing lock version handling'
missing_lock_preview=$("$bin" --dry-run)
printf '%s\n' "$missing_lock_preview" |
  grep -Fq '✔ bumping version in package-lock.json from undefined to 1.1.0'
if grep -Fq '"version": "1.1.0"' package-lock.json; then
  printf '%s\n' 'dry-run must not mutate a lockfile missing its version' >&2
  exit 1
fi
test -z "$(git status --porcelain)"
missing_lock_output=$("$bin" --skip.commit --skip.tag)
printf '%s\n' "$missing_lock_output" |
  grep -Fq '✔ bumping version in package-lock.json from undefined to 1.1.0'
test "$(grep -c '"version": "1.1.0"' package-lock.json)" -eq 2

mkdir "$tmp/invalid-json-package-lock"
cd "$tmp/invalid-json-package-lock"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{\n  "name": "invalid-json-lock-fixture",\n  "version": "1.0.0"\n}\n' > package.json
cat > package-lock.json <<'JSON'
{
  "name": undefined,
  "lockfileVersion": 3,
  "requires": true,
  "packages": {
    "": {"name": "invalid-json-lock-fixture"}
  }
}
JSON
cp package-lock.json "$tmp/invalid-json-lock.expected"
git add package.json package-lock.json
git commit -qm 'chore: initialize invalid JSON lock fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise strict JSON parsing'
invalid_json_lock_preview=$("$bin" --dry-run 2>&1)
if printf '%s\n' "$invalid_json_lock_preview" |
  grep -Fq 'bumping version in package-lock.json'; then
  printf '%s\n' 'dry-run must reject a package-lock with invalid JSON values' >&2
  exit 1
fi
if printf '%s\n' "$invalid_json_lock_preview" |
  grep -Fq 'committing package-lock.json'; then
  printf '%s\n' 'dry-run must omit an invalid JSON lockfile from the commit path' >&2
  exit 1
fi
test -z "$(git status --porcelain)"
invalid_json_lock_output=$("$bin" --skip.commit --skip.tag 2>&1)
printf '%s\n' "$invalid_json_lock_output" |
  grep -Fq "Unexpected token 'u', ...\"  \"name\": undefined,\"... is not valid JSON"
cmp -s package-lock.json "$tmp/invalid-json-lock.expected"

mkdir "$tmp/private-package-hint"
cd "$tmp/private-package-hint"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "private-package-hint-fixture",
  "version": "1.0.0",
  "private": true
}
JSON
git add package.json
git commit -qm 'chore: seed private package'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise private package hint'
private_preview=$("$bin" --dry-run)
printf '%s\n' "$private_preview" |
  grep -Fq 'ℹ Run `git push --follow-tags origin master` to publish' || {
  printf '%s\n' 'private package must still show the Git push hint' >&2
  exit 1
}
if printf '%s\n' "$private_preview" | grep -Fq 'npm publish'; then
  printf '%s\n' 'private package must not suggest npm publish' >&2
  exit 1
fi

test -z "$(git status --porcelain)"

printf '%s\n' 'release workflow tests passed'
