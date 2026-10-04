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
regenerated_header_line=$(grep -m 1 -n 'for commit guidelines\.' CHANGELOG.md | cut -d: -f1)
regenerated_latest_line=$(grep -m 1 -n '^## \[2.0.1\]' CHANGELOG.md | cut -d: -f1)
test "$((regenerated_latest_line - regenerated_header_line))" -eq 3
initial_release_line=$(grep -n '^## 1\.0\.0 (' CHANGELOG.md | cut -d: -f1)
preserved_release_line=$(grep -n '^## \[2\.0\.1\]' CHANGELOG.md | tail -n 1 | cut -d: -f1)
[ "$((preserved_release_line - initial_release_line))" -eq 3 ]
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
printf 'first release feature\n' > first-release-feature.txt
git add first-release-feature.txt
git commit -qm 'feat: include first-release section'
"$bin" --first-release > /dev/null
first_release_heading=$(grep -m 1 '^## ' CHANGELOG.md)
case "$first_release_heading" in
  '## 1.0.0 ('*) ;;
  *) printf 'first release heading is not in upstream format: %s\n' "$first_release_heading" >&2; exit 1 ;;
esac
first_release_heading_line=$(grep -m 1 -n '^## 1.0.0 (' CHANGELOG.md | cut -d: -f1)
first_release_section_line=$(grep -m 1 -n '^### Features$' CHANGELOG.md | cut -d: -f1)
test "$((first_release_section_line - first_release_heading_line))" -eq 2
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

mkdir "$tmp/prerelease-empty-id-tag-fallback"
cd "$tmp/prerelease-empty-id-tag-fallback"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{"name":"prerelease-empty-id-tag-fallback","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/prerelease-empty-id-tag-fallback.git"},"commit-and-tag-version":{"packageFiles":[]}}\n' > package.json
git add package.json
git commit -qm 'chore: seed unnamed prerelease fallback'
git tag -a v1.2.3-beta.0 -m 'release 1.2.3-beta.0'
git commit --allow-empty -qm 'feat: promote prerelease by feature bump'
if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
  --prerelease > "$tmp/prerelease-empty-id-tag-fallback.stdout" \
  2> "$tmp/prerelease-empty-id-tag-fallback.stderr"; then
  empty_prerelease_status=0
else
  empty_prerelease_status=$?
fi
test "$empty_prerelease_status" -eq 0
printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 1.3.0-0' \
  > "$tmp/prerelease-empty-id-tag-fallback.expected.stdout"
cmp "$tmp/prerelease-empty-id-tag-fallback.expected.stdout" \
  "$tmp/prerelease-empty-id-tag-fallback.stdout"
test ! -s "$tmp/prerelease-empty-id-tag-fallback.stderr"
test -z "$(git status --porcelain)"

run_named_prerelease_case() {
  case_name=$1
  commit_message=$2
  expected_version=$3
  tag_version=${4:-1.2.3-beta.0}
  case_dir="$tmp/prerelease-named-$case_name"
  mkdir "$case_dir"
  cd "$case_dir"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  printf '{"name":"prerelease-named-%s","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/prerelease-named-%s.git"},"commit-and-tag-version":{"packageFiles":[]}}\n' \
    "$case_name" "$case_name" > package.json
  git add package.json
  git commit -qm 'chore: seed named prerelease progression'
  git tag -a "v$tag_version" -m "release $tag_version"
  git commit --allow-empty -qm "$commit_message"
  if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
    --prerelease beta > "$case_dir.stdout" 2> "$case_dir.stderr"; then
    case_status=0
  else
    case_status=$?
  fi
  test "$case_status" -eq 0
  printf '✔ bumping version in package.json from 1.0.0 to %s\n' \
    "$expected_version" > "$case_dir.expected.stdout"
  cmp "$case_dir.expected.stdout" "$case_dir.stdout"
  test ! -s "$case_dir.stderr"
  test -z "$(git status --porcelain)"
}
run_named_prerelease_case patch 'fix: continue patch prerelease' 1.2.3-beta.1
run_named_prerelease_case minor 'feat: promote to minor prerelease' 1.3.0-beta.0
run_named_prerelease_case major 'feat!: promote to major prerelease' 2.0.0-beta.0
run_named_prerelease_case suffix 'fix: continue suffix prerelease' \
  1.2.3-beta.1.foo 1.2.3-beta.0.foo
run_named_prerelease_case zero 'fix: start a patch prerelease from zero' \
  0.0.1-beta.0 0.0.0-beta.0

run_release_as_prerelease_case() {
  case_name=$1
  release_type=$2
  prerelease_id=$3
  case_dir="$tmp/release-as-prerelease-$case_name"
  mkdir "$case_dir"
  cd "$case_dir"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  printf '{"name":"release-as-prerelease-%s","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/release-as-prerelease-%s.git"},"commit-and-tag-version":{"packageFiles":[]}}\n' \
    "$case_name" "$case_name" > package.json
  git add package.json
  git commit -qm 'chore: seed releaseAs prerelease progression'
  git tag -a v2.0.0-beta.0 -m 'release 2.0.0-beta.0'
  if [ -n "$prerelease_id" ]; then
    if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
      --prerelease "$prerelease_id" --release-as "$release_type" \
      > "$case_dir.stdout" 2> "$case_dir.stderr"; then
      case_status=0
    else
      case_status=$?
    fi
  else
    if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
      --prerelease --release-as "$release_type" > "$case_dir.stdout" \
      2> "$case_dir.stderr"; then
      case_status=0
    else
      case_status=$?
    fi
  fi
  test "$case_status" -eq 0
  printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 2.0.0-beta.1' \
    > "$case_dir.expected.stdout"
  cmp "$case_dir.expected.stdout" "$case_dir.stdout"
  test ! -s "$case_dir.stderr"
  test -z "$(git status --porcelain)"
}
run_release_as_prerelease_case patch patch beta
run_release_as_prerelease_case minor minor beta
run_release_as_prerelease_case major major beta
run_release_as_prerelease_case empty patch ''

run_release_as_semver_case() {
  case_name=$1
  release_version=$2
  expected_version=$3
  base_tag_version=${4:-2.0.0-beta.0}
  case_dir="$tmp/release-as-semver-prerelease-$case_name"
  mkdir "$case_dir"
  cd "$case_dir"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  printf '{"name":"release-as-semver-%s","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/release-as-semver-%s.git"},"commit-and-tag-version":{"packageFiles":[]}}\n' \
    "$case_name" "$case_name" > package.json
  git add package.json
  git commit -qm 'chore: seed exact releaseAs prerelease case'
  git tag -a "v$base_tag_version" -m "release $base_tag_version"
  if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
    --prerelease beta --release-as "$release_version" > "$case_dir.stdout" \
    2> "$case_dir.stderr"; then
    case_status=0
  else
    case_status=$?
  fi
  test "$case_status" -eq 0
  printf '✔ bumping version in package.json from 1.0.0 to %s\n' \
    "$expected_version" > "$case_dir.expected.stdout"
  cmp "$case_dir.expected.stdout" "$case_dir.stdout"
  test ! -s "$case_dir.stderr"
  test -z "$(git status --porcelain)"
}
run_release_as_semver_case same-stable 2.0.0 2.0.0-beta.1
run_release_as_semver_case higher-stable 2.1.0 2.1.0-beta.0
run_release_as_semver_case lower-stable 1.9.0 1.9.0-beta.0
run_release_as_semver_case same-prerelease 2.0.0-beta.0 2.0.0-beta.1
run_release_as_semver_case build-metadata 2.0.0-beta.1+build.7 \
  2.0.0-beta.1+build.7
run_release_as_semver_case suffix-prerelease 1.2.3-beta.0 \
  1.2.3-beta.1.foo 1.2.3-beta.0.foo
run_release_as_semver_case suffix-stable 1.2.3 1.2.3-beta.1.foo \
  1.2.3-beta.0.foo

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

mkdir "$tmp/pkg-bumpfiles-mixed"
cd "$tmp/pkg-bumpfiles-mixed"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-bumpfiles-mixed-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "bumpFiles": ["VERSION.txt", {"filename": "metadata.json", "type": "json"}]
  }
}
JSON
printf '1.0.0\n' > VERSION.txt
printf '{"version":"1.0.0"}\n' > metadata.json
git add package.json VERSION.txt metadata.json
git commit -qm 'chore: seed mixed package bumpFiles fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: test mixed package bumpFiles'
"$bin" --skip.changelog --skip.commit --skip.tag > /dev/null
test "$(cat VERSION.txt)" = '1.1.0' || {
  printf '%s\n' 'mixed package bumpFiles should update its inferred string target' >&2
  exit 1
}
grep -Fq '"version": "1.0.0"' package.json || {
  printf '%s\n' 'mixed package bumpFiles should not update package.json unless listed' >&2
  exit 1
}
grep -Fq '"version": "1.1.0"' metadata.json || {
  printf '%s\n' 'mixed package bumpFiles should apply the typed JSON updater' >&2
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

mkdir "$tmp/pkg-bumpfiles-typed-unsupported"
cd "$tmp/pkg-bumpfiles-typed-unsupported"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-bumpfiles-typed-unsupported-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-bumpfiles-typed-unsupported.git"},
  "commit-and-tag-version": {
    "packageFiles": ["package.json"],
    "bumpFiles": [
      {"filename": "package.json", "type": "json"},
      {"filename": "metadata.toml", "type": "toml"},
      {"type": "custom", "filename": "custom.dat", "label": "retained"}
    ]
  }
}
JSON
printf 'version = "1.0.0"\n' > metadata.toml
printf 'version=1.0.0\n' > custom.dat
git add package.json metadata.toml custom.dat
git commit -qm 'chore: seed typed unsupported updater fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: trigger typed unsupported updater fixture'
"$bin" --release-as 1.0.1 --skip.changelog --skip.commit --skip.tag \
  > "$tmp/typed-unsupported.stdout" 2> "$tmp/typed-unsupported.stderr"
printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 1.0.1' \
  > "$tmp/typed-unsupported.expected.stdout"
cmp "$tmp/typed-unsupported.expected.stdout" "$tmp/typed-unsupported.stdout"
printf '%s\n' \
  'Unable to obtain updater for: {"filename":"metadata.toml","type":"toml"}' \
  ' - Error: Unable to locate updater for provided type (toml).' \
  ' - Skipping...' \
  'Unable to obtain updater for: {"type":"custom","filename":"custom.dat","label":"retained"}' \
  ' - Error: Unable to locate updater for provided type (custom).' \
  ' - Skipping...' > "$tmp/typed-unsupported.expected.stderr"
cmp "$tmp/typed-unsupported.expected.stderr" "$tmp/typed-unsupported.stderr"
grep -Fq '"version": "1.0.1"' package.json
test "$(cat metadata.toml)" = 'version = "1.0.0"'
test "$(cat custom.dat)" = 'version=1.0.0'
test ! -e CHANGELOG.md

