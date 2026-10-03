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
printf '%s\n' 'compatibility executable test passed'
