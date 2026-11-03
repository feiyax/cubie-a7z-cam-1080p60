# Validation and limits

The implementation was tested on a Radxa Cubie A7Z (Allwinner A733) with an IMX214 camera, Debian 13 (`DEBIAN_VERSION_FULL=13.7` in the observed OS release), vendor kernel `6.6.98-5-aw2511`, GStreamer 1.26.2 and vendor gst-omx package `1.18.3-1.1`. The vendor encoder came from `libcedarc-dev-2.0.0-arm64` version `1.0.7`. Results describe this tested stack, not every camera, image or kernel. Kernel patches require the matching source/configuration and hardware checks.

## Hardware results

| Test | Independently decoded result | Source timing/result |
|---|---|---|
| Sustained 1920×1080 camera capture and hardware H.264 | **11,100 / 11,100**, including final frame | 185.022762 s, **59.987214 fps**, no source sequence gaps |
| One injected 650 ms compressed-file write delay | **600 / 600** | 59.986164 fps, no source gaps; 39 captures and 39 encoder retrievals during the blocked write |
| Retained original 1080p30 mode | **300 / 300** | 30.027404 fps after a separately specified 2 s startup exclusion; no source gaps |
| Production launcher, without audit or overlay | **120 / 120** | Short count-only smoke, not a full provenance test |

The sustained run used actual camera buffers rather than a generated source. Source sequence→exported DMA FD→imported IOMMU address→encoder input→output PTS association was logged explicitly. Independent software decoding recovered every burned sequence number and presentation timestamp and matched them to the encoder submission records. No duplicate, missing interior or missing final picture was accepted. The receiver status was clean in the tested runs.

The sustained audit observed 24 plane-FD imports and no raw encoder-input allocation, instrumented raw-frame copies, DMA maps, camera mappings or camera memcpy bytes. This is evidence for the tested direct raw-buffer path, not a claim that every CPU copy is absent: the small timestamp bitmap and compressed output are copied. The audit cannot detect every possible uninstrumented copy.

## Development and regression matrix

Failed trials are included to distinguish the qualified configuration from plausible settings that did not work. They are diagnostic observations on the tested board, not universal electrical limits.

| Trial | Observation and disposition |
|---|---|
| Nominal 60 fps request with original mode | Negotiated caps alone did not establish physical 60 fps; sensor timing readback still showed the original mode. Rejected as proof of capture rate. |
| Initial 1200 Mbps/lane, 480 MHz pixel-clock mode | Capture and encode probes stalled with MIPI packet and TDM frame/hblank errors despite short exposure and confirmed registers. Rejected. |
| 1080 Mbps/lane, 432 MHz, HTS 5008, VTS 1438; receiver delay 7 | Approximately 60 fps capture, but packet errors and severe vertical striping. Rejected. |
| Same sensor mode, receiver delay 5 | Short 180-frame gate had clean receiver status and smooth images. Selected for longer testing. |
| Same sensor mode, receiver delay 9 | No captured inputs before the bounded deadline; packet errors. Rejected. |
| Original five-buffer capture pool | Occasional encoder scheduling delays exhausted reusable raw buffers. A larger downstream raw queue alone did not fix this and worsened gaps. |
| Scoped 12-buffer pool plus bounded raw queue of 4 | Short 600-frame gate retained five startup missing source IDs, then stable capture; it was not called lossless. Subsequent sustained tests and the final combined run had no source gaps. |
| Original OMX EOS handling | Final native packet retrieved but absent from the file/decoder; rejected as complete delivery. |
| Corrected EOS ordering | All 1, 7 and 180 pictures delivered in separate audited runs; four pipelines in one process delivered 1, 7, 180 and 7 pictures. The latter are count/lifecycle checks. |
| Combined path before compressed queue | One 602.630 ms retrieval stall and 28 missing source IDs. Failed source-continuity qualification. |
| Combined path with 4 MiB compressed queue | All 11,100 source and decoded pictures, plus the separate 650 ms delayed-write test summarized above. |
| Allocated-buffer rate change | Real V4L2 test allocated three buffers and checked that a rate change returns `EBUSY` without changing the mode. After buffer release, rate changes succeeded. |
| Rate-parameter compatibility | Original 30↔60 transitions and unspecified 0/0 normalization tested; half-zero requests returned `EINVAL` without changing the mode. No stream was needed for this negative ioctl test. |
| Host EOS callback tests | Event ordering, flush cancellation, EOS-bearing payload, unrelated events/components, handle reuse and late-callback guards checked with mocks. These are software regression tests, not capture evidence. |

