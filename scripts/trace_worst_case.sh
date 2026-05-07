#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 output.dat preemptirqsoff|wakeup_rt command [args...]" >&2
    exit 1
fi

out=$1
shift
tracer=$1
shift

case "$tracer" in
    preemptirqsoff|wakeup_rt) ;;
    *)
        echo "unsupported tracer: $tracer" >&2
        exit 1
        ;;
esac

trace-cmd record -o "$out" -p "$tracer" -- "$@"
