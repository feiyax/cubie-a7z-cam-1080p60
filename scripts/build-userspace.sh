#!/bin/bash
set -euo pipefail
camera_root=$(cd "$(dirname "$0")/.." && pwd)
camera_out=${OUTPUT_DIR:-$camera_root/out}
camera_cc=${CC:-aarch64-linux-gnu-gcc}
mkdir -p "$camera_out"
common=(-std=gnu11 -Wall -Wextra -Werror -O2)
"$camera_cc" "${common[@]}" -shared -fPIC -pthread -I"$camera_root/upstream/openmax" "$camera_root/src/omx_eos_order.c" -ldl -o "$camera_out/omx_eos_order.so"
"$camera_cc" "${common[@]}" -shared -fPIC -pthread "$camera_root/src/profile_alias.c" -ldl -o "$camera_out/profile_alias.so"
"$camera_cc" "${common[@]}" -shared -fPIC "$camera_root/src/sink_write_trace.c" -ldl -o "$camera_out/sink_write_trace.so"
"$camera_cc" "${common[@]}" -shared -fPIC "$camera_root/tests/sink_delay_test.c" -ldl -o "$camera_out/sink_delay_test.so"
"$camera_cc" "${common[@]}" "$camera_root/src/repeat_pipeline.c" -ldl -o "$camera_out/repeat_pipeline"
"$camera_cc" "${common[@]}" "$camera_root/tests/test-rate-busy.c" -o "$camera_out/test-rate-busy"
(cd "$camera_out"; sha256sum omx_eos_order.so profile_alias.so > compat.sha256)
