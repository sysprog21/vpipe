#!/usr/bin/env bash
set -euo pipefail

modprobe vivid

if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl -d /dev/video0 --set-fmt-video=width=640,height=480,pixelformat=GREY >/dev/null
    v4l2-ctl -d /dev/video0 --set-parm=30 >/dev/null
    v4l2-ctl --list-devices
else
    echo "v4l2-ctl not found; enumerating /sys/class/video4linux instead" >&2
    ls -1 /sys/class/video4linux
fi
