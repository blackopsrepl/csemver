#!/bin/sh
set -eu

bin=${1:?compatibility executable path required}
case "$bin" in
  /*) ;;
  *) bin="$(pwd)/$bin" ;;
esac

if [ ! -L "$bin" ]; then
  printf 'expected %s to be a symlink\n' "$bin" >&2
  exit 1
fi
if [ "$(readlink "$bin")" != 'csemver' ]; then
  printf 'expected %s to point to csemver\n' "$bin" >&2
  exit 1
fi
if [ "$("$bin" --version)" != 'unknown' ]; then
  printf '%s\n' 'compatibility executable did not use upstream package lookup from this directory' >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/project/nested" "$tmp/project/dot.child"
printf '{"name":"version-context","version":"7.6.5"}\n' > "$tmp/project/package.json"
printf '{"name":"dot-context","version":"8.7.6"}\n' > "$tmp/project/dot.child/package.json"
if [ "$(cd "$tmp/project" && "$bin" --version)" != '7.6.5' ]; then
  printf '%s\n' 'compatibility executable did not read the nearest package version' >&2
  exit 1
fi
if [ "$(cd "$tmp/project/nested" && "$bin" --version)" != '7.6.5' ]; then
  printf '%s\n' 'compatibility executable did not find a parent package version' >&2
  exit 1
fi
if [ "$(cd "$tmp/project/dot.child" && "$bin" --version)" != '7.6.5' ]; then
  printf '%s\n' 'compatibility executable did not match upstream version lookup for dotted directories' >&2
  exit 1
fi

mkdir "$tmp/command-bin" "$tmp/drop-in-project"
ln -s "$bin" "$tmp/command-bin/commit-and-tag-version"
cd "$tmp/drop-in-project"
git init -q -b master
git config user.name 'C Semver Test'
git config user.email 'test@example.invalid'
git config commit.gpgSign false
printf '{"name":"drop-in-project","version":"1.2.3","repository":{"type":"git","url":"https://github.com/example/drop-in-project.git"}}\n' > package.json
git add package.json
git commit -qm 'chore: initialize unmodified drop-in project'
git tag -a v1.2.3 -m 'release 1.2.3'
git commit --allow-empty -qm 'fix: exercise PATH command substitution'
PATH="$tmp/command-bin:$PATH" commit-and-tag-version \
  --skip.changelog --skip.commit --skip.tag > "$tmp/drop-in.stdout" \
  2> "$tmp/drop-in.stderr"
printf '✔ bumping version in package.json from 1.2.3 to 1.2.4\n' \
  > "$tmp/drop-in.expected.stdout"
cmp "$tmp/drop-in.expected.stdout" "$tmp/drop-in.stdout"
test ! -s "$tmp/drop-in.stderr"
grep -q '"version": "1.2.4"' package.json
test -z "$(git tag --list v1.2.4)"
test "$(git status --porcelain)" = ' M package.json'
printf '%s\n' 'compatibility executable test passed'