for package_source_type in toml custom; do
  mkdir "$tmp/pkg-packagefiles-typed-$package_source_type"
  cd "$tmp/pkg-packagefiles-typed-$package_source_type"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  printf '{\n  "name": "pkg-packagefiles-typed-%s-fixture",\n  "version": "1.0.0",\n  "repository": {"type": "git", "url": "https://github.com/example/pkg-packagefiles-typed.git"},\n  "commit-and-tag-version": {"packageFiles": [{"filename": "metadata.toml", "type": "%s"}, "package.json"], "bumpFiles": ["package.json"]}\n}\n' \
    "$package_source_type" "$package_source_type" > package.json
  printf 'version = "3.0.0"\n' > metadata.toml
  git add package.json metadata.toml
  git commit -qm 'chore: seed typed packageFiles updater fixture'
  git tag -a v1.0.0 -m 'release 1.0.0'
  git commit --allow-empty -qm 'fix: trigger typed packageFiles updater fixture'
  "$bin" --release-as 1.0.1 --skip.changelog --skip.commit --skip.tag \
    > "$tmp/packagefiles-typed.stdout" 2> "$tmp/packagefiles-typed.stderr"
  test ! -s "$tmp/packagefiles-typed.stdout"
  printf 'Unable to obtain updater for: {"filename":"metadata.toml","type":"%s"}\n - Error: Unable to locate updater for provided type (%s).\n - Skipping...\n' \
    "$package_source_type" "$package_source_type" \
    > "$tmp/packagefiles-typed.expected.stderr"
  cmp "$tmp/packagefiles-typed.expected.stderr" \
    "$tmp/packagefiles-typed.stderr"
  grep -Fq '"version": "1.0.0"' package.json
  test "$(cat metadata.toml)" = 'version = "3.0.0"'
  test ! -e CHANGELOG.md
  test -z "$(git status --porcelain)"
done

mkdir "$tmp/pkg-packagefiles-typed-order"
cd "$tmp/pkg-packagefiles-typed-order"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-packagefiles-typed-order-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-packagefiles-typed-order.git"},
  "commit-and-tag-version": {
    "packageFiles": [
      {"type": "custom", "filename": "metadata.dat", "label": "retained", "extra": {"10": "ten", "2": "two", "list": [true, null]}},
      "package.json"
    ],
    "bumpFiles": ["package.json"]
  }
}
JSON
printf 'version=3.0.0\n' > metadata.dat
git add package.json metadata.dat
git commit -qm 'chore: seed typed packageFiles ordering fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: trigger typed packageFiles ordering fixture'
"$bin" --skip.changelog --skip.commit --skip.tag \
  > "$tmp/packagefiles-ordering.stdout" 2> "$tmp/packagefiles-ordering.stderr"
test ! -s "$tmp/packagefiles-ordering.stdout"
printf '%s\n' \
  'Unable to obtain updater for: {"type":"custom","filename":"metadata.dat","label":"retained","extra":{"2":"two","10":"ten","list":[true,null]}}' \
  ' - Error: Unable to locate updater for provided type (custom).' \
  ' - Skipping...' > "$tmp/packagefiles-ordering.expected.stderr"
cmp "$tmp/packagefiles-ordering.expected.stderr" \
  "$tmp/packagefiles-ordering.stderr"
grep -Fq '"version": "1.0.0"' package.json
test "$(cat metadata.dat)" = 'version=3.0.0'
test -z "$(git status --porcelain)"

for package_source_case in untyped-string untyped-object inferred-string inferred-object; do
  mkdir "$tmp/pkg-packagefiles-$package_source_case"
  cd "$tmp/pkg-packagefiles-$package_source_case"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  case "$package_source_case" in
    untyped-string)
      package_source_entry='"metadata.toml"'
      package_source_filename=metadata.toml
      package_source_expected_identifier='"metadata.toml"'
      ;;
    untyped-object)
      package_source_entry='{"filename":"metadata.toml","label":"retained"}'
      package_source_filename=metadata.toml
      package_source_expected_identifier='{"filename":"metadata.toml","label":"retained"}'
      ;;
    inferred-string)
      package_source_entry='"openapi.yaml"'
      package_source_filename=openapi.yaml
      ;;
    inferred-object)
      package_source_entry='{"filename":"openapi.yaml"}'
      package_source_filename=openapi.yaml
      ;;
  esac
  printf '{\n  "name": "pkg-packagefiles-%s-fixture",\n  "version": "1.0.0",\n  "repository": {"type": "git", "url": "https://github.com/example/pkg-packagefiles-inferred.git"},\n  "commit-and-tag-version": {"packageFiles": [%s, "package.json"], "bumpFiles": ["package.json"]}\n}\n' \
    "$package_source_case" "$package_source_entry" > package.json
  if [ "$package_source_filename" = metadata.toml ]; then
    printf 'version = "3.0.0"\n' > "$package_source_filename"
  else
    printf 'openapi: 3.0.0\ninfo:\n  title: fixture\n  version: 3.0.0\npaths: {}\n' \
      > "$package_source_filename"
  fi
  git add package.json "$package_source_filename"
  git commit -qm 'chore: seed inferred packageFiles fixture'
  git tag -a v1.0.0 -m 'release 1.0.0'
  git commit --allow-empty -qm 'fix: trigger inferred packageFiles fixture'
  "$bin" --skip.changelog --skip.commit --skip.tag \
    > "$tmp/packagefiles-inferred.stdout" 2> "$tmp/packagefiles-inferred.stderr"
  case "$package_source_case" in
    untyped-string|untyped-object)
      test ! -s "$tmp/packagefiles-inferred.stdout"
      printf 'Unable to obtain updater for: %s\n - Error: Unsupported file (%s) provided for bumping.\n Please specify the updater `type` or use a custom `updater`.\n - Skipping...\n' \
        "$package_source_expected_identifier" "$package_source_filename" \
        > "$tmp/packagefiles-inferred.expected.stderr"
      cmp "$tmp/packagefiles-inferred.expected.stderr" \
        "$tmp/packagefiles-inferred.stderr"
      grep -Fq '"version": "1.0.0"' package.json
      test -z "$(git status --porcelain)"
      ;;
    inferred-string|inferred-object)
      printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 3.0.1' \
        > "$tmp/packagefiles-inferred.expected.stdout"
      cmp "$tmp/packagefiles-inferred.expected.stdout" \
        "$tmp/packagefiles-inferred.stdout"
      test ! -s "$tmp/packagefiles-inferred.stderr"
      grep -Fq '"version": "3.0.1"' package.json
      grep -Fq 'version: 3.0.0' openapi.yaml
      git diff --quiet -- openapi.yaml
      test -z "$(git ls-files --others --exclude-standard)"
      ;;
  esac
done

for updater_order_case in gradle-before-maven csproj-after-maven python-before-yaml; do
  mkdir "$tmp/pkg-packagefiles-$updater_order_case"
  cd "$tmp/pkg-packagefiles-$updater_order_case"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  case "$updater_order_case" in
    gradle-before-maven)
      package_source_filename=build.gradle.pom.xml
      cat > "$package_source_filename" <<'XML'
<project><modelVersion>4.0.0</modelVersion><groupId>x</groupId><artifactId>x</artifactId><version>3.0.0</version></project>
XML
      ;;
    csproj-after-maven)
      package_source_filename=pom.xml.csproj
      cat > "$package_source_filename" <<'XML'
<project><modelVersion>4.0.0</modelVersion><groupId>x</groupId><artifactId>x</artifactId><version>3.0.0</version><Version>9.9.9</Version></project>
XML
      ;;
    python-before-yaml)
      package_source_filename=pyproject.toml.yaml
      printf 'version: 3.0.0\n' > "$package_source_filename"
      ;;
  esac
  printf '{\n  "name": "pkg-packagefiles-%s-fixture",\n  "version": "1.0.0",\n  "repository": {"type": "git", "url": "https://github.com/example/pkg-packagefiles-order.git"},\n  "commit-and-tag-version": {"packageFiles": ["%s", "package.json"], "bumpFiles": ["package.json"]}\n}\n' \
    "$updater_order_case" "$package_source_filename" > package.json
  git add package.json "$package_source_filename"
  git commit -qm 'chore: seed updater inference order fixture'
  git tag -a v1.0.0 -m 'release 1.0.0'
  git commit --allow-empty -qm 'fix: trigger updater inference order fixture'
  "$bin" --skip.changelog --skip.commit --skip.tag \
    > "$tmp/packagefiles-order.stdout" 2> "$tmp/packagefiles-order.stderr"
  printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 3.0.1' \
    > "$tmp/packagefiles-order.expected.stdout"
  cmp "$tmp/packagefiles-order.expected.stdout" \
    "$tmp/packagefiles-order.stdout"
  test ! -s "$tmp/packagefiles-order.stderr"
  grep -Fq '"version": "3.0.1"' package.json
  git diff --quiet -- "$package_source_filename"
  test -z "$(git ls-files --others --exclude-standard)"
done

for regex_case in pom-dot-wildcard gradle-dot-wildcard openapi-dot-wildcard python-dot-wildcard; do
  mkdir "$tmp/pkg-packagefiles-$regex_case"
  cd "$tmp/pkg-packagefiles-$regex_case"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  case "$regex_case" in
    pom-dot-wildcard)
      package_source_filename=pom-xml
      cat > "$package_source_filename" <<'XML'
<project><modelVersion>4.0.0</modelVersion><groupId>x</groupId><artifactId>x</artifactId><version>3.0.0</version></project>
XML
      ;;
    gradle-dot-wildcard)
      package_source_filename=build-gradle
      printf 'plugins {}\nversion = "3.0.0"\n' > "$package_source_filename"
      ;;
    openapi-dot-wildcard)
      package_source_filename=openapi-yaml
      printf 'openapi: 3.0.0\ninfo:\n  title: fixture\n  version: 3.0.0\npaths: {}\n' \
        > "$package_source_filename"
      ;;
    python-dot-wildcard)
      package_source_filename=pyproject-toml
      printf '[project]\nname = "fixture"\nversion = "3.0.0"\n' \
        > "$package_source_filename"
      ;;
  esac
  printf '{\n  "name": "pkg-packagefiles-%s-fixture",\n  "version": "1.0.0",\n  "repository": {"type": "git", "url": "https://github.com/example/pkg-packagefiles-dot-wildcard.git"},\n  "commit-and-tag-version": {"packageFiles": ["%s", "package.json"], "bumpFiles": ["package.json"]}\n}\n' \
    "$regex_case" "$package_source_filename" > package.json
  git add package.json "$package_source_filename"
  git commit -qm 'chore: seed updater regex inference fixture'
  git tag -a v1.0.0 -m 'release 1.0.0'
  git commit --allow-empty -qm 'fix: trigger updater regex inference fixture'
  "$bin" --skip.changelog --skip.commit --skip.tag \
    > "$tmp/packagefiles-regex.stdout" 2> "$tmp/packagefiles-regex.stderr"
  printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 3.0.1' \
    > "$tmp/packagefiles-regex.expected.stdout"
  cmp "$tmp/packagefiles-regex.expected.stdout" \
    "$tmp/packagefiles-regex.stdout"
  test ! -s "$tmp/packagefiles-regex.stderr"
  grep -Fq '"version": "3.0.1"' package.json
  git diff --quiet -- "$package_source_filename"
  test -z "$(git ls-files --others --exclude-standard)"
done

mkdir "$tmp/pkg-bumpfiles-inferred-object"
cd "$tmp/pkg-bumpfiles-inferred-object"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-bumpfiles-inferred-object-fixture",
  "version": "1.0.0",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-bumpfiles-inferred-object.git"},
  "commit-and-tag-version": {
    "bumpFiles": [{"filename": "VERSION.txt"}]
  }
}
JSON
printf '1.0.0\n' > VERSION.txt
git add package.json VERSION.txt
git commit -qm 'chore: seed inferred bumpFiles fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: trigger inferred bumpFiles fixture'
"$bin" --skip.changelog --skip.commit --skip.tag \
  > "$tmp/bumpfiles-inferred.stdout" 2> "$tmp/bumpfiles-inferred.stderr"
printf '%s\n' '✔ bumping version in VERSION.txt from 1.0.0' ' to 1.0.1' \
  > "$tmp/bumpfiles-inferred.expected.stdout"
cmp "$tmp/bumpfiles-inferred.expected.stdout" \
  "$tmp/bumpfiles-inferred.stdout"
