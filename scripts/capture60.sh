#!/bin/bash
# Run under flock and sudo; all outputs must be on disk.
set -euo pipefail
if (($# < 1 || $# > 2)); then
    echo "Usage: $0 OUTPUT_H264 [FRAMES=600]" >&2
    exit 2
fi
camera_root=$(cd "$(dirname "$0")/.." && pwd)
camera_out=${OUTPUT_DIR:-$camera_root/out}
camera_output=$(realpath -m -- "$1")
camera_frames=${2:-600}
[[ "$camera_frames" =~ ^[1-9][0-9]*$ ]]
((camera_frames <= 10000000))
test -d "$(dirname "$camera_output")"
test ! -e "$camera_output"
case $(findmnt -n -o FSTYPE -T "$(dirname "$camera_output")") in
    tmpfs | ramfs)
        echo 'Capture output must be on disk.' >&2
        exit 1
        ;;
esac
(
    cd "$camera_out"
    sha256sum --status -c modules.sha256
)
for module in imx214 vin_v4l2; do
    test "$(cat "/sys/module/$module/srcversion")" = \
        "$(modinfo -F srcversion "$camera_out/$module.ko")"
done
test "$(cat /sys/module/imx214/parameters/receiver_deskew)" = 5
exec bash "$camera_root/scripts/with-camera60-fixes.sh" \
    timeout --signal=INT --kill-after=5 "$((camera_frames / 60 + 30))s" \
    gst-launch-1.0 --no-fault -e \
    v4l2src device=/dev/video0 en-awisp=1 en-largemode=0 io-mode=dmabuf \
    "num-buffers=$camera_frames" \
    ! video/x-raw,format=NV12,width=1920,height=1080,framerate=60/1 \
    ! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 \
    ! omxh264videoenc target-bitrate=12000000 \
    ! queue max-size-buffers=0 max-size-bytes=4194304 max-size-time=0 \
    ! filesink "location=$camera_output"
