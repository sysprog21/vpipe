#!/usr/bin/env bash

# Run cppcheck static analysis on vpipe userspace sources.
#
# Kernel code under kmod/ is intentionally excluded: cppcheck's analysis of
# Linux kernel macros (per_cpu, container_of, EXPORT_SYMBOL_GPL, sparse
# annotations) produces extensive false positives without per-file
# suppressions. Kernel-side correctness is enforced by the kernel build
# (-Wall -Werror via kbuild) and sparse where applicable.
#
# CI mode: --max-configs=1 + --enable=warning for speed.

set -e -u -o pipefail

mapfile -t SOURCES < <(git ls-files -z -- 'user/*.c' | tr '\0' '\n')

if [ ${#SOURCES[@]} -eq 0 ]; then
    echo "No tracked userspace C source files found."
    exit 0
fi

# 120s budget is generous; expected runtime <30s with --max-configs=1.
timeout 120 cppcheck \
    -Iuser -Ikmod \
    --platform=unix64 \
    --enable=warning \
    --max-configs=1 --error-exitcode=1 --inline-suppr \
    --suppress=checkersReport --suppress=unmatchedSuppression \
    --suppress=missingIncludeSystem --suppress=noValidConfiguration \
    --suppress=normalCheckLevelMaxBranches \
    --suppress=preprocessorErrorDirective \
    -D_GNU_SOURCE -D__linux__ \
    "${SOURCES[@]}"