test ! -s "$tmp/bumpfiles-inferred.stderr"
grep -Fq '"version": "1.0.0"' package.json
test "$(cat VERSION.txt)" = '1.0.1'

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

mkdir "$tmp/pkg-package-files-mixed"
cd "$tmp/pkg-package-files-mixed"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "pkg-package-files-mixed-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "packageFiles": ["VERSION.txt", {"filename": "metadata.json", "type": "json"}]
  }
}
JSON
printf '2.0.0\n' > VERSION.txt
printf '{"version":"2.0.0"}\n' > metadata.json
git add package.json VERSION.txt metadata.json
git commit -qm 'chore: seed mixed packageFiles fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: test mixed packageFiles'
mixed_package_files_preview=$("$bin" --dry-run 2>&1)
printf '%s\n' "$mixed_package_files_preview" | \
  grep -Fq 'bumping version in package.json from 1.0.0 to 2.1.0' || {
    printf 'mixed string/object packageFiles should retain their configured source version:\n%s\n' \
      "$mixed_package_files_preview" >&2
    exit 1
  }

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

mkdir "$tmp/pkg-prebump"
cd "$tmp/pkg-prebump"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
cat > package.json <<'JSON'
{
  "name": "pkg-prebump-fixture",
  "version": "1.2.3",
  "repository": {"type": "git", "url": "https://github.com/example/pkg-prebump.git"},
  "commit-and-tag-version": {"scripts": {"prebump": "printf 1.3.0"}}
}
JSON
git add package.json
git commit -qm 'chore: seed package prebump fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
git commit --allow-empty -qm 'fix: exercise package prebump override'
"$bin" > "$tmp/pkg-prebump.stdout" 2> "$tmp/pkg-prebump.stderr"
printf '✔ Running lifecycle script "prebump"\nℹ - execute command: "printf 1.3.0"\n✔ bumping version in package.json from 1.2.3 to 1.3.0\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ tagging release v1.3.0\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n' \
  > "$tmp/pkg-prebump.expected.stdout"
cmp "$tmp/pkg-prebump.expected.stdout" "$tmp/pkg-prebump.stdout"
test ! -s "$tmp/pkg-prebump.stderr"
grep -q '"version": "1.3.0"' package.json
test "$(git tag --list v1.3.0)" = v1.3.0
test "$(git log -1 --format=%s)" = 'chore(release): 1.3.0'
test -z "$(git status --porcelain)"

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

mkdir "$tmp/lifecycle-script-failure"
cd "$tmp/lifecycle-script-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "lifecycle-script-failure-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {"prerelease": "printf hook-stdout; printf hook-failure >&2; exit 7"}
  }
}
JSON
git add package.json
git commit -qm 'chore: seed lifecycle script failure fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise lifecycle script failure'
set +e
"$bin" --skip.changelog --skip.commit --skip.tag > "$tmp/lifecycle-failure.stdout" 2> "$tmp/lifecycle-failure.stderr"
lifecycle_failure_status=$?
set -e
test "$lifecycle_failure_status" -eq 1
printf '%s\n' \
  '✔ Running lifecycle script "prerelease"' \
  'ℹ - execute command: "printf hook-stdout; printf hook-failure >&2; exit 7"' \
  > "$tmp/lifecycle-failure.stdout.expected"
cmp "$tmp/lifecycle-failure.stdout.expected" "$tmp/lifecycle-failure.stdout"
printf '%s\n' \
  'hook-failure' \
  'Command failed: printf hook-stdout; printf hook-failure >&2; exit 7' \
  'hook-failure' > "$tmp/lifecycle-failure.stderr.expected"
cmp "$tmp/lifecycle-failure.stderr.expected" "$tmp/lifecycle-failure.stderr"
cat > package.json <<'JSON'
{
  "name": "lifecycle-script-failure-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {"prerelease": "printf hook-stdout; exit 7"}
  }
}
JSON
git add package.json
git commit -qm 'chore: use stdout-only failing hook'
set +e
"$bin" --skip.changelog --skip.commit --skip.tag > "$tmp/lifecycle-failure-stdout-only.stdout" 2> "$tmp/lifecycle-failure-stdout-only.stderr"
lifecycle_failure_stdout_only_status=$?
set -e
test "$lifecycle_failure_stdout_only_status" -eq 1
printf '%s\n' \
  '✔ Running lifecycle script "prerelease"' \
  'ℹ - execute command: "printf hook-stdout; exit 7"' \
  > "$tmp/lifecycle-failure-stdout-only.stdout.expected"
cmp "$tmp/lifecycle-failure-stdout-only.stdout.expected" "$tmp/lifecycle-failure-stdout-only.stdout"
printf '%s\n' \
  'Command failed: printf hook-stdout; exit 7' \
  '' \
  'Command failed: printf hook-stdout; exit 7' \
  '' > "$tmp/lifecycle-failure-stdout-only.stderr.expected"
cmp "$tmp/lifecycle-failure-stdout-only.stderr.expected" "$tmp/lifecycle-failure-stdout-only.stderr"
set +e
"$bin" --silent --skip.changelog --skip.commit --skip.tag > "$tmp/lifecycle-failure-silent.stdout" 2> "$tmp/lifecycle-failure-silent.stderr"
lifecycle_failure_silent_status=$?
set -e
test "$lifecycle_failure_silent_status" -eq 1
test ! -s "$tmp/lifecycle-failure-silent.stdout"
test ! -s "$tmp/lifecycle-failure-silent.stderr"
test -z "$(git status --porcelain)"
test "$(git tag --list 'v1.1.0')" = ''

mkdir "$tmp/lifecycle-stream-fds"
cd "$tmp/lifecycle-stream-fds"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "lifecycle-stream-fds-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {
      "prerelease": "if test -S /proc/self/fd/1 && test -S /proc/self/fd/2; then yes x | head -c 262144; yes y | head -c 262144 >&2; else printf non-socket >&2; fi"
    }
  }
}
JSON
git add package.json
git commit -qm 'chore: seed lifecycle stream descriptor fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: inspect lifecycle output descriptors'
timeout 30 "$bin" --skip.changelog --skip.commit --skip.tag > "$tmp/lifecycle-stream-fds.stdout" 2> "$tmp/lifecycle-stream-fds.stderr"
test "$(wc -c < "$tmp/lifecycle-stream-fds.stderr")" -eq 262145

mkdir "$tmp/lifecycle-max-buffer"
cd "$tmp/lifecycle-max-buffer"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "lifecycle-max-buffer-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {"prerelease": "yes x | head -c 1100000"}
  }
}
JSON
git add package.json
git commit -qm 'chore: seed lifecycle max buffer fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exceed lifecycle output buffer'
set +e
timeout 30 "$bin" --skip.changelog --skip.commit --skip.tag > "$tmp/lifecycle-max-buffer.stdout" 2> "$tmp/lifecycle-max-buffer.stderr"
lifecycle_max_buffer_status=$?
set -e
test "$lifecycle_max_buffer_status" -eq 1
printf '%s\n' \
  '✔ Running lifecycle script "prerelease"' \
  'ℹ - execute command: "yes x | head -c 1100000"' \
  > "$tmp/lifecycle-max-buffer.stdout.expected"
cmp "$tmp/lifecycle-max-buffer.stdout.expected" "$tmp/lifecycle-max-buffer.stdout"
printf '%s\n' \
  'stdout maxBuffer length exceeded' \
  'stdout maxBuffer length exceeded' \
  > "$tmp/lifecycle-max-buffer.stderr.expected"
cmp "$tmp/lifecycle-max-buffer.stderr.expected" "$tmp/lifecycle-max-buffer.stderr"
set +e
timeout 30 "$bin" --silent --skip.changelog --skip.commit --skip.tag > "$tmp/lifecycle-max-buffer-silent.stdout" 2> "$tmp/lifecycle-max-buffer-silent.stderr"
lifecycle_max_buffer_silent_status=$?
set -e
test "$lifecycle_max_buffer_silent_status" -eq 1
test ! -s "$tmp/lifecycle-max-buffer-silent.stdout"
test ! -s "$tmp/lifecycle-max-buffer-silent.stderr"
test -z "$(git status --porcelain)"
test "$(git tag --list 'v1.1.0')" = ''

mkdir "$tmp/lifecycle-max-buffer-stderr"
cd "$tmp/lifecycle-max-buffer-stderr"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "lifecycle-max-buffer-stderr-fixture",
  "version": "1.0.0",
  "commit-and-tag-version": {
    "scripts": {"prerelease": "yes x | head -c 1100000 >&2"}
  }
}
JSON
git add package.json
git commit -qm 'chore: seed lifecycle stderr max buffer fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exceed lifecycle stderr output buffer'
mkfifo "$tmp/lifecycle-max-buffer-stderr.pipe"
cat "$tmp/lifecycle-max-buffer-stderr.pipe" > "$tmp/lifecycle-max-buffer-stderr.actual" &
stderr_reader_pid=$!
set +e
timeout 30 "$bin" --skip.changelog --skip.commit --skip.tag > "$tmp/lifecycle-max-buffer-stderr.stdout" 2> "$tmp/lifecycle-max-buffer-stderr.pipe"
lifecycle_stderr_max_buffer_status=$?
set -e
wait "$stderr_reader_pid"
test "$lifecycle_stderr_max_buffer_status" -eq 1
yes x | head -c 1100000 > "$tmp/lifecycle-max-buffer-stderr.raw"
head -c 65536 "$tmp/lifecycle-max-buffer-stderr.raw" > "$tmp/lifecycle-max-buffer-stderr.expected"
cmp "$tmp/lifecycle-max-buffer-stderr.expected" "$tmp/lifecycle-max-buffer-stderr.actual"
test -z "$(git status --porcelain)"
test "$(git tag --list 'v1.1.0')" = ''

mkdir "$tmp/gradle-updater"
cd "$tmp/gradle-updater"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "gradle-updater-fixture",
  "commit-and-tag-version": {
    "packageFiles": [{"filename": "build.gradle.kts", "type": "gradle"}],
    "bumpFiles": ["build.gradle.kts"]
  }
}
JSON
cat > build.gradle.kts <<'GRADLE'
plugins {
    id("org.springframework.boot") version "2.4.6"
    kotlin("jvm") version "1.4.31"
    kotlin("plugin.spring") version "1.4.31"
}

version = "6.3.1"
java.sourceCompatibility = JavaVersion.VERSION_1_8

repositories {
    mavenLocal()
    mavenCentral()
}
GRADLE
git add package.json build.gradle.kts
git commit -qm 'chore: seed Gradle updater fixture'
git tag -a v6.3.1 -m 'release 6.3.1'
git commit --allow-empty -qm 'feat: add Gradle feature'
gradle_output=$("$bin" --skip.changelog --skip.commit --skip.tag)
printf '%s\n' "$gradle_output" | grep -Fq '✔ bumping version in build.gradle.kts from 6.3.1 to 6.4.0'
cat > "$tmp/gradle-updater.expected" <<'GRADLE'
plugins {
    id("org.springframework.boot") version "2.4.6"
    kotlin("jvm") version "1.4.31"
    kotlin("plugin.spring") version "1.4.31"
}

version = "6.4.0"
java.sourceCompatibility = JavaVersion.VERSION_1_8

repositories {
    mavenLocal()
    mavenCentral()
}
GRADLE
cmp "$tmp/gradle-updater.expected" build.gradle.kts
test "$(git tag --list 'v6.4.0')" = ''

mkdir "$tmp/csproj-updater"
cd "$tmp/csproj-updater"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "csproj-updater-fixture",
  "commit-and-tag-version": {
    "packageFiles": [{"filename": "Project.csproj", "type": "csproj"}],
    "bumpFiles": ["Project.csproj"]
  }
}
JSON
cat > Project.csproj <<'CSPROJ'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net7.0</TargetFramework>
    <Version>6.3.1</Version>
  </PropertyGroup>
