#!/usr/bin/env bash

# Security checks for vpipe source files (kmod/ and user/).
#
# 1. Banned functions -- unsafe libc / kernel calls with safer alternatives
#    (kernel-side: prefer strscpy/scnprintf; userspace: prefer strn* family).
# 2. Credential / secret patterns -- catch accidental key leaks.
# 3. Dangerous preprocessor -- detect disabled hardening features.

set -u -o pipefail

failed=0

banned='(^|[^[:alnum:]_])(gets|sprintf|vsprintf|strcpy|stpcpy|strcat|atoi|atol|atoll|atof|mktemp|tmpnam|tempnam)[[:space:]]*\('
secrets='(password|secret|api_key|private_key|token)[[:space:]]*=[[:space:]]*"[^"]+'
dangerous_pp='#[[:space:]]*(undef|define)[[:space:]]+((_FORTIFY_SOURCE[[:space:]]+0)|(__SSP__))'
comment_only='^[[:space:]]*(//|/\*|\*|\*/)'

while IFS= read -r -d '' f; do
    code=$(grep -vE "$comment_only" "$f")

    if echo "$code" | grep -qE "$banned"; then
        echo "Banned function in $f:"
        grep -nE "$banned" "$f" | grep -vE "$comment_only"
        failed=1
    fi
    if echo "$code" | grep -iqE "$secrets"; then
        echo "Possible hardcoded secret in $f:"
        grep -inE "$secrets" "$f" | grep -vE "$comment_only"
        failed=1
    fi
    if echo "$code" | grep -qE "$dangerous_pp"; then
        echo "Dangerous preprocessor directive in $f:"
        grep -nE "$dangerous_pp" "$f" | grep -vE "$comment_only"
        failed=1
    fi
done < <(git ls-files -z -- 'kmod/*.c' 'kmod/*.h' 'user/*.c' 'user/*.h')

if [ $failed -eq 0 ]; then
    echo "Security checks passed."
fi

exit $failed
