# Cubie A7Z IMX214 1080p60 camera capture

Genuine **1920×1080 at 59.987 fps** from an IMX214 on the Radxa Cubie A7Z / Allwinner A733, with DMA-BUF camera input to the hardware H.264 encoder. The tested pipeline captured and independently decoded **all 11,100 pictures over 185 seconds**, including the final picture, without source sequence gaps.

This repository contains the sensor/VIN changes, narrowly scoped userspace compatibility adapters, build and rollback tools, and validation source. It does not contain vendor runtime binaries, private recordings or hardware certification for other boards. See [vendor limitations and fixes](docs/vendor-limitations.md), [tests and measured statistics](docs/validation.md), and [source provenance/licensing](docs/PROVENANCE.md).

## What changes

- A source-backed 2×2-binned sensor mode: 432 MHz pixel clock, 1080 Mbps per lane, HTS 5008, VTS 1438, and receiver clock delay 5. It produces real 1920×1080 samples at nominal 60 fps; changing a framerate label alone is insufficient.
- VIN rate negotiation and a 12-buffer pool scoped to the new plain 1080p60 mode. Original 1080p30 timing and buffer policy remain available.
- A process-local OMX adapter defers the vendor's premature EOS notification until actual drained output, retaining the final encoded picture.
- A process-local ISP adapter selects the existing 1080 profile for the new rate and applies a 60 fps exposure policy through vendor APIs. It does not change actual timing to 30 fps.
- Bounded queues separate capture scheduling and compressed disk writes. A controlled 650 ms write stall retained all 600 pictures in a separate test.

No timestamp rewriting, synthetic frame insertion or dummy encodes are used. The measured rate is 59.987 fps, not exactly 60.000. Linux scheduling, finite storage buffering and per-buffer clock sampling still impose limits; details and failed trials are documented.

## Tested stack and prerequisites

The hardware results apply to Cubie A7Z/A733 with IMX214, vendor kernel **6.6.98-5-aw2511**, GStreamer 1.26.2 and the vendor gst-omx/ISP/Cedar libraries listed in [runtime-baseline.sha256](runtime-baseline.sha256). Other runtime fingerprints are deliberately rejected by the ABI-specific adapters. This is not a generic upstream Linux camera driver.

Build on Linux with Python **3.12 or newer**, GCC, GNU make, an ARM64 cross compiler (`aarch64-linux-gnu-gcc`), and matching complete vendor kernel headers, including generated configuration and `Module.symvers`. Python's safe tar extraction filter is required. The verified cross compiler was GCC 13.3; the vendor kernel reports GCC 14.2. Dependencies on Debian/Ubuntu include `build-essential gcc-aarch64-linux-gnu python3 kmod`.

Obtain the matching headers from your vendor image/package and put them at a local path. The build makes a disposable copy and rebuilds its host tools; it never edits supplied headers. Source archives are pinned and SHA256 checked. Network access is needed for the first BSP download.

```sh
python3 scripts/build-kernel.py --headers /path/to/linux-headers-6.6.98-5-aw2511
scripts/build-userspace.sh
scripts/test-host.sh
```

Outputs go to ignored `out/`; cached downloads and isolated build logs go to ignored `.work/`. `WORK_DIR`, `OUTPUT_DIR`, `KERNEL_HEADERS`, `CROSS_COMPILE`, `HOSTCC` and `JOBS` allow other locations/toolchains. Userspace compilation accepts `CC`; use `CC=gcc` for a native ARM64 build. Keep build/output directories on disk, with sufficient free space. Paths used by kernel make must not contain spaces.

The versioned `kernel/` files are the authoritative implementation. The build verifies original BSP hashes, overlays these files, and requires the generated mode to match the versioned header byte-for-byte. There is no second maintained patch copy. See [development and history](docs/development.md) for the exact upstream baseline and agent commit policy.

## Load and capture on the board

Copy the checkout and built `out/` directory to a disk-backed directory on the board. Install the usual vendor GStreamer tools, `v4l2-ctl`, `fuser` (`psmisc`), `pgrep` (`procps`), `flock` (`util-linux`), `kmod`, and `udevadm` (`udev`). Stop other camera/codec clients. Use the same lock for all camera experiments; it is a tiny coordination file, not storage for video.

From that checkout:

```sh
flock /tmp/camera-codec.lock sudo bash scripts/driverctl.sh candidate
mkdir -p runs
flock /tmp/camera-codec.lock sudo bash scripts/capture60.sh "$PWD/runs/camera60.h264" 11100
```

The capture command refuses an existing output file and RAM-backed storage. It checks staged module hashes, loaded source versions, receiver delay, and runtime library fingerprints. The default frame count is 600. It uses a nonleaky raw queue of four buffers and a nonleaky compressed queue capped at 4 MiB, with a bounded timeout. Rebuilding changes module source versions when relevant: the loader compares against the staged build rather than a hardcoded private binary.

The production launcher loads only the EOS/ISP fixes for that process. It excludes the audit, timestamp overlay and fault injection. It uses existing ISP controls and saved context; the qualified state was Auto exposure, automatic gain and automatic white balance. For manual operation, use the documented [control units](docs/vendor-limitations.md#isp-profile-and-exposure-policy). Camera brightness cannot be made independent of scene lighting or exposure time. Remove any protective film before judging optical quality; no arbitrary white-balance correction is included.

## Rollback

```sh
flock /tmp/camera-codec.lock sudo bash scripts/driverctl.sh original
```

The loader refuses open camera/codec handles, normally unloads VIN then sensor, and attempts packaged-module recovery if insertion or verification fails. It never forces module removal. The original module files, library files, device tree, module dependency files, initramfs and boot entry are untouched. **Reboot returns to packaged camera drivers.** Omitting `with-camera60-fixes.sh` also omits the process-local userspace adapters. Do not add these modules or preloads to global boot configuration without separate qualification.

## Audit and reproduce the measurements

Host CI checks callback behavior, source hashes, generated mode and commit metadata. It cannot establish camera timing, DMA behavior, image quality or hardware stability.

For full on-board evidence, first build the optional audit using the exact installed `libcedarc-dev-2.0.0-arm64` version `1.0.7` SDK's include directory containing `vencoder.h` and its dependency headers. The internal BSP codec header is a different ABI and must not be substituted. These external headers are not redistributed here:

```sh
VENCODER_INCLUDE=/path/to/vendor-sdk/include scripts/build-audit.sh
flock /tmp/camera-codec.lock sudo python3 scripts/qualify.py own-run --frames 11100
```

The runner requires existing Auto exposure/automatic gain, does not change controls, preserves preexisting saved ISP contexts, and keeps private evidence under ignored `runs/`. It enables the timestamp overlay, source-to-encoder audit, output write trace and resource sampling. A separate test-only `--inject-write-delay-ms 650 --frames 600` reproduces one selected compressed-write stall. Never publish generated captures, raw logs or context files without reviewing privacy.

Follow [validation.md](docs/validation.md) to independently decode every frame, reconcile source/DMA/PTS links and output bytes, and distinguish the normal two-clock timestamp variation from missing or mismatched pictures. The public glyph validator requires the complete stream by default; an old missing-final-frame result is a failure.

## License

Original standalone contributions are MIT licensed. Imported and modified driver/mode sources retain their upstream GPL notices; the combined Intel-derived sensor work is distributed under GPLv2. Khronos OpenMAX headers retain their permissive notices. See [LICENSE](LICENSE), [NOTICE](NOTICE), and [provenance](docs/PROVENANCE.md). External proprietary runtime libraries must be obtained separately from the vendor.
