#!/usr/bin/env bash
# Generates the Wayland protocol headers GLFW's Wayland backend #includes.
#
# GLFW's CMake does this itself; we don't use its CMake, so premake calls this instead. Output goes
# OUTSIDE the submodule (VAE/vendor-generated/) so `git status` in the submodule stays clean.
set -euo pipefail
cd "$(dirname "$0")/.."

SCANNER=${WAYLAND_SCANNER:-wayland-scanner}
XMLDIR=VAE/vendor/GLFW/deps/wayland
OUT=VAE/vendor-generated/wayland

command -v "$SCANNER" >/dev/null || { echo "error: wayland-scanner not found (pacman -S wayland)"; exit 1; }
mkdir -p "$OUT"

# The exact list GLFW's src/CMakeLists.txt generates; wl_init.c #includes all of them by name.
protocols=(
    wayland.xml
    viewporter.xml
    xdg-shell.xml
    idle-inhibit-unstable-v1.xml
    pointer-constraints-unstable-v1.xml
    relative-pointer-unstable-v1.xml
    fractional-scale-v1.xml
    xdg-activation-v1.xml
    xdg-decoration-unstable-v1.xml
)

generated=0
for xml in "${protocols[@]}"; do
    src="$XMLDIR/$xml"
    [ -f "$src" ] || { echo "error: missing $src (did submodules init?)"; exit 1; }
    base="${xml%.xml}"
    hdr="$OUT/$base-client-protocol.h"
    code="$OUT/$base-client-protocol-code.h"
    if [ ! -f "$hdr" ] || [ "$src" -nt "$hdr" ]; then
        "$SCANNER" client-header "$src" "$hdr"
        "$SCANNER" private-code  "$src" "$code"
        generated=$((generated + 1))
    fi
done

echo "  wayland protocols: $generated generated, ${#protocols[@]} total -> $OUT"
