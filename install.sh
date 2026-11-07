#!/bin/bash
# Install the driver as a DKMS module, so it is rebuilt on every kernel update.
# Run as root on the host.
#
#   sudo ./install.sh                     add, build and install for the running kernel
#   sudo ./install.sh --kernel <version>  build for another installed kernel instead
#   sudo ./install.sh --remove            remove every version and the source tree
#
# The --kernel form is for the gap after a kernel update: the running kernel has lost its build
# tree, so nothing can be built for it, while the kernel about to be booted has one.

set -eu

NAME=hid-logitech-headset
VERSION=0.4
SRC="/usr/src/$NAME-$VERSION"
HERE="$(cd "$(dirname "$0")" && pwd)"
KVER="$(uname -r)"

while [ $# -gt 0 ]; do
	case "$1" in
	--remove)
		REMOVE=1
		shift
		;;
	--kernel)
		KVER="${2:-}"
		[ -n "$KVER" ] || { echo "--kernel wants a version" >&2; exit 1; }
		shift 2
		;;
	*)
		echo "unknown argument: $1" >&2
		exit 1
		;;
	esac
done

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
command -v dkms >/dev/null || { echo "dkms is not installed" >&2; exit 1; }

if [ -n "${REMOVE:-}" ]; then
	for old in $(dkms status "$NAME" | sed -n "s|^$NAME/\([^,:]*\).*|\1|p" | sort -u); do
		dkms remove "$NAME/$old" --all || true
		rm -rf "/usr/src/$NAME-$old"
	done
	depmod -a
	echo "removed, the module is gone from the next boot"
	exit 0
fi

# Every kernel dkms builds for needs its headers.
if [ ! -d "/lib/modules/$KVER/build" ]; then
	echo "no headers for $KVER" >&2
	for dir in /lib/modules/*/build; do
		[ -d "$dir" ] || continue
		dir=${dir%/build}
		echo "  $0 --kernel ${dir##*/}" >&2
	done
	exit 1
fi

# Drop the hand-built module first, otherwise insmod and the dkms one both claim the device.
if [ "$KVER" = "$(uname -r)" ] && [ -e /sys/module/hid_logitech_headset ]; then
	echo "unloading the hand-built module"
	rmmod hid_logitech_headset
fi

# Remove every installed version, so an older one does not stay beside this one.
for old in $(dkms status "$NAME" | sed -n "s|^$NAME/\([^,:]*\).*|\1|p" | sort -u); do
	dkms remove "$NAME/$old" --all
	rm -rf "/usr/src/$NAME-$old"
done

rm -rf "$SRC"
mkdir -p "$SRC"
install -m 644 "$HERE/dkms.conf" "$SRC/dkms.conf"
install -m 644 "$HERE/hid-logitech-headset.c" "$SRC/hid-logitech-headset.c"
install -m 644 "$HERE/Makefile" "$SRC/Makefile"

dkms add "$NAME/$VERSION"
dkms build "$NAME/$VERSION" -k "$KVER"
dkms install "$NAME/$VERSION" -k "$KVER"
depmod -a "$KVER"

echo
dkms status "$NAME"
echo
if [ "$KVER" = "$(uname -r)" ]; then
	echo "modprobe $NAME, or replug the receiver. udev loads it by modalias from now on."
else
	echo "built for $KVER, which takes effect once you boot it."
	echo "the module loaded right now keeps running until then, and $(uname -r) has no copy left."
fi
