#!/bin/bash
# Run under the shared camera lock; temporary load only, never force removal.
set -euo pipefail
camera_root=$(cd "$(dirname "$0")/.." && pwd)
camera_out=${OUTPUT_DIR:-$camera_root/out}
mode=${1:?Usage: driverctl.sh candidate|original}
case "$mode" in
    candidate | original) ;;
    *) exit 2 ;;
esac
test "$EUID" -eq 0
for tool in fuser pgrep modinfo rmmod modprobe insmod udevadm sha256sum uname cut; do
    command -v "$tool" >/dev/null || {
        echo "Required command unavailable: $tool" >&2
        exit 1
    }
done
if fuser /dev/video* /dev/v4l-subdev* /dev/cedar_dev >/dev/null 2>&1; then
    echo 'Refusing replacement: camera/codec device open' >&2
    exit 1
fi
if pgrep -x gst-launch-1.0 >/dev/null; then
    echo 'Refusing replacement: capture process exists' >&2
    exit 1
fi
if test "$mode" = candidate; then
    (
        cd "$camera_out"
        sha256sum -c modules.sha256
    )
    for module in imx214 vin_v4l2; do
        test "$(modinfo -F vermagic "$camera_out/$module.ko" | cut -d' ' -f1)" = "$(uname -r)"
    done
fi
# Verify packaged originals remain available before changing anything.
for module in imx214 vin_v4l2; do
    original=$(modinfo -n "$module")
    test -f "$original"
    sha256sum "$original"
done
remove_if_loaded() {
    if test -d "/sys/module/$1"; then
        rmmod "$1"
    fi
}
rollback() {
    echo 'Restoring packaged modules' >&2
    remove_if_loaded vin_v4l2 || return 1
    remove_if_loaded imx214 || return 1
    modprobe imx214 && modprobe vin_v4l2
}
trap '
    rc=$?
    trap - ERR
    rollback || echo "Rollback failed; reboot to packaged modules" >&2
    exit "$rc"
' ERR
remove_if_loaded vin_v4l2
remove_if_loaded imx214
if test "$mode" = candidate; then
    insmod "$camera_out/imx214.ko" receiver_deskew=5
    insmod "$camera_out/vin_v4l2.ko"
    for module in imx214 vin_v4l2; do
        test "$(cat "/sys/module/$module/srcversion")" = \
            "$(modinfo -F srcversion "$camera_out/$module.ko")"
    done
else
    modprobe imx214
    modprobe vin_v4l2
fi
udevadm settle --timeout=10
test -c /dev/video0
found_sensor=0
for name in /sys/class/video4linux/v4l-subdev*/name; do
    if test "$(cat "$name")" = imx214; then
        found_sensor=1
    fi
done
test "$found_sensor" = 1
trap - ERR
echo "Driver load complete: $mode (no persistent boot changes)"