</Project>
CSPROJ
git add package.json Project.csproj
git commit -qm 'chore: seed C# project updater fixture'
git tag -a v6.3.1 -m 'release 6.3.1'
git commit --allow-empty -qm 'feat: add C# project feature'
csproj_output=$("$bin" --skip.changelog --skip.commit --skip.tag)
printf '%s\n' "$csproj_output" | grep -Fq '✔ bumping version in Project.csproj from 6.3.1 to 6.4.0'
cat > "$tmp/csproj-updater.expected" <<'CSPROJ'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net7.0</TargetFramework>
    <Version>6.4.0</Version>
  </PropertyGroup>
</Project>
CSPROJ
cmp "$tmp/csproj-updater.expected" Project.csproj
test "$(git tag --list 'v6.4.0')" = ''

mkdir "$tmp/maven-updater"
cd "$tmp/maven-updater"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "maven-updater-fixture",
  "commit-and-tag-version": {
    "packageFiles": [{"filename": "pom.xml", "type": "maven"}],
    "bumpFiles": ["pom.xml"]
  }
}
JSON
cat > pom.xml <<'POM'
<project>
  <version>6.3.1</version>
</project>
POM
git add package.json pom.xml
git commit -qm 'chore: seed Maven updater fixture'
git tag -a v6.3.1 -m 'release 6.3.1'
git commit --allow-empty -qm 'feat: add Maven feature'
maven_output=$("$bin" --skip.changelog --skip.commit --skip.tag)
printf '%s\n' "$maven_output" | grep -Fq '✔ bumping version in pom.xml from 6.3.1 to 6.4.0'
cat > "$tmp/maven-updater.expected" <<'POM'
<project>
  <version>6.4.0</version>
</project>

POM
cmp "$tmp/maven-updater.expected" pom.xml
test "$(git tag --list 'v6.4.0')" = ''

mkdir "$tmp/python-updater"
cd "$tmp/python-updater"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "python-updater-fixture",
  "commit-and-tag-version": {
    "packageFiles": [{"filename": "pyproject.toml", "type": "python"}],
    "bumpFiles": [{"filename": "pyproject.toml", "type": "python"}]
  }
}
JSON
cat > pyproject.toml <<'PY'
# version = '6.3.1'
[tool.poetry]
version = "6.3.1"
PY
git add package.json pyproject.toml
git commit -qm 'chore: seed Python updater fixture'
git tag -a v6.3.1 -m 'release 6.3.1'
git commit --allow-empty -qm 'feat: add Python feature'
python_output=$("$bin" --skip.changelog --skip.commit --skip.tag)
printf '%s\n' "$python_output" | grep -Fq '✔ bumping version in pyproject.toml from 6.3.1 to 6.4.0'
cat > "$tmp/python-updater.expected" <<'PY'
# version = '6.4.0'
[tool.poetry]
version = "6.3.1"
PY
cmp "$tmp/python-updater.expected" pyproject.toml
test "$(git tag --list 'v6.4.0')" = ''

mkdir "$tmp/csproj-invalid-version"
cd "$tmp/csproj-invalid-version"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
cat > package.json <<'JSON'
{
  "name": "csproj-invalid-version-fixture",
  "commit-and-tag-version": {
    "packageFiles": [{"filename": "Project.csproj", "type": "csproj"}],
    "bumpFiles": [{"filename": "Project.csproj", "type": "csproj"}]
  }
}
JSON
printf '%s\n' '<Project><Version>not a version</Version></Project>' > Project.csproj
git add package.json Project.csproj
git commit -qm 'chore: seed invalid C# project version'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: test invalid C# project version'
if "$bin" --skip.changelog --skip.commit --skip.tag > "$tmp/csproj-invalid.stdout" 2> "$tmp/csproj-invalid.stderr"; then
  exit 1
else
  csproj_invalid_status=$?
fi
test "$csproj_invalid_status" -eq 1
printf '%s\n' 'Invalid Version: not a version' > "$tmp/csproj-invalid.expected"
cmp "$tmp/csproj-invalid.expected" "$tmp/csproj-invalid.stderr"
test ! -s "$tmp/csproj-invalid.stdout"
if "$bin" --skip.changelog --skip.commit --skip.tag --silent > "$tmp/csproj-invalid-silent.stdout" 2> "$tmp/csproj-invalid-silent.stderr"; then
  exit 1
else
  csproj_invalid_silent_status=$?
fi
test "$csproj_invalid_silent_status" -eq 1
test ! -s "$tmp/csproj-invalid-silent.stdout"
test ! -s "$tmp/csproj-invalid-silent.stderr"
test -z "$(git status --porcelain)"
test "$(git tag --list)" = 'v1.0.0'

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

mkdir "$tmp/malformed-package-fallback"
cd "$tmp/malformed-package-fallback"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"malformed-primary","version":}\n' > package.json
printf '{"name":"valid-fallback","version":"2.0.0"}\n' > bower.json
git add package.json bower.json
git commit -qm 'chore: seed malformed primary package file'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: use fallback package file'
malformed_package_preview=$("$bin" --dry-run 2>&1)
printf '%s\n' "$malformed_package_preview" | \
  grep -Fq 'bumping version in bower.json from 2.0.0 to 2.1.0' || {
    printf 'malformed packageFiles entries should fall through to the next valid file:\n%s\n' \
      "$malformed_package_preview" >&2
    exit 1
  }
printf '%s\n' "$malformed_package_preview" | \
  grep -Fq "Unexpected token '}', ...\"\"version\":}" || {
    printf 'malformed package diagnostic did not match upstream:\n%s\n' \
      "$malformed_package_preview" >&2
    exit 1
  }
if printf '%s\n' "$malformed_package_preview" | \
  grep -Fq 'JSON version file has no root version string'; then
  printf 'failed packageFiles should be skipped silently before bump processing:\n%s\n' \
    "$malformed_package_preview" >&2
  exit 1
fi

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

mkdir "$tmp/brace-ignore-bump-files"
cd "$tmp/brace-ignore-bump-files"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf '*.{json,yml}\n' > .gitignore
printf '{"name":"brace-ignore-bump-files","version":"1.0.0"}\n' > package.json
printf '{"name":"brace-ignore-bump-files","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"brace-ignore-bump-files","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json package-lock.json
git commit -qm 'chore: initialize brace ignore fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise brace ignore matching'
cp package.json "$tmp/brace-ignore-package.expected"
cp package-lock.json "$tmp/brace-ignore-lock.expected"
brace_ignore_head=$(git rev-parse HEAD)
brace_ignore_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_brace_ignore_output=$(printf '%s\n' \
  "Not updating file 'package.json', as it is ignored in Git" \
  "Not updating file 'bower.json', as it is ignored in Git" \
  "Not updating file 'manifest.json', as it is ignored in Git" \
  "Not updating file 'package-lock.json', as it is ignored in Git" \
  "Not updating file 'npm-shrinkwrap.json', as it is ignored in Git")
test "$brace_ignore_output" = "$expected_brace_ignore_output"
cmp "$tmp/brace-ignore-package.expected" package.json
cmp "$tmp/brace-ignore-lock.expected" package-lock.json
test "$(git rev-parse HEAD)" = "$brace_ignore_head"
test "$(git tag --list)" = v1.0.0
test -z "$(git status --porcelain)"
test ! -e CHANGELOG.md

mkdir "$tmp/brace-ignore-order"
cd "$tmp/brace-ignore-order"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf '{package,manifest,bower}.json\n' > .gitignore
printf '{"name":"brace-ignore-order","version":"1.0.0"}\n' > package.json
printf '{"name":"brace-ignore-order","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"brace-ignore-order","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json package-lock.json
git commit -qm 'chore: initialize brace ignore order fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise bump output ordering'
cp package.json "$tmp/brace-order-package.expected"
brace_order_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_brace_order_output=$(printf '%s\n' \
  "Not updating file 'package.json', as it is ignored in Git" \
  "Not updating file 'bower.json', as it is ignored in Git" \
  "Not updating file 'manifest.json', as it is ignored in Git" \
  '✔ bumping version in package-lock.json from 1.0.0 to 1.0.1')
test "$brace_order_output" = "$expected_brace_order_output"
cmp "$tmp/brace-order-package.expected" package.json
grep -q '"version": "1.0.1"' package-lock.json
test "$(git status --porcelain)" = ' M package-lock.json'

mkdir "$tmp/brace-ignore-range"
cd "$tmp/brace-ignore-range"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf 'packag{e..e}.json\n' > .gitignore
printf '{"name":"brace-ignore-range","version":"1.0.0"}\n' > package.json
printf '{"name":"brace-ignore-range","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"brace-ignore-range","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json package-lock.json
git commit -qm 'chore: initialize brace range fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise brace range matching'
cp package.json "$tmp/brace-range-package.expected"
brace_range_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_brace_range_output=$(printf '%s\n' \
  "Not updating file 'package.json', as it is ignored in Git" \
  '✔ bumping version in package-lock.json from 1.0.0 to 1.0.1')
test "$brace_range_output" = "$expected_brace_range_output"
cmp "$tmp/brace-range-package.expected" package.json
grep -q '"version": "1.0.1"' package-lock.json
test "$(git status --porcelain)" = ' M package-lock.json'

mkdir "$tmp/brace-ignore-numeric-range"
cd "$tmp/brace-ignore-numeric-range"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf 'package{1..5..2}.json\n' > .gitignore
printf '%s\n' '{"name":"brace-ignore-numeric-range","version":"1.0.0","commit-and-tag-version":{"bumpFiles":[{"filename":"package3.json","type":"json"}]}}' > package.json
printf '{"version":"1.0.0"}\n' > package3.json
git add -f .gitignore package.json package3.json
git commit -qm 'chore: initialize numeric brace range fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise numeric brace range matching'
cp package.json "$tmp/brace-numeric-package.expected"
cp package3.json "$tmp/brace-numeric-target.expected"
numeric_brace_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
test "$numeric_brace_output" = "Not updating file 'package3.json', as it is ignored in Git"
cmp "$tmp/brace-numeric-package.expected" package.json
cmp "$tmp/brace-numeric-target.expected" package3.json
test -z "$(git status --porcelain)"

mkdir "$tmp/extglob-ignore-bump-files"
cd "$tmp/extglob-ignore-bump-files"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf '@(package|package-lock).json\n' > .gitignore
printf '{"name":"extglob-ignore-bump-files","version":"1.0.0"}\n' > package.json
printf '{"name":"extglob-ignore-bump-files","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"extglob-ignore-bump-files","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json package-lock.json
git commit -qm 'chore: initialize extglob ignore fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise extglob ignore matching'
cp package.json "$tmp/extglob-package.expected"
cp package-lock.json "$tmp/extglob-lock.expected"
extglob_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_extglob_output=$(printf '%s\n' \
  "Not updating file 'package.json', as it is ignored in Git" \
  "Not updating file 'package-lock.json', as it is ignored in Git")
test "$extglob_output" = "$expected_extglob_output"
cmp "$tmp/extglob-package.expected" package.json
cmp "$tmp/extglob-lock.expected" package-lock.json
test -z "$(git status --porcelain)"

mkdir "$tmp/gitignore-trim-patterns"
cd "$tmp/gitignore-trim-patterns"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf ' \302\240package.json\302\240 \npackage-lock.json\\ \n' > .gitignore
printf '{"name":"gitignore-trim-patterns","version":"1.0.0"}\n' > package.json
printf '{"name":"gitignore-trim-patterns","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"gitignore-trim-patterns","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json package-lock.json
git commit -qm 'chore: initialize trimmed ignore pattern fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise ignore whitespace trimming'
cp package.json "$tmp/gitignore-trim-package.expected"
gitignore_trim_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_gitignore_trim_output=$(printf '%s\n' \
  "Not updating file 'package.json', as it is ignored in Git" \
  '✔ bumping version in package-lock.json from 1.0.0 to 1.0.1')
