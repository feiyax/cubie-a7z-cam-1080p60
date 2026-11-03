#!/bin/bash
# Process-local, fingerprint-guarded adapters; never replace packaged libraries.
set -euo pipefail
(($# > 0)) || {
    echo "Usage: $0 command [args...]" >&2
    exit 2
}
camera_root=$(cd "$(dirname "$0")/.." && pwd)
camera_out=${OUTPUT_DIR:-$camera_root/out}
(
    cd "$camera_out"
    sha256sum --status -c compat.sha256
)
if ! sha256sum --status -c "$camera_root/runtime-baseline.sha256"; then
    echo 'Unsupported vendor library fingerprint; refusing ABI-specific adapters.' >&2
    exit 1
fi
export A733_IMX214_60_PROFILE=1080p30 A733_IMX214_AE_FPS=60
export LD_PRELOAD="$camera_out/omx_eos_order.so:\
$camera_out/profile_alias.so\
${A733_EXTRA_PRELOAD:+:$A733_EXTRA_PRELOAD}\
${LD_PRELOAD:+:$LD_PRELOAD}"
exec "$@"
