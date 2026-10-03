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
printf '%s\n' "$lerna_dry_run" | grep -q 'bumping version in VERSION from 1.0.0 to 1.0.1' || {
  printf 'lerna package tag was not used as the bump boundary:\n%s\n' \
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
printf '%s\n' "$lerna_prerelease_dry_run" | grep -q 'bumping version in VERSION from 1.0.0 to 1.1.0' || {
  printf 'unstable package tag replaced the last stable bump boundary:\n%s\n' \
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

printf '%s\n' 'release workflow tests passed'