The final sensor clock settings predict 59.987469 fps, consistent with measured capture. The original 30 fps path retains its 800 Mbps link, 320 MHz pixel clock, VTS 2128, receiver delay 7 and five-buffer policy. The 12-buffer change is scoped to the new normal 1080p60 mode, not all camera modes.

## Resource measurements

These values are from the final sustained run, including audit instrumentation, dynamic timestamp overlay and the sink-write probe. They are not an isolated production-launcher benchmark.

| Measurement | Observed result |
|---|---:|
| Mean process CPU, 100% = one core | 34.17% |
| Mean total system CPU across eight cores | 4.88% |
| Minimum available memory | 596,704 KiB / 582.72 MiB |
| Maximum sampled temperature | 65.49 °C |
| VE / ISP / VIN interrupt deltas | 11,100 / 22,212 / 88,848 |
| Swap-out / swap-in | 462 / 0 pages |
| Allocation stalls / directly reclaimed pages, whole run | 21 / 1,258 |
| After 2 s: allocation stalls / direct-scan pages | 1 / 52 |
| OOM events | 0 |

Memory pressure was observed, not assumed absent. Source continuity survived the measured reclaim activity, but this does not establish behavior under arbitrary memory pressure or thermal conditions. The CPU denominators differ: process CPU is normalized to one core; system CPU is normalized across all eight cores. Neither is a power measurement.

## Timing, buffering and EOS

With a fixed 1 s warmup, the sustained source interval was 184.022407 s at 59.987260 fps. Maximum driver interval was 17.353 ms. Source dequeue→native bitstream retrieval latency had median 30.020 ms and maximum 52.187 ms; successive-third medians were 30.243, 30.188 and 29.618 ms. There was no accumulating retrieval backlog, but maximum retrieval interval was 43.408 ms. **This is not a hard per-frame scheduling guarantee.**

A 4 MiB compressed queue separates disk writes from OMX output-buffer reuse. Inspection of the installed plugin confirmed that the normal output path copies compressed data to an independently allocated GstBuffer before passing it downstream, then releases the OMX buffer. A preceding run without this queue had a 602.630 ms retrieval stall and 28 missing source IDs and was rejected. The controlled 650 ms sink-delay test subsequently demonstrated uninterrupted capture and encoding through that bounded stall. Finite buffering cannot absorb arbitrary or sustained slow storage.

For the sustained run, 3,546 successful writes accounted for all 277,498,326 file bytes and all 11,100 retrieved packets, including 35 initial codec-header bytes. Maximum write-call duration was 99.328 ms; source dequeue→write-return latency had median 51.989 ms and maximum 163.930 ms. During the injected-delay test it reached 723.289 ms. Peak retrieved compressed bytes minus completed-write bytes were 187,777 B normally and 1,093,236 B during injection; both drained to zero. This balance includes filesink buffering and in-flight writes, **not exact queue occupancy**. Write return means kernel acceptance, not fsync, durable media or network delivery.

The EOS adapter addresses a demonstrated vendor component ordering defect: an output EOS event could precede the final encoded payload. It defers that specific event until the actual EOS-bearing output callback, without extra input pictures, dummy encodes or arbitrary drain sleeps. Independent short tests delivered all 1, 7 and 180 pictures; the sustained all-frame result confirms the final picture is retained. Same-process restart tests were additional count checks rather than substitutes for the fully audited run.

## Timestamp interpretation

The overlay contains **presentation timestamps**, not UTC wall time. Exact encoder-input, encoder-output and decoded-overlay PTS associations passed. A stricter check requiring a perfectly constant driver→pipeline epoch offset within 10 µs did **not** pass the sustained run: the offset spanned 43 µs, with 2 µs first-to-last change and identical third medians. That failure remains explicit; the threshold was not relaxed to make the result pass.

The installed V4L2 plugin was inspected and uses two separately sampled clocks. For a valid driver timestamp, its conversion has the form:

```text
pipeline_PTS = driver_timestamp + pipeline_clock_now - base_time - monotonic_now
```