test "$gitignore_trim_output" = "$expected_gitignore_trim_output"
cmp "$tmp/gitignore-trim-package.expected" package.json
grep -q '"version": "1.0.1"' package-lock.json
test "$(git status --porcelain)" = ' M package-lock.json'

mkdir "$tmp/gitignore-trim-negation"
cd "$tmp/gitignore-trim-negation"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf ' !package.json\n' > .gitignore
printf '{"name":"gitignore-trim-negation","version":"1.0.0"}\n' > package.json
printf '{"name":"gitignore-trim-negation","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"gitignore-trim-negation","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json package-lock.json
git commit -qm 'chore: initialize trimmed negation fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise trimmed negation matching'
cp package-lock.json "$tmp/gitignore-negation-lock.expected"
gitignore_negation_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_gitignore_negation_output=$(printf '%s\n' \
  '✔ bumping version in package.json from 1.0.0 to 1.1.0' \
  "Not updating file 'bower.json', as it is ignored in Git" \
  "Not updating file 'manifest.json', as it is ignored in Git" \
  "Not updating file 'package-lock.json', as it is ignored in Git" \
  "Not updating file 'npm-shrinkwrap.json', as it is ignored in Git")
test "$gitignore_negation_output" = "$expected_gitignore_negation_output"
grep -q '"version": "1.1.0"' package.json
cmp "$tmp/gitignore-negation-lock.expected" package-lock.json
test "$(git status --porcelain)" = ' M package.json'

mkdir "$tmp/gitignore-trim-negated-comment"
cd "$tmp/gitignore-trim-negated-comment"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf ' !#comment\n' > .gitignore
printf '{"name":"gitignore-trim-negated-comment","version":"1.0.0"}\n' > package.json
printf '{"name":"gitignore-trim-negated-comment","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"gitignore-trim-negated-comment","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json package-lock.json
git commit -qm 'chore: initialize negated comment fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'feat: exercise negated comment matching'
cp package.json "$tmp/gitignore-negated-comment-package.expected"
cp package-lock.json "$tmp/gitignore-negated-comment-lock.expected"
negated_comment_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_negated_comment_output=$(printf '%s\n' \
  "Not updating file 'package.json', as it is ignored in Git" \
  "Not updating file 'bower.json', as it is ignored in Git" \
  "Not updating file 'manifest.json', as it is ignored in Git" \
  "Not updating file 'package-lock.json', as it is ignored in Git" \
  "Not updating file 'npm-shrinkwrap.json', as it is ignored in Git")
test "$negated_comment_output" = "$expected_negated_comment_output"
cmp "$tmp/gitignore-negated-comment-package.expected" package.json
cmp "$tmp/gitignore-negated-comment-lock.expected" package-lock.json
test -z "$(git status --porcelain)"

mkdir "$tmp/ignored-bump-files"
cd "$tmp/ignored-bump-files"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf 'package-lock.json\n' > .gitignore
printf '{"name":"ignored-bump-files","version":"1.0.0"}\n' > package.json
printf '{"name":"ignored-bump-files","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"ignored-bump-files","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json
git commit -qm 'chore: initialize ignored bump file fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise ignored bump file handling'
ignored_bump_head=$(git rev-parse HEAD)
ignored_bump_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_ignored_bump_output=$(printf '%s\n%s' \
  '✔ bumping version in package.json from 1.0.0 to 1.0.1' \
  "Not updating file 'package-lock.json', as it is ignored in Git")
test "$ignored_bump_output" = "$expected_ignored_bump_output"
grep -q '"version": "1.0.1"' package.json
grep -q '"version":"1.0.0"' package-lock.json
test "$(git rev-parse HEAD)" = "$ignored_bump_head"
test "$(git tag --list)" = v1.0.0
test "$(git status --porcelain)" = ' M package.json'
test ! -e CHANGELOG.md

mkdir "$tmp/directory-bump-file"
cd "$tmp/directory-bump-file"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
mkdir package-lock.json
printf '{"name":"directory-bump-file","version":"1.0.0"}\n' > package.json
printf 'untouched\n' > package-lock.json/marker
git add package.json package-lock.json/marker
git commit -qm 'chore: initialize directory bump fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise directory bump handling'
directory_bump_head=$(git rev-parse HEAD)
directory_bump_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_directory_bump_output=$(printf '%s\n%s' \
  '✔ bumping version in package.json from 1.0.0 to 1.0.1' \
  "Not updating 'package-lock.json', as it is not a file")
test "$directory_bump_output" = "$expected_directory_bump_output"
grep -q '"version": "1.0.1"' package.json
test "$(cat package-lock.json/marker)" = untouched
test "$(git rev-parse HEAD)" = "$directory_bump_head"
test "$(git tag --list)" = v1.0.0
test "$(git status --porcelain)" = ' M package.json'
test ! -e CHANGELOG.md

mkdir "$tmp/globstar-root-bump-file"
cd "$tmp/globstar-root-bump-file"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email test@example.invalid
git config commit.gpgSign false
printf '**/package-lock.json\n' > .gitignore
printf '{"name":"globstar-root-bump-file","version":"1.0.0"}\n' > package.json
printf '{"name":"globstar-root-bump-file","version":"1.0.0","lockfileVersion":3,"packages":{"":{"name":"globstar-root-bump-file","version":"1.0.0"}}}\n' > package-lock.json
git add .gitignore package.json
git commit -qm 'chore: initialize globstar root fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise globstar root behavior'
globstar_bump_output=$("$bin" --skip.changelog --skip.commit --skip.tag 2>&1)
expected_globstar_bump_output=$(printf '%s\n%s' \
  '✔ bumping version in package.json from 1.0.0 to 1.0.1' \
  '✔ bumping version in package-lock.json from 1.0.0 to 1.0.1')
test "$globstar_bump_output" = "$expected_globstar_bump_output"
grep -q '"version": "1.0.1"' package.json
grep -q '"version": "1.0.1"' package-lock.json

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

mkdir "$tmp/large-typed-packagefile-diagnostic"
cd "$tmp/large-typed-packagefile-diagnostic"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
long_label=$(printf '%*s' 3000 '' | tr ' ' x)
printf '{\n  "name": "large-typed-packagefile-diagnostic-fixture",\n  "version": "1.0.0",\n  "repository": {"type": "git", "url": "https://github.com/example/large-typed-packagefile-diagnostic.git"},\n  "commit-and-tag-version": {"packageFiles": [{"type": "custom", "filename": "metadata.dat", "label": "%s"}, "package.json"], "bumpFiles": ["package.json"]}\n}\n' \
  "$long_label" > package.json
printf 'version=3.0.0\n' > metadata.dat
git add package.json metadata.dat
git commit -qm 'chore: seed large updater diagnostic fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: trigger large updater diagnostic fixture'
"$bin" --skip.changelog --skip.commit --skip.tag \
  > "$tmp/large-updater-diagnostic.stdout" \
  2> "$tmp/large-updater-diagnostic.stderr"
test ! -s "$tmp/large-updater-diagnostic.stdout"
printf 'Unable to obtain updater for: {"type":"custom","filename":"metadata.dat","label":"%s"}\n - Error: Unable to locate updater for provided type (custom).\n - Skipping...\n' \
  "$long_label" > "$tmp/large-updater-diagnostic.expected.stderr"
cmp "$tmp/large-updater-diagnostic.expected.stderr" \
  "$tmp/large-updater-diagnostic.stderr"
test -z "$(git status --porcelain)"

for custom_updater_location in packageFiles bumpFiles; do
  mkdir "$tmp/custom-updater-$custom_updater_location"
  cd "$tmp/custom-updater-$custom_updater_location"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  if [ "$custom_updater_location" = packageFiles ]; then
    package_files='[{"filename":"metadata.dat","updater":"./custom-updater.js"},"package.json"]'
    bump_files='["package.json"]'
  else
    package_files='["package.json"]'
    bump_files='[{"filename":"metadata.dat","updater":"./custom-updater.js"},"package.json"]'
  fi
  printf '{"name":"custom-updater-%s-fixture","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/custom-updater-fixture.git"},"commit-and-tag-version":{"packageFiles":%s,"bumpFiles":%s}}\n' \
    "$custom_updater_location" "$package_files" "$bump_files" > package.json
  printf 'version=3.0.0\n' > metadata.dat
  git add package.json metadata.dat
  git commit -qm 'chore: seed custom updater fixture'
  git tag -a v1.0.0 -m 'release 1.0.0'
  git commit --allow-empty -qm 'fix: trigger custom updater fixture'
  if "$bin" --skip.changelog --skip.commit --skip.tag \
    > "$tmp/custom-updater.stdout" 2> "$tmp/custom-updater.stderr"; then
    custom_updater_status=0
  else
    custom_updater_status=$?
  fi
  test "$custom_updater_status" -eq 2
  test ! -s "$tmp/custom-updater.stdout"
  printf '%s\n' \
    'csemver: custom JavaScript updaters require Node and are unsupported' \
    > "$tmp/custom-updater.expected.stderr"
  cmp "$tmp/custom-updater.expected.stderr" "$tmp/custom-updater.stderr"
  test -z "$(git status --porcelain)"
done

mkdir "$tmp/plain-text-trailing-whitespace"
cd "$tmp/plain-text-trailing-whitespace"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{"name":"plain-text-trailing-whitespace","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/plain-text-trailing-whitespace.git"},"commit-and-tag-version":{"packageFiles":["VERSION.txt"],"bumpFiles":["VERSION.txt"]}}\n' > package.json
printf '1.0.0 \n' > VERSION.txt
git add package.json VERSION.txt
git commit -qm 'chore: seed plain-text whitespace fixture'
git tag -a v1.0.0 -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise plain-text whitespace'
if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
  --release-as patch > "$tmp/plain-text-whitespace.stdout" \
  2> "$tmp/plain-text-whitespace.stderr"; then
  plain_text_whitespace_status=0
else
  plain_text_whitespace_status=$?
fi
test "$plain_text_whitespace_status" -eq 0
printf '✔ bumping version in VERSION.txt from 1.0.0 \n to 1.0.1\n' \
  > "$tmp/plain-text-whitespace.expected.stdout"
cmp "$tmp/plain-text-whitespace.expected.stdout" \
  "$tmp/plain-text-whitespace.stdout"
test ! -s "$tmp/plain-text-whitespace.stderr"
printf '1.0.0 \n' > "$tmp/plain-text-whitespace.expected.version"
cmp "$tmp/plain-text-whitespace.expected.version" VERSION.txt
test -z "$(git status --porcelain)"

mkdir "$tmp/tag-fallback-prerelease-filter"
cd "$tmp/tag-fallback-prerelease-filter"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{"name":"tag-fallback-prerelease-filter","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/tag-fallback-prerelease-filter.git"},"commit-and-tag-version":{"packageFiles":[]}}\n' > package.json
git add package.json
git commit -qm 'chore: seed prerelease fallback fixture'
git tag -a v1.2.3-beta.0 -m 'release 1.2.3-beta.0'
git commit --allow-empty -qm 'fix: exercise prerelease fallback filtering'
if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag --prerelease rc \
  > "$tmp/tag-fallback-prerelease-filter.stdout" \
  2> "$tmp/tag-fallback-prerelease-filter.stderr"; then
  tag_fallback_status=0
else
  tag_fallback_status=$?
