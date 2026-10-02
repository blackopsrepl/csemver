#!/bin/sh
set -eu
bin=${1:-./build/csemver}
expected_version=${2:-0.1.0}
help=$($bin --help)
printf '%s\n' "$help" | grep -Fq 'Usage: csemver [options]'
printf '%s\n' "$help" | grep -q -- '--release-as'
printf '%s\n' "$help" | grep -q -- '--dry-run'
printf '%s\n' "$help" | grep -Fq 'Changelog sections (0 all, N latest)'
for option in --first-release --packageFiles --bumpFiles --issuePrefixes \
    --lerna-package --tag-force --git-tag-fallback --noBumpWhenEmptyChanges; do
    if ! printf '%s\n' "$help" | grep -Fq -- "$option"; then
        printf 'help is missing %s\n' "$option" >&2
        exit 1
    fi
done
[ "$("$bin" --version)" = "csemver $expected_version" ]
if "$bin" --definitely-not-an-option >/dev/null 2>&1; then
    printf '%s\n' 'unknown option unexpectedly succeeded' >&2
    exit 1
fi
printf '%s\n' 'cli smoke tests passed'
