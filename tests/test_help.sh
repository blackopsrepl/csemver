#!/bin/sh
set -eu
# The README's CLI options block is the documented help text. Keep the two
# identical so the reference and the binary cannot drift apart.
bin=${1:-./build/csemver}
case "$bin" in
  /*) ;;
  *) bin="$(pwd)/$bin" ;;
esac
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/csemver-help.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
awk '
  /^## CLI options$/ { section = 1; next }
  section && /^```text$/ { block = 1; next }
  block && /^```$/ { exit }
  block { print }
' "$script_dir/../README.md" > "$tmp/documented"
if [ ! -s "$tmp/documented" ]; then
  printf '%s\n' 'README.md has no CLI options block' >&2
  exit 1
fi
"$bin" --help > "$tmp/actual"
if ! diff -u "$tmp/documented" "$tmp/actual" >&2; then
  printf '%s\n' 'csemver --help differs from the README CLI options block' >&2
  exit 1
fi
printf '%s\n' 'help text matches README'
