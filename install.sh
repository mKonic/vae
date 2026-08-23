#!/bin/sh
# VAE — build and install from a clone
#
# Two ways to install, and the default is the one that suits a clone you intend
# to keep pulling:
#
#   linked (default)  the prefix gets symlinks into this clone, so
#                     `git pull && make config=dist` is live with no reinstall
#                     step. The clone becomes load-bearing -- move or delete it
#                     and the links dangle.
#   --copy            independent copies of the binaries and the engine's
#                     assets, so the clone is disposable. Every update needs
#                     this script run again.
#
# Installs under ~/.local by default and asks for no privilege. VAE needs none
# to run: it is a per-user tool that edits one user's projects. Pass --system,
# or run this as root, when you want a shared prefix anyway.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$here"

mode=link
prefix=
destdir=
config=dist
run_tests=1
run_build=1
action=install
jobs=8

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
	c_bold=$(printf '\033[1m'); c_red=$(printf '\033[31m')
	c_yellow=$(printf '\033[33m'); c_green=$(printf '\033[32m')
	c_dim=$(printf '\033[2m'); c_off=$(printf '\033[0m')
else
	c_bold=; c_red=; c_yellow=; c_green=; c_dim=; c_off=
fi

say()  { printf '%s\n' "$*"; }
step() { printf '%s==>%s %s\n' "$c_bold" "$c_off" "$*"; }
warn() { printf '%s warning %s %s\n' "$c_yellow" "$c_off" "$*" >&2; }
die()  { printf '%s error   %s %s\n' "$c_red" "$c_off" "$*" >&2; exit 1; }

usage() {
	cat <<EOF
VAE installer

  ./install.sh [options]

  --copy           install copies instead of symlinks into this clone
  --prefix DIR     install under DIR (default: ~/.local, or /usr/local as root)
  --system         shorthand for --prefix /usr/local
  --destdir DIR    stage every file under DIR instead of installing it, for a
                   package build. Implies --copy and system scope.
  --config NAME    which build to install: dist (default), release, debug
  --skip-build     install what is already built, do not build
  --skip-tests     do not run the unit suite before installing
  --uninstall      remove what a previous run installed
  -j N             parallel build jobs (default: $jobs)
  -h, --help       this

A linked install points at this clone's bin/ and at its assets, so a later
'git pull && make config=dist' takes effect immediately. That is also why a
linked install cannot survive the clone being moved.
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
	--copy)       mode=copy ;;
	--link)       mode=link ;;
	--prefix)     [ $# -ge 2 ] || die "--prefix needs a directory"; prefix=$2; shift ;;
	--prefix=*)   prefix=${1#--prefix=} ;;
	--system)     prefix=/usr/local ;;
	--destdir)    [ $# -ge 2 ] || die "--destdir needs a directory"; destdir=$2; shift ;;
	--destdir=*)  destdir=${1#--destdir=} ;;
	--config)     [ $# -ge 2 ] || die "--config needs a name"; config=$2; shift ;;
	--config=*)   config=${1#--config=} ;;
	--skip-build) run_build=0 ;;
	--skip-tests) run_tests=0 ;;
	--uninstall)  action=uninstall ;;
	-j)           [ $# -ge 2 ] || die "-j needs a count"; jobs=$2; shift ;;
	-j*)          jobs=${1#-j} ;;
	-h|--help)    usage; exit 0 ;;
	*)            die "unknown option: $1 (try --help)" ;;
	esac
	shift
done

case $config in
dist|release|debug) ;;
*) die "--config must be dist, release or debug" ;;
esac

# Capitalised the way premake names its output directory.
case $config in
dist)    build_dir=Dist-linux-x86_64 ;;
release) build_dir=Release-linux-x86_64 ;;
debug)   build_dir=Debug-linux-x86_64 ;;
esac

# A package build defaults to a system prefix whoever installs it will have:
# staging $HOME/.local into a package would put one machine's home directory in
# every copy of it.
if [ -z "$prefix" ]; then
	if [ -n "$destdir" ] || [ "$(id -u)" = 0 ]; then prefix=/usr/local; else prefix=$HOME/.local; fi
