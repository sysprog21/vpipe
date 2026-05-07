#!/usr/bin/env bash

set -eu

script_dir=$(cd -P "$(dirname "$0")" && pwd)
. "$script_dir/common.sh"
set_colors

if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
    printf "${YELLOW}Skipping git hook install outside a git worktree.${NC}\n"
    exit 0
fi

repo_root=$(git rev-parse --show-toplevel)
hook_dir=$(git rev-parse --git-path hooks)
mkdir -p "$hook_dir"

install_hook()
{
    local name="$1"
    local source="$repo_root/scripts/$name.hook"
    local target="$hook_dir/$name"

    [ -r "$source" ] || die "missing hook source: $source"

    if [ -L "$target" ] && [ "$(readlink "$target")" = "$source" ]; then
        return 0
    fi

    if [ -e "$target" ] && [ ! -L "$target" ]; then
        throw "refusing to replace existing hook %s" "$target"
    fi

    rm -f "$target"
    ln -s "$source" "$target"
    printf "Installed %s hook\n" "$name"
}

install_hook pre-commit
install_hook commit-msg
install_hook prepare-commit-msg