fi
test "$tag_fallback_status" -eq 1
test ! -s "$tmp/tag-fallback-prerelease-filter.stdout"
printf '%s\n' 'Invalid version. Must be a string. Got type "undefined".' \
  > "$tmp/tag-fallback-prerelease-filter.expected.stderr"
cmp "$tmp/tag-fallback-prerelease-filter.expected.stderr" \
  "$tmp/tag-fallback-prerelease-filter.stderr"
test -z "$(git status --porcelain)"

mkdir "$tmp/tag-prefix-regex"
cd "$tmp/tag-prefix-regex"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{"name":"tag-prefix-regex","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/tag-prefix-regex.git"},"commit-and-tag-version":{"packageFiles":[],"tagPrefix":"v+"}}\n' > package.json
git add package.json
git commit -qm 'chore: seed tag prefix fixture'
git tag -a 'v+1.0.0' -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise tag prefix regex'
if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
  > "$tmp/tag-prefix-regex.stdout" 2> "$tmp/tag-prefix-regex.stderr"; then
  tag_prefix_status=0
else
  tag_prefix_status=$?
fi
test "$tag_prefix_status" -eq 1
test ! -s "$tmp/tag-prefix-regex.stdout"
printf '%s\n' 'Invalid version. Must be a string. Got type "object".' \
  > "$tmp/tag-prefix-regex.expected.stderr"
cmp "$tmp/tag-prefix-regex.expected.stderr" "$tmp/tag-prefix-regex.stderr"
if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
  --prerelease rc > "$tmp/tag-prefix-regex-prerelease.stdout" \
  2> "$tmp/tag-prefix-regex-prerelease.stderr"; then
  tag_prefix_prerelease_status=0
else
  tag_prefix_prerelease_status=$?
fi
test "$tag_prefix_prerelease_status" -eq 1
test ! -s "$tmp/tag-prefix-regex-prerelease.stdout"
printf '%s\n' 'Invalid version. Must be a string. Got type "undefined".' \
  > "$tmp/tag-prefix-regex-prerelease.expected.stderr"
cmp "$tmp/tag-prefix-regex-prerelease.expected.stderr" \
  "$tmp/tag-prefix-regex-prerelease.stderr"
test -z "$(git status --porcelain)"

mkdir "$tmp/tag-decoration-parenthesis"
cd "$tmp/tag-decoration-parenthesis"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{"name":"tag-decoration-parenthesis","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/tag-decoration-parenthesis.git"},"commit-and-tag-version":{"packageFiles":[],"tagPrefix":"v(foo)"}}\n' > package.json
git add package.json
git commit -qm 'chore: seed decorated tag fixture'
git tag -a 'v(foo)1.0.0' -m 'release 1.0.0'
git commit --allow-empty -qm 'fix: exercise decorated tag parsing'
if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
  > "$tmp/tag-decoration-parenthesis.stdout" \
  2> "$tmp/tag-decoration-parenthesis.stderr"; then
  tag_decoration_status=0
else
  tag_decoration_status=$?
fi
test "$tag_decoration_status" -eq 0
printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 1.0.1' \
  > "$tmp/tag-decoration-parenthesis.expected.stdout"
cmp "$tmp/tag-decoration-parenthesis.expected.stdout" \
  "$tmp/tag-decoration-parenthesis.stdout"
test ! -s "$tmp/tag-decoration-parenthesis.stderr"
test -z "$(git status --porcelain)"

mkdir "$tmp/tag-fallback-unreachable-tag"
cd "$tmp/tag-fallback-unreachable-tag"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{"name":"tag-fallback-unreachable-tag","version":"1.0.0","repository":{"type":"git","url":"https://github.com/example/tag-fallback-unreachable-tag.git"},"commit-and-tag-version":{"packageFiles":[]}}\n' > package.json
git add package.json
git commit -qm 'chore: seed unreachable tag fixture'
git checkout -qb side
git commit --allow-empty -qm 'feat: add unreachable tagged release'
git tag -a v2.0.0 -m 'release 2.0.0'
git checkout -q master
git commit --allow-empty -qm 'fix: retain default reachable release'
if "$bin" --dry-run --skip.changelog --skip.commit --skip.tag \
  > "$tmp/tag-fallback-unreachable-tag.stdout" \
  2> "$tmp/tag-fallback-unreachable-tag.stderr"; then
  unreachable_tag_status=0
else
  unreachable_tag_status=$?
fi
test "$unreachable_tag_status" -eq 0
printf '%s\n' '✔ bumping version in package.json from 1.0.0 to 1.0.1' \
  > "$tmp/tag-fallback-unreachable-tag.expected.stdout"
cmp "$tmp/tag-fallback-unreachable-tag.expected.stdout" \
  "$tmp/tag-fallback-unreachable-tag.stdout"
test ! -s "$tmp/tag-fallback-unreachable-tag.stderr"
test -z "$(git status --porcelain)"

mkdir "$tmp/path-filter-release"
cd "$tmp/path-filter-release"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"path-filter-release","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/path-filter-release.git"}}\n' > package.json
git add package.json
git commit -qm 'chore: initialize path filter fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
mkdir docs packages
printf 'unrelated\n' > docs/other.txt
git add docs/other.txt
git commit -qm 'feat: unrelated documentation feature'
printf 'target\n' > packages/widget.txt
git add packages/widget.txt
git commit -qm 'fix: update widget'
"$bin" --path packages/widget.txt > "$tmp/path-filter-release.stdout" \
  2> "$tmp/path-filter-release.stderr"
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ tagging release v1.2.4\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n' \
  > "$tmp/path-filter-release.expected.stdout"
cmp "$tmp/path-filter-release.expected.stdout" \
  "$tmp/path-filter-release.stdout"
test ! -s "$tmp/path-filter-release.stderr"
grep -q '"version": "1.2.4"' package.json
grep -q 'update widget' CHANGELOG.md
! grep -q 'unrelated documentation feature' CHANGELOG.md
test "$(git tag --list v1.2.4)" = v1.2.4
test "$(git log -1 --format=%s)" = 'chore(release): 1.2.4'
test -z "$(git status --porcelain)"

run_publish_hint_case() {
  publish_case_name=$1
  publish_manager=$2
  shift 2
  publish_case_dir="$tmp/publish-hint-$publish_case_name"
  mkdir "$publish_case_dir"
  cd "$publish_case_dir"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  printf '{"name":"publish-hint-probe","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/publish-hint-probe.git"}}\n' > package.json
  for publish_lock in "$@"; do
    if [ "$publish_lock" = package-lock.json ]; then
      printf '{"name":"publish-hint-probe","version":"1.2.3","lockfileVersion":3,"packages":{"":{"name":"publish-hint-probe","version":"1.2.3"}}}\n' > "$publish_lock"
    else
      : > "$publish_lock"
    fi
  done
  git add .
  git commit -qm 'chore: initialize publish hint fixture'
  git tag -a v1.2.3 -m 'release 1.2.3'
  git commit --allow-empty -qm 'fix: exercise publish hint selection'
  "$bin" --dry-run --skip.changelog --skip.commit \
    > "$publish_case_dir.stdout" 2> "$publish_case_dir.stderr"
  expected_publish_hint=$(printf \
    'ℹ Run `git push --follow-tags origin master && %s` to publish' \
    "$publish_manager")
  grep -Fxq "$expected_publish_hint" "$publish_case_dir.stdout"
  test ! -s "$publish_case_dir.stderr"
  grep -q '"version":"1.2.3"' package.json
  test -z "$(git tag --list v1.2.4)"
  test -z "$(git status --porcelain)"
}
run_publish_hint_case no-lock 'npm publish'
run_publish_hint_case npm-lock 'npm publish' package-lock.json
run_publish_hint_case pnpm 'pnpm publish' pnpm-lock.yaml
run_publish_hint_case yarn 'yarn publish' yarn.lock
run_publish_hint_case pnpm-over-npm 'pnpm publish' package-lock.json pnpm-lock.yaml
run_publish_hint_case yarn-over-pnpm 'yarn publish' pnpm-lock.yaml yarn.lock

mkdir "$tmp/commit-all-scope"
cd "$tmp/commit-all-scope"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"commit-all-scope","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/commit-all-scope.git"}}\n' > package.json
printf 'committed\n' > tracked.txt
git add package.json tracked.txt
git commit -qm 'chore: initialize commit-all scope fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
git commit --allow-empty -qm 'fix: exercise commit-all scope'
printf 'staged\n' > staged.txt
git add staged.txt
printf 'unstaged modification\n' > tracked.txt
printf 'untracked\n' > untracked.txt
"$bin" --commit-all > "$tmp/commit-all-scope.stdout" \
  2> "$tmp/commit-all-scope.stderr"
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md and all staged files\n✔ tagging release v1.2.4\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n' \
  > "$tmp/commit-all-scope.expected.stdout"
cmp "$tmp/commit-all-scope.expected.stdout" \
  "$tmp/commit-all-scope.stdout"
test ! -s "$tmp/commit-all-scope.stderr"
test "$(git show --pretty=format: --name-only HEAD | LC_ALL=C sort)" = \
  "$(printf 'CHANGELOG.md\npackage.json\nstaged.txt')"
test "$(git show HEAD:tracked.txt)" = committed
printf 'unstaged modification\n' | cmp - tracked.txt
test "$(git show HEAD:staged.txt)" = staged
test -f untracked.txt
! git cat-file -e HEAD:untracked.txt 2>/dev/null
test "$(git tag --list v1.2.4)" = v1.2.4
test "$(git status --porcelain)" = "$(printf ' M tracked.txt\n?? untracked.txt')"

run_custom_infile_case() {
  infile_case=$1
  infile_dir="$tmp/custom-infile-$infile_case"
  mkdir "$infile_dir"
  cd "$infile_dir"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  git config commit.gpgSign false
  printf '{"name":"custom-infile","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/custom-infile.git"}}\n' > package.json
  mkdir docs
  if [ "$infile_case" = existing ]; then
    printf '# Changelog\n\nPreamble retained.\n\n## [1.2.3] - 2026-09-01\n\n### Fixes\n\n* old change\n' > docs/RELEASES.md
  fi
  git add .
  git commit -qm 'chore: initialize custom infile fixture'
  git tag -a v1.2.3 -m 'release 1.2.3'
  git commit --allow-empty -qm 'fix: exercise custom infile'
  "$bin" --infile docs/RELEASES.md > "$infile_dir.stdout" \
    2> "$infile_dir.stderr"
  if [ "$infile_case" = existing ]; then
    printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ outputting changes to docs/RELEASES.md\n✔ committing package.json and docs/RELEASES.md\n✔ tagging release v1.2.4\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n' > "$infile_dir.expected.stdout"
  else
    printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created docs/RELEASES.md\n✔ outputting changes to docs/RELEASES.md\n✔ committing package.json and docs/RELEASES.md\n✔ tagging release v1.2.4\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n' > "$infile_dir.expected.stdout"
  fi
  cmp "$infile_dir.expected.stdout" "$infile_dir.stdout"
  test ! -s "$infile_dir.stderr"
  grep -q '1.2.4' docs/RELEASES.md
  test ! -e CHANGELOG.md
  test "$(git show --pretty=format: --name-only HEAD | LC_ALL=C sort)" = \
    "$(printf 'docs/RELEASES.md\npackage.json')"
  test "$(git tag --list v1.2.4)" = v1.2.4
  test -z "$(git status --porcelain)"
  if [ "$infile_case" = existing ]; then
    grep -q 'old change' docs/RELEASES.md
  fi
}
run_custom_infile_case existing
run_custom_infile_case missing

