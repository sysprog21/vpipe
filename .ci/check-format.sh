#!/usr/bin/env bash

# Verify clang-format-20 conformance for tracked vpipe C/H sources.
# Pinned to clang-format-20: older versions diverge on the .clang-format
# style, which would cause CI flakes when the runner image rolls forward.

set -e -u -o pipefail

# Default to clang-format-20; allow override for local runs (e.g. when a
# distro packages the same version under a different binary name).
CLANG_FORMAT="${CLANG_FORMAT:-clang-format-20}"

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "Error: $CLANG_FORMAT not found (clang-format-20 is required; older versions differ in style)" >&2
    exit 1
fi

ret=0
while IFS= read -r -d '' file; do
    expected=$(mktemp)
    "$CLANG_FORMAT" "$file" >"$expected" 2>/dev/null
    if ! diff -u -p --label="$file" --label="expected coding style" "$file" "$expected"; then
        ret=1
    fi
    rm -f "$expected"
done < <(git ls-files -z -- 'kmod/*.c' 'kmod/*.h' 'user/*.c' 'user/*.h')

exit $ret
