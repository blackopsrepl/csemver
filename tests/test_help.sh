#!/bin/sh
set -eu
# Normalize the runtime-specific invocation label to the drop-in command name.
bin=${1:-./build/commit-and-tag-version}
case "$bin" in
  /*) ;;
  *) bin="$(pwd)/$bin" ;;
esac
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
actual=$("$bin" --help)
printf '%s\n' "$actual" | diff -u \
  "$script_dir/fixtures/commit-and-tag-version-help.txt" -