mkdir "$tmp/commit-all-empty-paths"
cd "$tmp/commit-all-empty-paths"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
printf '{"name":"commit-all-empty-paths","version":"1.2.3"}\n' > package.json
git add package.json
git commit -qm 'chore: initialize empty commit-all fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'staged\n' > staged.txt
git add staged.txt
if git add > "$tmp/commit-all-empty-paths.git-add.stdout" \
  2> "$tmp/commit-all-empty-paths.git-add.stderr"; then
  empty_git_add_status=0
else
  empty_git_add_status=$?
fi
test "$empty_git_add_status" -eq 0
if [ -s "$tmp/commit-all-empty-paths.git-add.stderr" ]; then
  printf '\n' >> "$tmp/commit-all-empty-paths.git-add.stderr"
fi
"$bin" --skip.bump --skip.changelog --skip.tag --commit-all \
  > "$tmp/commit-all-empty-paths.stdout" \
  2> "$tmp/commit-all-empty-paths.stderr"
printf '✔ committing all staged files and %%s\n' \
  > "$tmp/commit-all-empty-paths.expected.stdout"
cmp "$tmp/commit-all-empty-paths.expected.stdout" \
  "$tmp/commit-all-empty-paths.stdout"
cmp "$tmp/commit-all-empty-paths.git-add.stderr" \
  "$tmp/commit-all-empty-paths.stderr"
test "$(git show --pretty=format: --name-only HEAD)" = staged.txt
test "$(git tag --list)" = v1.2.3
test -z "$(git status --porcelain)"

mkdir "$tmp/signoff-release"
cd "$tmp/signoff-release"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"signoff-release","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/signoff-release.git"}}\n' > package.json
git add package.json
git commit -qm 'chore: initialize signoff release fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
git commit --allow-empty -qm 'fix: exercise signoff release'
"$bin" --signoff > "$tmp/signoff-release.stdout" \
  2> "$tmp/signoff-release.stderr"
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ tagging release v1.2.4\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n' \
  > "$tmp/signoff-release.expected.stdout"
cmp "$tmp/signoff-release.expected.stdout" "$tmp/signoff-release.stdout"
test ! -s "$tmp/signoff-release.stderr"
test "$(git show -s --format=%B HEAD)" = \
  "$(printf 'chore(release): 1.2.4\n\nSigned-off-by: C Semver Test <test@example.invalid>')"
test "$(git tag --list v1.2.4)" = v1.2.4
test -z "$(git status --porcelain)"

mkdir "$tmp/release-message-format"
cd "$tmp/release-message-format"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"release-message-format","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/release-message-format.git"}}\n' > package.json
git add package.json
git commit -qm 'chore: initialize release message fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
git commit --allow-empty -qm 'fix: exercise release message format'
"$bin" --releaseCommitMessageFormat \
  'release {{currentTag}} / {{currentTag}}' \
  > "$tmp/release-message-format.stdout" \
  2> "$tmp/release-message-format.stderr"
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ tagging release v1.2.4\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n' \
  > "$tmp/release-message-format.expected.stdout"
cmp "$tmp/release-message-format.expected.stdout" \
  "$tmp/release-message-format.stdout"
test ! -s "$tmp/release-message-format.stderr"
test "$(git show -s --format=%s HEAD)" = 'release 1.2.4 / 1.2.4'
test "$(git tag --list v1.2.4)" = v1.2.4
test -z "$(git status --porcelain)"

setup_safecrlf_release_fixture() {
  safecrlf_dir=$1
  mkdir "$safecrlf_dir"
  cd "$safecrlf_dir"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  git config commit.gpgSign false
  git config core.autocrlf input
  git config core.safecrlf warn
  printf '{\r\n  "name": "safecrlf-release",\r\n  "version": "1.2.3",\r\n  "repository": {"type": "git", "url": "https://github.com/example/safecrlf-release.git"}\r\n}\r\n' > package.json
  git add package.json > /dev/null 2>&1
  git commit -qm 'chore: initialize safecrlf fixture'
  git tag -a v1.2.3 -m 'release 1.2.3'
  git commit --allow-empty -qm 'fix: exercise safecrlf warnings'
}
safecrlf_candidate="$tmp/safecrlf-candidate"
setup_safecrlf_release_fixture "$safecrlf_candidate"
"$bin" > "$safecrlf_candidate.stdout" 2> "$safecrlf_candidate.stderr"
safecrlf_silent="$tmp/safecrlf-silent"
setup_safecrlf_release_fixture "$safecrlf_silent"
"$bin" --silent > "$safecrlf_silent.stdout" 2> "$safecrlf_silent.stderr"
test ! -s "$safecrlf_silent.stdout"
test ! -s "$safecrlf_silent.stderr"
safecrlf_manual="$tmp/safecrlf-manual"
setup_safecrlf_release_fixture "$safecrlf_manual"
printf '{\r\n  "name": "safecrlf-release",\r\n  "version": "1.2.4",\r\n  "repository": {"type": "git", "url": "https://github.com/example/safecrlf-release.git"}\r\n}\r\n' > package.json
printf '# Changelog\n' > CHANGELOG.md
git add -- package.json CHANGELOG.md > "$safecrlf_manual.add.stdout" \
  2> "$safecrlf_manual.add.stderr"
git commit -m 'chore(release): 1.2.4' package.json CHANGELOG.md \
  > "$safecrlf_manual.commit.stdout" 2> "$safecrlf_manual.commit.stderr"
cat "$safecrlf_manual.add.stderr" > "$safecrlf_manual.expected.stderr"
if [ -s "$safecrlf_manual.add.stderr" ]; then
  printf '\n' >> "$safecrlf_manual.expected.stderr"
fi
cat "$safecrlf_manual.commit.stderr" >> "$safecrlf_manual.expected.stderr"
if [ -s "$safecrlf_manual.commit.stderr" ]; then
  printf '\n' >> "$safecrlf_manual.expected.stderr"
fi
cmp "$safecrlf_manual.expected.stderr" "$safecrlf_candidate.stderr"

mkdir "$tmp/precommit-failure"
cd "$tmp/precommit-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"precommit-failure","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/precommit-failure.git"}}\n' > package.json
git add package.json
git commit -qm 'chore: initialize precommit probe'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: reproduce precommit failure'
printf '#!/bin/sh\necho hook-says-no >&2\nexit 1\n' > .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
if "$bin" > "$tmp/precommit-failure.stdout" \
  2> "$tmp/precommit-failure.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing pre-commit hook' >&2
  exit 1
else
  precommit_failure_status=$?
fi
test "$precommit_failure_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n' \
  > "$tmp/precommit-failure.expected.stdout"
printf 'hook-says-no\n\nCommand failed: git commit CHANGELOG.md package.json -m chore(release): 1.2.4\nhook-says-no\n\n' \
  > "$tmp/precommit-failure.expected.stderr"
cmp "$tmp/precommit-failure.expected.stdout" \
  "$tmp/precommit-failure.stdout"
cmp "$tmp/precommit-failure.expected.stderr" \
  "$tmp/precommit-failure.stderr"
test "$(git status --porcelain)" = \
  "$(printf 'A  CHANGELOG.md\nM  package.json')"
test "$(git tag --list)" = v1.2.3
test "$(git log -1 --format=%s)" = 'fix: reproduce precommit failure'
git reset --hard -q HEAD
rm -f CHANGELOG.md
if "$bin" --silent > "$tmp/precommit-failure.silent.stdout" \
  2> "$tmp/precommit-failure.silent.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing pre-commit hook' >&2
  exit 1
else
  precommit_failure_silent_status=$?
fi
test "$precommit_failure_silent_status" -eq 1
test ! -s "$tmp/precommit-failure.silent.stdout"
test ! -s "$tmp/precommit-failure.silent.stderr"
test "$(git status --porcelain)" = \
  "$(printf 'A  CHANGELOG.md\nM  package.json')"
test "$(git tag --list)" = v1.2.3
test "$(git log -1 --format=%s)" = 'fix: reproduce precommit failure'

mkdir "$tmp/occupied-release-tag"
cd "$tmp/occupied-release-tag"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"occupied-release-tag","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/occupied-release-tag.git"}}\n' > package.json
git add package.json
git commit -qm 'chore: initialize occupied tag fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
git tag -a v1.2.4 -m 'occupied release tag'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: attempt occupied release tag'
if "$bin" > "$tmp/occupied-release-tag.stdout" \
  2> "$tmp/occupied-release-tag.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with an occupied release tag' >&2
  exit 1
else
  occupied_tag_status=$?
fi
test "$occupied_tag_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ tagging release v1.2.4\n' \
  > "$tmp/occupied-release-tag.expected.stdout"
printf 'fatal: tag '\''v1.2.4'\'' already exists\n\nCommand failed: git tag -a v1.2.4 -m chore(release): 1.2.4\nfatal: tag '\''v1.2.4'\'' already exists\n\n' \
  > "$tmp/occupied-release-tag.expected.stderr"
printf '# Changelog\n\nAll notable changes to this project will be documented in this file. See [commit-and-tag-version](https://github.com/absolute-version/commit-and-tag-version) for commit guidelines.\n\n\n' \
  > "$tmp/occupied-release-tag.expected.changelog"
cmp "$tmp/occupied-release-tag.expected.stdout" \
  "$tmp/occupied-release-tag.stdout"
cmp "$tmp/occupied-release-tag.expected.stderr" \
  "$tmp/occupied-release-tag.stderr"
cmp "$tmp/occupied-release-tag.expected.changelog" CHANGELOG.md
test "$(git tag --list)" = "$(printf 'v1.2.3\nv1.2.4')"
test "$(git log -1 --format=%s)" = 'chore(release): 1.2.4'
test -z "$(git status --porcelain)"

mkdir "$tmp/pretag-script-failure"
cd "$tmp/pretag-script-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
cat > package.json <<'JSON'
{
  "name": "pretag-script-failure",
  "version": "1.2.3",
  "repository": {"type": "git", "url": "https://github.com/example/pretag-script-failure.git"},
  "commit-and-tag-version": {"scripts": {"pretag": "./fail-pretag.sh"}}
}
JSON
cat > fail-pretag.sh <<'SH'
#!/bin/sh
printf 'stdout-marker\n'
printf 'stderr-marker\n' >&2
exit 7
SH
chmod +x fail-pretag.sh
git add package.json fail-pretag.sh
git commit -qm 'chore: initialize pretag script fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: trigger pretag script failure'
if "$bin" > "$tmp/pretag-script-failure.stdout" \
  2> "$tmp/pretag-script-failure.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing pretag script' >&2
  exit 1
else
  pretag_script_failure_status=$?
fi
test "$pretag_script_failure_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ Running lifecycle script "pretag"\nℹ - execute command: "./fail-pretag.sh"\n' \
  > "$tmp/pretag-script-failure.expected.stdout"
printf 'stderr-marker\n\nCommand failed: ./fail-pretag.sh\nstderr-marker\n\n' \
  > "$tmp/pretag-script-failure.expected.stderr"
cmp "$tmp/pretag-script-failure.expected.stdout" \
  "$tmp/pretag-script-failure.stdout"
cmp "$tmp/pretag-script-failure.expected.stderr" \
  "$tmp/pretag-script-failure.stderr"
grep -q '"version": "1.2.4"' package.json
test "$(git tag --list)" = v1.2.3
test "$(git log -1 --format=%s)" = 'chore(release): 1.2.4'
test -z "$(git status --porcelain)"

