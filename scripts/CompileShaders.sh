#!/usr/bin/env bash
# Compiles every GLSL shader under VAE/assets/shaders to SPIR-V beside itself.
# Kept as a script (not a premake rule) so it can be swapped for a Slang invocation when a
# second GPU backend lands — see design/architecture.md §4.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=VAE/assets/shaders
OUT="$SRC/cache"
mkdir -p "$OUT"

shopt -s nullglob
found=0
for f in "$SRC"/*.vert "$SRC"/*.frag "$SRC"/*.comp; do
    found=1
    out="$OUT/$(basename "$f").spv"
    echo "  glslc $(basename "$f")"
    glslc --target-env=vulkan1.3 -O "$f" -o "$out"
done
[ "$found" = 0 ] && echo "  (no shaders yet)"
exit 0
