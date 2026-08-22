#!/usr/bin/env bash
# Initialises submodules and generates the makefiles. Run from anywhere.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> submodules"
git submodule update --init --recursive

echo "==> premake"
# premake5 5.0.0-beta8: the action is 'gmake', NOT 'gmake2'.
premake5 gmake

echo "==> done. build with:  make config=debug -j\$(nproc)"
