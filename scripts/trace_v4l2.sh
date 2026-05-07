#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 output.dat [command ...]" >&2
    exit 1
fi

out=$1
shift || true

if [ "$#" -gt 0 ]; then
    trace-cmd record -o "$out" -e sched_switch -e irq_handler_entry -e irq_handler_exit -e v4l2 -- "$@"
else
    trace-cmd record -o "$out" -e sched_switch -e irq_handler_entry -e irq_handler_exit -e v4l2 sleep 5
fi