mkdir "$tmp/postcommit-script-failure"
cd "$tmp/postcommit-script-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
cat > package.json <<'JSON'
{
  "name": "postcommit-script-failure",
  "version": "1.2.3",
  "repository": {"type": "git", "url": "https://github.com/example/postcommit-script-failure.git"},
  "commit-and-tag-version": {"scripts": {"postcommit": "./fail-postcommit.sh"}}
}
JSON
cat > fail-postcommit.sh <<'SH'
#!/bin/sh
printf 'stdout-marker\n'
printf 'stderr-marker\n' >&2
exit 7
SH
chmod +x fail-postcommit.sh
git add package.json fail-postcommit.sh
git commit -qm 'chore: initialize postcommit script fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: trigger postcommit script failure'
if "$bin" > "$tmp/postcommit-script-failure.stdout" \
  2> "$tmp/postcommit-script-failure.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing postcommit script' >&2
  exit 1
else
  postcommit_script_failure_status=$?
fi
test "$postcommit_script_failure_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ Running lifecycle script "postcommit"\nℹ - execute command: "./fail-postcommit.sh"\n' \
  > "$tmp/postcommit-script-failure.expected.stdout"
printf 'stderr-marker\n\nCommand failed: ./fail-postcommit.sh\nstderr-marker\n\n' \
  > "$tmp/postcommit-script-failure.expected.stderr"
cmp "$tmp/postcommit-script-failure.expected.stdout" \
  "$tmp/postcommit-script-failure.stdout"
cmp "$tmp/postcommit-script-failure.expected.stderr" \
  "$tmp/postcommit-script-failure.stderr"
grep -q '"version": "1.2.4"' package.json
test "$(git tag --list)" = v1.2.3
test "$(git log -1 --format=%s)" = 'chore(release): 1.2.4'
test -z "$(git status --porcelain)"

mkdir "$tmp/posttag-script-failure"
cd "$tmp/posttag-script-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
cat > package.json <<'JSON'
{
  "name": "posttag-script-failure",
  "version": "1.2.3",
  "repository": {"type": "git", "url": "https://github.com/example/posttag-script-failure.git"},
  "commit-and-tag-version": {"scripts": {"posttag": "./fail-posttag.sh"}}
}
JSON
cat > fail-posttag.sh <<'SH'
#!/bin/sh
printf 'stdout-marker\n'
printf 'stderr-marker\n' >&2
exit 7
SH
chmod +x fail-posttag.sh
git add package.json fail-posttag.sh
git commit -qm 'chore: initialize posttag script fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: trigger posttag script failure'
if "$bin" > "$tmp/posttag-script-failure.stdout" \
  2> "$tmp/posttag-script-failure.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing posttag script' >&2
  exit 1
else
  posttag_script_failure_status=$?
fi
test "$posttag_script_failure_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ committing package.json and CHANGELOG.md\n✔ tagging release v1.2.4\nℹ Run `git push --follow-tags origin master && npm publish` to publish\n✔ Running lifecycle script "posttag"\nℹ - execute command: "./fail-posttag.sh"\n' \
  > "$tmp/posttag-script-failure.expected.stdout"
printf 'stderr-marker\n\nCommand failed: ./fail-posttag.sh\nstderr-marker\n\n' \
  > "$tmp/posttag-script-failure.expected.stderr"
cmp "$tmp/posttag-script-failure.expected.stdout" \
  "$tmp/posttag-script-failure.stdout"
cmp "$tmp/posttag-script-failure.expected.stderr" \
  "$tmp/posttag-script-failure.stderr"
grep -q '"version": "1.2.4"' package.json
test "$(git tag --list)" = "$(printf 'v1.2.3\nv1.2.4')"
test "$(git log -1 --format=%s)" = 'chore(release): 1.2.4'
test -z "$(git status --porcelain)"

mkdir "$tmp/postbump-script-failure"
cd "$tmp/postbump-script-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
cat > package.json <<'JSON'
{
  "name": "postbump-script-failure",
  "version": "1.2.3",
  "repository": {"type": "git", "url": "https://github.com/example/postbump-script-failure.git"},
  "commit-and-tag-version": {"scripts": {"postbump": "./fail-postbump.sh"}}
}
JSON
cat > fail-postbump.sh <<'SH'
#!/bin/sh
printf 'stdout-marker\n'
printf 'stderr-marker\n' >&2
exit 7
SH
chmod +x fail-postbump.sh
git add package.json fail-postbump.sh
git commit -qm 'chore: initialize postbump script fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: trigger postbump script failure'
if "$bin" > "$tmp/postbump-script-failure.stdout" \
  2> "$tmp/postbump-script-failure.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing postbump script' >&2
  exit 1
else
  postbump_script_failure_status=$?
fi
test "$postbump_script_failure_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ Running lifecycle script "postbump"\nℹ - execute command: "./fail-postbump.sh"\n' \
  > "$tmp/postbump-script-failure.expected.stdout"
printf 'stderr-marker\n\nCommand failed: ./fail-postbump.sh\nstderr-marker\n\n' \
  > "$tmp/postbump-script-failure.expected.stderr"
cmp "$tmp/postbump-script-failure.expected.stdout" \
  "$tmp/postbump-script-failure.stdout"
cmp "$tmp/postbump-script-failure.expected.stderr" \
  "$tmp/postbump-script-failure.stderr"
grep -q '"version": "1.2.4"' package.json
test ! -e CHANGELOG.md
test "$(git tag --list)" = v1.2.3
test "$(git log -1 --format=%s)" = 'fix: trigger postbump script failure'
test "$(git status --porcelain)" = ' M package.json'

mkdir "$tmp/postchangelog-script-failure"
cd "$tmp/postchangelog-script-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
cat > package.json <<'JSON'
{
  "name": "postchangelog-script-failure",
  "version": "1.2.3",
  "repository": {"type": "git", "url": "https://github.com/example/postchangelog-script-failure.git"},
  "commit-and-tag-version": {"scripts": {"postchangelog": "./fail-postchangelog.sh"}}
}
JSON
cat > fail-postchangelog.sh <<'SH'
#!/bin/sh
printf 'stdout-marker\n'
printf 'stderr-marker\n' >&2
exit 7
SH
chmod +x fail-postchangelog.sh
git add package.json fail-postchangelog.sh
git commit -qm 'chore: initialize postchangelog script fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: trigger postchangelog script failure'
if "$bin" > "$tmp/postchangelog-script-failure.stdout" \
  2> "$tmp/postchangelog-script-failure.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing postchangelog script' >&2
  exit 1
else
  postchangelog_script_failure_status=$?
fi
test "$postchangelog_script_failure_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ created CHANGELOG.md\n✔ outputting changes to CHANGELOG.md\n✔ Running lifecycle script "postchangelog"\nℹ - execute command: "./fail-postchangelog.sh"\n' \
  > "$tmp/postchangelog-script-failure.expected.stdout"
printf 'stderr-marker\n\nCommand failed: ./fail-postchangelog.sh\nstderr-marker\n\n' \
  > "$tmp/postchangelog-script-failure.expected.stderr"
cmp "$tmp/postchangelog-script-failure.expected.stdout" \
  "$tmp/postchangelog-script-failure.stdout"
cmp "$tmp/postchangelog-script-failure.expected.stderr" \
  "$tmp/postchangelog-script-failure.stderr"
grep -q '"version": "1.2.4"' package.json
test -e CHANGELOG.md
test "$(git tag --list)" = v1.2.3
test "$(git log -1 --format=%s)" = 'fix: trigger postchangelog script failure'
test "$(git status --porcelain)" = \
  "$(printf ' M package.json\n?? CHANGELOG.md')"

mkdir "$tmp/prechangelog-script-failure"
cd "$tmp/prechangelog-script-failure"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
cat > package.json <<'JSON'
{
  "name": "prechangelog-script-failure",
  "version": "1.2.3",
  "repository": {"type": "git", "url": "https://github.com/example/prechangelog-script-failure.git"},
  "commit-and-tag-version": {"scripts": {"prechangelog": "./fail-prechangelog.sh"}}
}
JSON
cat > fail-prechangelog.sh <<'SH'
#!/bin/sh
printf 'stdout-marker\n'
printf 'stderr-marker\n' >&2
exit 7
SH
chmod +x fail-prechangelog.sh
git add package.json fail-prechangelog.sh
git commit -qm 'chore: initialize prechangelog script fixture'
git tag -a v1.2.3 -m 'release 1.2.3'
printf 'fix\n' > fix.txt
git add fix.txt
git commit -qm 'fix: trigger prechangelog script failure'
if "$bin" > "$tmp/prechangelog-script-failure.stdout" \
  2> "$tmp/prechangelog-script-failure.stderr"; then
  printf '%s\n' 'release unexpectedly succeeded with a failing prechangelog script' >&2
  exit 1
else
  prechangelog_script_failure_status=$?
fi
test "$prechangelog_script_failure_status" -eq 1
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n✔ Running lifecycle script "prechangelog"\nℹ - execute command: "./fail-prechangelog.sh"\n' \
  > "$tmp/prechangelog-script-failure.expected.stdout"
printf 'stderr-marker\n\nCommand failed: ./fail-prechangelog.sh\nstderr-marker\n\n' \
  > "$tmp/prechangelog-script-failure.expected.stderr"
cmp "$tmp/prechangelog-script-failure.expected.stdout" \
  "$tmp/prechangelog-script-failure.stdout"
cmp "$tmp/prechangelog-script-failure.expected.stderr" \
  "$tmp/prechangelog-script-failure.stderr"
grep -q '"version": "1.2.4"' package.json
test ! -e CHANGELOG.md
test "$(git tag --list)" = v1.2.3
test "$(git log -1 --format=%s)" = 'fix: trigger prechangelog script failure'
test "$(git status --porcelain)" = ' M package.json'

run_pre_bump_lifecycle_failure() (
  hook=$1
  script="fail-$hook.sh"
  case_dir="$tmp/$hook-script-failure"
  mkdir "$case_dir"
  cd "$case_dir"
  git init -q -b master
  git config user.name 'C Semver Test'
  git config user.email 'test@example.invalid'
  git config commit.gpgSign false
  printf '{\n  "name": "%s-script-failure",\n  "version": "1.2.3",\n  "repository": {"type": "git", "url": "https://github.com/example/%s-script-failure.git"},\n  "commit-and-tag-version": {"scripts": {"%s": "./%s"}}\n}\n' \
    "$hook" "$hook" "$hook" "$script" > package.json
  cp package.json "$tmp/$hook-package.expected.json"
  cat > "$script" <<'SH'
#!/bin/sh
printf 'stdout-marker\n'
printf 'stderr-marker\n' >&2
exit 7
SH
  chmod +x "$script"
  git add package.json "$script"
  git commit -qm "chore: initialize $hook failure fixture"
  git tag -a v1.2.3 -m 'release 1.2.3'
  printf 'fix\n' > fix.txt
  git add fix.txt
  git commit -qm "fix: trigger $hook failure"
  if "$bin" > "$case_dir.stdout" 2> "$case_dir.stderr"; then
    printf 'release unexpectedly succeeded with a failing %s script\n' "$hook" >&2
    exit 1
  else
    hook_status=$?
  fi
  test "$hook_status" -eq 1
  printf '✔ Running lifecycle script "%s"\nℹ - execute command: "./%s"\n' \
    "$hook" "$script" > "$case_dir.expected.stdout"
  printf 'stderr-marker\n\nCommand failed: ./%s\nstderr-marker\n\n' \
    "$script" > "$case_dir.expected.stderr"
  cmp "$case_dir.expected.stdout" "$case_dir.stdout"
  cmp "$case_dir.expected.stderr" "$case_dir.stderr"
  cmp "$tmp/$hook-package.expected.json" package.json
  test ! -e CHANGELOG.md
  test "$(git tag --list)" = v1.2.3
  test "$(git log -1 --format=%s)" = "fix: trigger $hook failure"
  test -z "$(git status --porcelain)"
)
run_pre_bump_lifecycle_failure prerelease
run_pre_bump_lifecycle_failure prebump

printf '%s\n' 'release workflow tests passed'
