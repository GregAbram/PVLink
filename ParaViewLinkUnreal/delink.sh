#!/usr/bin/env bash
# delink.sh — Replace every symlink in this plugin directory with a real copy.
# Use before committing to git or sharing the project.
# Run from anywhere: bash Plugins/ParaViewLink/delink.sh

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "$0")" && pwd)"

replace_link() {
    local link="$1"
    local target
    target="$(readlink "$link")"
    if [[ ! -e "$target" ]]; then
        echo "  SKIP (broken): $(basename "$link") -> $target"
        return
    fi
    # Copy to a temp file, then atomically replace the symlink
    cp "$target" "${link}.tmp"
    mv "${link}.tmp" "$link"
    echo "  copied: $(basename "$link")  <-  $target"
}

echo "=== delink: replacing symlinks with copies ==="

# .uplugin at plugin root
uplugin="$PLUGIN_DIR/ParaViewLink.uplugin"
if [[ -L "$uplugin" ]]; then
    replace_link "$uplugin"
fi

# Source files one level down (.cpp / .h / .cs)
SRC="$PLUGIN_DIR/Source/ParaViewLink"
if [[ -d "$SRC" ]]; then
    for ext in cpp h cs; do
        for f in "$SRC"/*."$ext"; do
            [[ -L "$f" ]] || continue   # only process symlinks
            replace_link "$f"
        done
    done
fi

echo "=== done ==="