Transient sampling/preemption differences can therefore change the offset without frame loss or accumulating clock drift. The matching upstream implementation is in [GStreamer 1.26.2 gstv4l2src.c](https://github.com/GStreamer/gstreamer/blob/100c21e1faf68efe7f3830b6e9f856760697ab48/subprojects/gst-plugins-good/sys/v4l2/gstv4l2src.c#L1198). No timestamp rescaling or synthetic clock substitution was used to hide this behavior. This explanation is specific to the inspected path, not proof that an entire vendor binary equals upstream.

## Reproduce with your own recordings

Build and configure the target as described in the [README](../README.md). Use the instrumented qualification runner for source/DMA/timestamp evidence; the normal production launcher deliberately omits that instrumentation. Inspect available runner options before capture:

```sh
python3 scripts/qualify.py --help
```

The audit library is optional and requires the vendor encoder SDK headers, obtained separately under their applicable terms. They are not distributed here. Build the userspace adapters and the audit library with `scripts/build-userspace.sh` and `scripts/build-audit.sh`; the latter requires `VENCODER_INCLUDE` to name the SDK include directory and `CC` to select the target compiler. Copy the matching build outputs to the target. Use Python 3.12 or newer for the project (the kernel build uses tar extraction filtering); `gst-launch-1.0`, `v4l2-ctl`, `findmnt` and the documented vendor runtime are target dependencies.

With the intended drivers already loaded, existing Auto exposure/automatic gain enabled, and no other camera process, run the qualification under a shared lock (use the same lock for all camera commands):

```sh
flock /tmp/camera-codec.lock sudo python3 scripts/qualify.py qualification --fps 60 --frames 11100 --timeout 215
```

The runner requires a fresh label, writes disk-backed output under ignored `runs/` by default, and restores preexisting saved ISP contexts. It does not alter ISP controls. Set `--build-dir`, `--output-root` or `--context-dir` when the defaults do not match your installation. A separate bounded fault test can use a new label, `--frames 600` and `--inject-write-delay-ms 650`; do not inject faults in normal production capture.

Record at least 11,100 frames at the tested rate, leaving more than 180 s after a predeclared 1 s warmup. Preserve the closed H.264 file, complete audit log, actual command/configuration, source/build hashes and before/after hardware status. Keep recordings and logs local: they may contain private imagery, filesystem paths or device details.

On the validation host, install dependencies in an isolated environment:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install av numpy Pillow
```

Use your own file paths in place of `recording.h264` and `capture.log`:

```sh
python validation/validate_dynamic.py recording.h264 capture.log --source camera --output glyph.json
python validation/review_camera.py capture.log glyph.json --fps 60 --min-seconds 180 --warmup-seconds 1 --output camera.json
python validation/review_packet_bytes.py recording.h264 capture.log --output packets.json
python validation/review_sink.py recording.h264 capture.log --output sink.json
```

Require all submitted frames, including the final frame; do not enable a legacy final-EOS-loss allowance. Check `complete_dynamic_stream_pass`, explicit source/input/output association, exact count and file-byte reconciliation. The packet-size reviewer is report-only: inspect its JSON count and mismatch fields rather than relying on its exit status. The camera reviewer may exit nonzero on the strict timestamp-offset check described above: inspect that specific failure and measured offsets rather than treating all failures as harmless. Missing fields or records do not constitute a pass. Sink reconstruction requires the scoped write trace and a serial append-only filesink without seeks or rewrites.

Review first, middle and final decoded images for corruption; a changed timestamp does not prove a new exposure. Compare cadence windows and latency trends, and record process CPU as a percentage of one core, memory availability, reclaim/stall/OOM counters and temperature. Require clean receiver status and retained 30 fps/restart behavior. Decode all frames rather than just sampling the overlay. The glyph validator streams images but retains compact per-frame metadata; its snapshot requires Pillow.

Earlier optical color/softness observations were confounded by a removable protective film in front of the lens. Remove optical obstructions before judging image quality. Later unobstructed images lacked the prior severe striping; no calibrated color-accuracy claim is made without a reference target. Noise/compression differences between pictures alone do not prove physical motion.

## Host tests are not hardware certification

Run `scripts/test-host.sh` for the supplied host checks (Linux host, GCC, Python 3.12+, Bash and Git). Mock callback tests can detect EOS event-ordering/reset regressions without the board. They cannot prove sensor rate, DMA behavior, receiver integrity, image quality or sustained timing. CI runs of these tests must not be described as hardware qualification. The measurements above are a sanitized summary of manual hardware testing; private recordings and raw session logs are intentionally not distributed.
