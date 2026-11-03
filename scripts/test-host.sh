#!/bin/bash
set -euo pipefail
camera_root=$(cd "$(dirname "$0")/.." && pwd)
camera_work=${WORK_DIR:-$camera_root/.work}
mkdir -p "$camera_work"
camera_test=$(mktemp -d "$camera_work/host-test.XXXXXX")
trap 'rm -rf "$camera_test"' EXIT
camera_cc=${HOSTCC:-gcc}
common=(-std=gnu11 -Wall -Wextra -Werror -O2 -I"$camera_root/upstream/openmax")
"$camera_cc" "${common[@]}" \
    -shared -fPIC -pthread \
    "$camera_root/src/omx_eos_order.c" \
    -ldl -o "$camera_test/omx_eos_order.so"
"$camera_cc" "${common[@]}" \
    -shared -fPIC \
    "$camera_root/tests/mock_omx.c" -o "$camera_test/libOmxCore.so"
"$camera_cc" "${common[@]}" \
    "$camera_root/tests/test_omx_order.c" \
    -L"$camera_test" -lOmxCore -o "$camera_test/test_omx_order"
LD_LIBRARY_PATH="$camera_test" LD_PRELOAD="$camera_test/omx_eos_order.so" \
    "$camera_test/test_omx_order"
"$camera_cc" -std=gnu11 -Wall -Wextra -Werror -fsyntax-only \
    "$camera_root/tests/test-rate-busy.c"
python3 "$camera_root/tests/test_metadata.py"
python3 "$camera_root/scripts/fetch-upstream.py"
python3 "$camera_root/scripts/generate-mode.py" --output "$camera_test/mode.h"
cmp "$camera_test/mode.h" "$camera_root/kernel/drivers/vin/modules/sensor/imx214-1080p60.h"
python3 "$camera_root/scripts/qualify.py" --help >/dev/null
python3 "$camera_root/scripts/scrub-public.py"
echo 'Host checks passed (not hardware qualification).'
