#!/bin/sh
set -eu

bin=${1:?compatibility executable path required}
expected_version=${2:-0.1.0}

if [ ! -L "$bin" ]; then
  printf 'expected %s to be a symlink\n' "$bin" >&2
  exit 1
fi
if [ "$(readlink "$bin")" != 'csemver' ]; then
  printf 'expected %s to point to csemver\n' "$bin" >&2
  exit 1
fi
if [ "$("$bin" --version)" != "$expected_version" ]; then
  printf '%s\n' 'compatibility executable did not run the csemver binary' >&2
  exit 1
fi
printf '%s\n' 'compatibility executable test passed'
