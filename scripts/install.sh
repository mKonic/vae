#!/usr/bin/env bash
# Installs VAE into a prefix, so Studio is something you launch rather than something you run out
# of a build directory.
#
#   ./scripts/install.sh                 # ~/.local
#   ./scripts/install.sh --prefix /usr   # system-wide (needs write access)
#   ./scripts/install.sh --uninstall
#
# Layout, and why: the binaries and the engine assets go together under lib/vae, with the .vaeroot
# marker beside them, because that is how the engine finds its shaders and fonts — it walks up from
# /proc/self/exe looking for the marker. bin/ gets symlinks, which is safe: readlink resolves them
# back to the real path, so the walk-up still lands in lib/vae.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${HOME}/.local"
CONFIG=Dist
UNINSTALL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --config) CONFIG="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

LIBDIR="$PREFIX/lib/vae"
BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"
ICONDIR="$PREFIX/share/icons/hicolor/scalable/apps"

if [ "$UNINSTALL" = 1 ]; then
    rm -rf "$LIBDIR"
    rm -f "$BINDIR/vae-studio" "$BINDIR/vae-player"
    rm -f "$APPDIR/vae-studio.desktop" "$ICONDIR/vae.svg"
    echo "removed VAE from $PREFIX"
    exit 0
fi

BUILD="$HERE/bin/${CONFIG}-linux-x86_64"
for binary in VAE-Studio VAE-Player; do
    if [ ! -x "$BUILD/$binary/$binary" ]; then
        echo "no $CONFIG build of $binary — run: make config=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]') -j8" >&2
        exit 1
    fi
done

# Shaders are compiled, not shipped as source: an installed engine has no compiler to hand.
if [ ! -d "$HERE/VAE/assets/shaders/cache" ]; then
    echo "no compiled shaders — run: ./scripts/CompileShaders.sh" >&2
    exit 1
fi

echo "==> installing to $PREFIX"
install -d "$LIBDIR" "$BINDIR" "$APPDIR" "$ICONDIR"
install -m755 "$BUILD/VAE-Studio/VAE-Studio" "$LIBDIR/VAE-Studio"
install -m755 "$BUILD/VAE-Player/VAE-Player" "$LIBDIR/VAE-Player"

# The engine's own assets, at the paths FileSystem::Asset() expects under the root.
install -d "$LIBDIR/VAE/assets"
cp -r "$HERE/VAE/assets/." "$LIBDIR/VAE/assets/"
: > "$LIBDIR/.vaeroot"

ln -sf "$LIBDIR/VAE-Studio" "$BINDIR/vae-studio"
ln -sf "$LIBDIR/VAE-Player" "$BINDIR/vae-player"

install -m644 "$HERE/VAE/assets/icon.svg" "$ICONDIR/vae.svg"
cat > "$APPDIR/vae-studio.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=VAE Studio
Comment=Visual app builder
Exec=$BINDIR/vae-studio %f
Icon=vae
Terminal=false
Categories=Development;IDE;Graphics;
MimeType=application/x-vae-screen;
StartupWMClass=VAE
DESKTOP

# The compositor takes a Wayland window's icon from the .desktop file, matched by StartupWMClass —
# which is why the window sets WM_CLASS to "VAE" and why this file names it.
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "$APPDIR" 2>/dev/null || true

echo "==> vae-studio and vae-player are in $BINDIR"
echo "    $("$LIBDIR/VAE-Studio" --version)"