fi
case $prefix in
/*) ;;
*)  prefix=$(CDPATH= cd -- "$(dirname -- "$prefix")" 2>/dev/null && pwd)/$(basename -- "$prefix") \
        || die "cannot resolve --prefix $prefix" ;;
esac

if [ -n "$destdir" ]; then
	# Staging a package: nothing may point into this clone, because the clone
	# will not exist on the machine that installs the package.
	mode=copy
fi

bindir=$destdir$prefix/bin
libdir=$destdir$prefix/lib/vae
appdir=$destdir$prefix/share/applications
icondir=$destdir$prefix/share/icons/hicolor/scalable/apps
mimedir=$destdir$prefix/share/mime/packages

# What goes *inside* generated files -- the .desktop Exec line -- is the
# installed location, never the staging one.
run_bindir=$prefix/bin

# ------------------------------------------------------------------ privilege
#
# Only escalate for a prefix we cannot write, and only for the steps that write
# to it. Building stays unprivileged regardless: a build run as root leaves
# root-owned bin/ and bin-int/ in the clone that the next ordinary make cannot
# overwrite.
as_root=
if [ "$action" = install ] && { ! mkdir -p "$bindir" 2>/dev/null || [ ! -w "$bindir" ]; }; then
	command -v sudo >/dev/null 2>&1 \
		|| die "$prefix is not writable and sudo is not installed"
	as_root=sudo
	step "$prefix needs root; sudo will be used for the install steps only"
	sudo mkdir -p "$bindir"
fi

run() {
	if [ -n "$as_root" ]; then sudo "$@"; else "$@"; fi
}

# ------------------------------------------------------------------ uninstall

if [ "$action" = uninstall ]; then
	step "removing from $prefix"
	for f in "$bindir/vae-studio" "$bindir/vae-player" \
	         "$appdir/vae-studio.desktop" "$icondir/vae.svg" \
	         "$mimedir/vae.xml"; do
		if [ -e "$f" ] || [ -L "$f" ]; then
			run rm -f "$f"
			say "  removed  $f"
		fi
	done
	if [ -e "$libdir" ] || [ -L "$libdir" ]; then
		run rm -rf "$libdir"
		say "  removed  $libdir"
	fi

	command -v update-desktop-database >/dev/null 2>&1 \
		&& run update-desktop-database "$appdir" 2>/dev/null || true
	command -v update-mime-database >/dev/null 2>&1 \
		&& run update-mime-database "$destdir$prefix/share/mime" 2>/dev/null || true

	say "${c_green}done${c_off} — the clone itself is untouched"
	say "  ${c_dim}left alone: ~/.config/vae (settings, recent projects, logs)${c_off}"
	say "  ${c_dim}left alone: ~/Documents/VAE (your projects)${c_off}"
	exit 0
fi

# ---------------------------------------------------------------- dependencies

# Names verified against Arch; the others are best effort, and a wrong one shows
# up as your package manager complaining rather than as a silent failure.
distro_install_line() {
	id=; like=
	if [ -r /etc/os-release ]; then
		id=$(. /etc/os-release 2>/dev/null; printf '%s' "${ID:-}")
		like=$(. /etc/os-release 2>/dev/null; printf '%s' "${ID_LIKE:-}")
	fi
	case " $id $like " in
	*arch*|*cachyos*|*manjaro*)
		printf 'sudo pacman -S --needed base-devel premake shaderc vulkan-icd-loader wayland-protocols libxkbcommon' ;;
	*debian*|*ubuntu*)
		printf 'sudo apt install build-essential premake4 glslc libvulkan-dev libwayland-dev wayland-protocols libxkbcommon-dev libx11-dev' ;;
	*fedora*|*rhel*|*centos*)
		printf 'sudo dnf install gcc-c++ make premake glslc vulkan-loader-devel wayland-devel wayland-protocols-devel libxkbcommon-devel libX11-devel' ;;
	*suse*)
		printf 'sudo zypper install gcc-c++ make premake shaderc vulkan-devel wayland-devel wayland-protocols-devel libxkbcommon-devel libX11-devel' ;;
	*)
		printf 'install: a C++23 compiler, make, premake5, glslc (shaderc), the Vulkan loader, wayland-scanner and the X11/xkbcommon headers' ;;
	esac
}

if [ "$run_build" -eq 1 ]; then
	step "checking dependencies"
	missing=
	for tool in c++ make premake5 glslc wayland-scanner; do
		if command -v "$tool" >/dev/null 2>&1; then
			say "  $tool $(printf '%*s' $((16 - ${#tool})) '') ${c_green}ok${c_off}"
		else
			say "  $tool $(printf '%*s' $((16 - ${#tool})) '') ${c_red}missing${c_off}"
			missing="$missing $tool"
		fi
	done
	[ -z "$missing" ] || die "missing:$missing
  $(distro_install_line)"

	# Submodules before anything reads a vendored header. A clone without them
	# fails hundreds of lines later with a missing <GLFW/glfw3.h>.
	if [ ! -f VAE/vendor/GLFW/include/GLFW/glfw3.h ]; then
		step "fetching submodules"
		git submodule update --init --recursive
	fi

	step "compiling shaders"
	./scripts/CompileShaders.sh >/dev/null

	step "generating the build"
	premake5 gmake >/dev/null

	step "building with -j$jobs ${c_dim}($config)${c_off}"
	make config="$config" -j"$jobs" >/dev/null
fi

for binary in VAE-Studio VAE-Player; do
	[ -x "bin/$build_dir/$binary/$binary" ] \
		|| die "no $config build of $binary — drop --skip-build, or run: make config=$config -j$jobs"
done

if [ "$run_tests" -eq 1 ]; then
	step "running the unit suite"
	./bin/"$build_dir"/VAE-Tests/VAE-Tests >/dev/null \
		|| die "the unit suite failed; nothing was installed"
	./bin/"$build_dir"/VAE-Studio/VAE-Studio --selftest >/dev/null 2>&1 \
		|| die "the editor selftest failed; nothing was installed"
fi

# -------------------------------------------------------------------- install

step "installing to $prefix ${c_dim}($mode)${c_off}"
run mkdir -p "$bindir" "$libdir" "$appdir" "$icondir" "$mimedir"

# The engine finds its shaders and fonts by walking up from /proc/self/exe for a
# .vaeroot marker, and readlink resolves symlinks first. So a linked install
# lands the walk-up in the clone -- which is what makes it live -- and a copied
# one needs the assets and the marker beside the copied binaries.
if [ "$mode" = link ]; then
	run ln -sfn "$here/bin/$build_dir/VAE-Studio/VAE-Studio" "$libdir/VAE-Studio"
	run ln -sfn "$here/bin/$build_dir/VAE-Player/VAE-Player" "$libdir/VAE-Player"
	say "  linked   $libdir/VAE-Studio -> the clone"
else
	run install -m755 "bin/$build_dir/VAE-Studio/VAE-Studio" "$libdir/VAE-Studio"
	run install -m755 "bin/$build_dir/VAE-Player/VAE-Player" "$libdir/VAE-Player"
	run mkdir -p "$libdir/VAE/assets"
	run cp -r VAE/assets/. "$libdir/VAE/assets/"
	run touch "$libdir/.vaeroot"
	say "  copied   $libdir ${c_dim}(binaries, shaders, fonts)${c_off}"
fi

run ln -sfn "$libdir/VAE-Studio" "$bindir/vae-studio"
run ln -sfn "$libdir/VAE-Player" "$bindir/vae-player"
say "  linked   $bindir/vae-studio, $bindir/vae-player"

run install -m644 VAE/assets/icon.svg "$icondir/vae.svg"

# A compositor takes a Wayland window's icon from the .desktop file, matched on
# StartupWMClass -- which is why the window sets WM_CLASS to "VAE" and why this
# file names it. Without the match the window gets a generic icon whatever the
# app hands to GLFW.
tmp=$(mktemp)
cat > "$tmp" <<DESKTOP
[Desktop Entry]
Type=Application
Name=VAE Studio
GenericName=Visual app builder
Comment=Design an application and run it
Exec=$run_bindir/vae-studio %f
Icon=vae
Terminal=false
Categories=Development;IDE;Graphics;
MimeType=application/x-vae-screen;application/x-vae-component;application/x-vae-project;
StartupWMClass=VAE
StartupNotify=true
DESKTOP
run install -m644 "$tmp" "$appdir/vae-studio.desktop"

cat > "$tmp" <<'MIME'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-vae-screen">
    <comment>VAE screen</comment>
    <glob pattern="*.vaescreen"/>
    <sub-class-of type="application/xml"/>
  </mime-type>
  <mime-type type="application/x-vae-component">
    <comment>VAE component</comment>
    <glob pattern="*.vaecomp"/>
    <sub-class-of type="application/xml"/>
  </mime-type>
  <mime-type type="application/x-vae-project">
    <comment>VAE project</comment>
    <glob pattern="*.vaeproj"/>
    <sub-class-of type="application/json"/>
  </mime-type>
</mime-info>
MIME
run install -m644 "$tmp" "$mimedir/vae.xml"
rm -f "$tmp"
say "  wrote    the desktop entry, the icon and the VAE file types"

if [ -z "$destdir" ]; then
	command -v update-desktop-database >/dev/null 2>&1 \
		&& run update-desktop-database "$appdir" 2>/dev/null || true
	command -v update-mime-database >/dev/null 2>&1 \
		&& run update-mime-database "$prefix/share/mime" 2>/dev/null || true
fi

# -------------------------------------------------------------------- epilogue

say ""
say "${c_green}done${c_off} — $("$libdir/VAE-Studio" --version 2>/dev/null | tail -1)"

if [ -n "$destdir" ]; then
	say "  ${c_dim}staged under $destdir; nothing on this machine was changed${c_off}"
	exit 0
fi

case ":${PATH}:" in
*":$run_bindir:"*)
	say "  ${c_dim}$run_bindir is on your PATH${c_off}" ;;
*)
	warn "$run_bindir is not on your PATH; add it:"
	say "    fish:  fish_add_path $run_bindir"
	say "    bash:  echo 'export PATH=\"$run_bindir:\$PATH\"' >> ~/.bashrc" ;;
esac

say ""
say "  Start it:  ${c_bold}vae-studio${c_off}   ${c_dim}(or open a project from your file manager)${c_off}"
say "  Run an app: ${c_bold}vae-player <project.vaeproj>${c_off}"

if [ "$mode" = link ]; then
	say ""
	say "  Linked, so updating is just:"
	say "    ${c_bold}cd $here && git pull && make config=$config -j$jobs${c_off}"
	say "  ${c_dim}Moving or deleting this clone breaks the install.${c_off}"
fi
say ""
say "  Remove with: ${c_bold}$here/install.sh --uninstall${c_off}"
