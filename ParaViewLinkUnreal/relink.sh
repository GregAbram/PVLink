#!/usr/bin/env bash
# relink.sh — Replace real file copies with symlinks back to the Claude workspace.
# Use after a fresh checkout to restore the live-edit workflow.
# Run from anywhere: bash Plugins/ParaViewLink/relink.sh

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "$0")" && pwd)"
CLAUDE_ROOT="/Users/gda/Documents/Claude/Projects/UE and PV"

make_link() {
    local dest="$1"    # file inside the UE project plugin
    local target="$2"  # canonical file in the Claude workspace

    if [[ ! -f "$target" ]]; then
        echo "  SKIP (not in Claude workspace): $(basename "$dest")"
        return
    fi
    if [[ -L "$dest" ]]; then
        echo "  already a symlink: $(basename "$dest")"
        return
    fi
    rm "$dest"
    ln -s "$target" "$dest"
    echo "  linked: $(basename "$dest")  ->  $target"
}

echo "=== relink: replacing copies with symlinks to Claude workspace ==="

# .uplugin at plugin root
uplugin="$PLUGIN_DIR/ParaViewLink.uplugin"
if [[ -f "$uplugin" ]]; then
    make_link "$uplugin" "$CLAUDE_ROOT/ParaViewLink/ParaViewLink.uplugin"
fi

# Source files one level down (.cpp / .h / .cs)
SRC="$PLUGIN_DIR/Source/ParaViewLink"
if [[ -d "$SRC" ]]; then
    for ext in cpp h cs; do
        for f in "$SRC"/*."$ext"; do
            [[ -e "$f" ]] || continue   # glob matched nothing
            fname="$(basename "$f")"
            make_link "$f" "$CLAUDE_ROOT/$fname"
        done
    done
fi

echo "=== done ==="
