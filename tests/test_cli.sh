#!/bin/sh
set -eu
bin=${1:-./build/csemver}
expected_version=${2:-0.1.0}
help=$("$bin" --help)
program=${bin##*/}
printf '%s\n' "$help" | grep -Fq "Usage: $program [options]"
printf '%s\n' "$help" | grep -q -- '--release-as'
printf '%s\n' "$help" | grep -q -- '--dry-run'
printf '%s\n' "$help" | grep -q -- '--preset'
printf '%s\n' "$help" | grep -Fq 'How many releases of changelog you want to generate.'
for option in --first-release --packageFiles --bumpFiles --lerna-package \
    --tag-force --git-tag-fallback --noBumpWhenEmptyChanges --scripts --skip \
    --sign --signoff --silent --changelogHeader; do
    if ! printf '%s\n' "$help" | grep -Fq -- "$option"; then
        printf 'help is missing upstream option %s\n' "$option" >&2
        exit 1
    fi
done
[ "$("$bin" --version)" = "$expected_version" ]
if ! "$bin" --definitely-not-an-option --help >/dev/null 2>&1; then
    printf '%s\n' 'upstream-compatible unknown option was rejected' >&2
    exit 1
fi
for option in \
    '--commitUrlFormat=https://example.invalid/{{hash}}' \
    '--commit-url-format=https://example.invalid/{{hash}}' \
    '--compareUrlFormat=https://example.invalid/{{previousTag}}...{{currentTag}}' \
    '--compare-url-format=https://example.invalid/{{previousTag}}...{{currentTag}}' \
    '--issueUrlFormat=https://example.invalid/issues/{{id}}' \
    '--issue-url-format=https://example.invalid/issues/{{id}}' \
    '--userUrlFormat=https://example.invalid/{{user}}' \
    '--user-url-format=https://example.invalid/{{user}}' \
    '--preMajor'; do
    if ! "$bin" "$option" --help >/dev/null 2>&1; then
        printf 'upstream-compatible option was rejected: %s\n' "$option" >&2
        exit 1
    fi
done
for option in \
    --dryRun --firstRelease --commitAll --noVerify --tagForce --gitTagFallback; do
    if ! "$bin" "$option" --help >/dev/null 2>&1; then
        printf 'upstream camelCase option was rejected: %s\n' "$option" >&2
        exit 1
    fi
done
for option in \
    --noDryRun --no-first-release --noSign --no-signoff --no-no-verify \
    --no-commit-all --noSilent --no-tag-force --noGitTagFallback \
    --noPreMajor --no-noBumpWhenEmptyChanges; do
    if ! "$bin" "$option" --help >/dev/null 2>&1; then
        printf 'upstream negated boolean option was rejected: %s\n' "$option" >&2
        exit 1
    fi
done
for option in '--releaseAs=patch' '--tagPrefix=release-' \
    '--lernaPackage=core' '--npmPublishHint=custom publish'; do
    if ! "$bin" "$option" --help >/dev/null 2>&1; then
        printf 'upstream camelCase option was rejected: %s\n' "$option" >&2
        exit 1
    fi
done
printf '%s\n' 'cli smoke tests passed'
