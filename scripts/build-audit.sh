#!/bin/bash
# Optional proprietary SDK headers must already be legally available locally.
set -euo pipefail
camera_root=$(cd "$(dirname "$0")/.." && pwd)
camera_out=${OUTPUT_DIR:-$camera_root/out}
camera_sdk=${VENCODER_INCLUDE:?Set VENCODER_INCLUDE to the installed SDK include directory}
camera_cc=${CC:-aarch64-linux-gnu-gcc}
mkdir -p "$camera_out"
"$camera_cc" -shared -fPIC -O2 -pthread -I"$camera_sdk" -I"$camera_sdk/sc_interface" -I"$camera_sdk/veInterface" -I"$camera_sdk/platform" -I"$camera_root/src" "$camera_root/src/benchmark_audit.c" -ldl -o "$camera_out/benchmark_audit.so"
