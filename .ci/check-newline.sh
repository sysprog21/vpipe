#!/usr/bin/env bash

# Ensure tracked text sources end with a trailing newline.
# Covers C/H, shell, Python, Markdown, and Makefiles -- broader than kbox
# because vpipe ships scripts and docs alongside source.

set -e -u -o pipefail

ret=0
while IFS= read -rd '' f; do
    # Skip binaries (file --mime-encoding emits "binary" for those).
    if file --mime-encoding "$f" | grep -qv binary; then
        if [ -n "$(tail -c1 <"$f")" ]; then
            echo "Warning: No newline at end of file $f"
            ret=1
        fi
    fi
done < <(git ls-files -z -- '*.c' '*.h' '*.sh' '*.py' '*.md' 'Makefile' '*/Makefile')

exit $ret
