#!/usr/bin/env bash

# Shared helpers for repository scripts and git hooks.

set_colors()
{
    if [ -t 1 ]; then
        RED='\033[1;31m'
        GREEN='\033[1;32m'
        YELLOW='\033[1;33m'
        CYAN='\033[1;36m'
        NC='\033[0m'
    else
        RED=''
        GREEN=''
        YELLOW=''
        CYAN=''
        NC=''
    fi
}

die()
{
    echo "error: $*" >&2
    exit 1
}

throw()
{
    local fmt="$1"
    shift
    # shellcheck disable=SC2059
    printf "\n${RED}[!] ${fmt}${NC}\n" "$@" >&2
    exit 1
}

check_ci()
{
    if [ -n "${CI:-}" ] || [ -d "/home/runner/work" ]; then
        exit 0
    fi
}

resolve_script_dir()
{
    local source="${BASH_SOURCE[0]}"
    while [ -L "$source" ]; do
        local dir
        dir=$(cd -P "$(dirname "$source")" && pwd) || return 1
        source=$(readlink "$source") || return 1
        [[ $source != /* ]] && source="$dir/$source"
    done
    cd -P "$(dirname "$source")" && pwd
}
