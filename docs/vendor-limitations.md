# Vendor-stack limitations and the scope of the fixes

These findings concern the tested Cubie A7Z, Radxa Camera 13M 214 and pinned software stack. They are not claims about every Allwinner or Radxa product. [Provenance](PROVENANCE.md) identifies the source revisions; [validation](validation.md) records the sanitized measurements and test limits.

## Sensor and capture negotiation

The original [IMX214 driver](https://github.com/radxa/allwinner-bsp/blob/f9a496c24bf6b7cc31c4b5c716ece58b2c287f01/drivers/vin/modules/sensor/imx214.c) supplied a 1080p mode at approximately 30 fps and no native 60 fps mode. Merely requesting 60 fps or reducing exposure could not produce 60 distinct captures per second. The new mode changes the complete readout/PLL configuration, using a [pinned Intel register table](https://github.com/intel/intel-camera-drivers/blob/f06e5fc31eb36edb15118cd04b29c53b56dc126a/drivers/media/i2c/crlmodule/crl_imx214_configuration.h) as its primary source. Qualified settings are 432 MHz pixel clock, 1080 Mbps per lane, HTS 5008 and VTS 1438, predicting 59.987469 fps.

The original 3840 × 2160 mode's nominal 30 fps label is not its measured cadence: capture remained approximately 20.4146 fps with short exposure. A 2560 × 1440 ISP output used the same 3840 × 2160 sensor readout and measured approximately 20.41454 fps. This project does **not** fix 4K at 30 fps or establish camera capture above 1080p at 30 fps. Synthetic encoder throughput is a separate result and must not be substituted for sensor capture capability.

The [VIN implementation](https://github.com/radxa/allwinner-bsp/blob/f9a496c24bf6b7cc31c4b5c716ece58b2c287f01/drivers/vin/vin-video/vin_video.c) also needed an idle rate-change path that reapplies format throughout the sensor/CSI/ISP/TDM pipeline. The observed client order was `S_INPUT → S_PARM(default) → S_FMT → G_PARM → S_PARM(requested rate) → STREAMON`; changing only the sensor left receiver configuration stale. The original helper could store an initial unspecified `0/0` and later reject a valid request based on the already-zero current numerator; VIN also masked the helper error. The fix normalizes unspecified `0/0`, rejects half-zero intervals and rejects rate changes while buffers are allocated. Real ioctl tests verified `EBUSY` with unchanged rate, followed by a successful 30 → 60 → 30 transition after buffer release.

The original sensor `SET_FPS` handler was a successful no-op. Live vendor `SET_FPS` is deliberately rejected by the new fixed mode: a frame-length-only change cannot safely substitute for its full PLL and receiver transition. Select the supported idle V4L2 mode before allocating capture buffers.

## Receiver margin and buffering

The initial faster 1200 Mbps link candidate failed with packet and TDM errors. Even the qualified 1080 Mbps mode required receiver sampling delay 5 on this board/cable/sensor; default delay 7 produced corruption at the new rate. Original 30 fps retains delay 7. This is a board-specific qualified setting, not a universal electrical-margin guarantee.

The original five-buffer capture pool occasionally starved at 60 fps under downstream scheduling. A queue alone was insufficient. The new normal 1080p60 mode uses at least 12 capture buffers plus a bounded four-buffer raw queue; the original 30 fps pool remains five. These buffers absorb scheduling variation, not unlimited stalls.

## ISP profile and exposure policy

The installed ISP lacked the new 1080p60 profile key and fell back to a 13 MP profile. The opt-in [profile adapter](../src/profile_alias.c) changes only the matching profile lookup to the existing 1080p30 profile, preserving actual 60 fps sensor timing. This is a compatibility workaround for the same crop/binning, not a newly calibrated vendor 60 fps profile.

Without a corresponding AE policy, the ISP requested approximately 90 ms integration while the sensor correctly clamped it to approximately 16.55 ms. That mismatch could mislead exposure/gain decisions. The adapter applies the vendor's exported AE-table update for 60 fps before its worker starts, without forcing unsupported sensor `SET_FPS`. Tracing subsequently showed requested and actual integration agreeing at approximately 9.993 ms. Related [vendor ISP source](https://github.com/lindenis-org/lindenis-v853-external/tree/bf42476c330aa41aada55e7c030e4be22e46107d/eyesee-mpp/middleware/sun8iw21/media/LIBRARY/libisp) supports the helper's meaning; it is not a proven complete matching source release for the installed ISP602 binary. Runtime fingerprint guards remain necessary.

Manual ISP gain uses Q8 units (256 is unity), whereas the sensor exposure/gain API uses Q4 (gain 16 is unity). A cached, nonvolatile gain-control readback is not independent proof of physical analog-gain register state; integration register readback is stronger evidence for actual exposure. Correct manual use also requires disabling automatic gain. Wrong control usage caused per-frame warnings; this is distinct from an ISP algorithm failure. The production path uses Auto. Remaining optional unsupported ISP ioctl warnings do not establish that all AWB statistics are broken: changing AWB outputs and usable images were observed.

The initial severe color cast and softness had a physical confound: protective film remained in front of the lens. Removing it substantially improved the image, including the unchanged 30 fps reference. No Bayer flip, arbitrary color filter or calibration adjustment was applied to compensate for the film. Fresh images were visually clear; no chart-based absolute color-accuracy claim is made.

## Encoded output completion and storage

The installed OpenMAX component emitted output EOS before its final asynchronous encoded payload. The final picture was retrieved internally but absent from the closed file. The scoped [EOS adapter](../src/omx_eos_order.c) defers that event until the actual EOS-bearing output callback. It does not insert dummy pictures or sleep to guess completion. Short lifecycle tests and a sustained 11,100-picture run verified complete final-frame delivery.

A separate approximately 603 ms storage/output stall caused 28 lost source frames despite clean CSI. A bounded 4 MiB compressed queue separates encoder callbacks from filesink writes. A deliberately delayed approximately 650.6 ms write then completed with all 600 source and decoded pictures intact. This handles the tested stall class, not arbitrary storage outages. Successful write return is not an `fsync` durability guarantee. Compressed output and OSD still involve CPU copies; the zero-copy claim applies specifically to raw camera image storage.

## Timing that is not a frame-loss defect

The final sustained run delivered all 11,100 pictures over 185.022762 seconds at 59.987214 fps, with continuous source sequences and clean sampled receiver status. Linux scheduling still produces variable dequeue, encode-retrieval and write latencies; throughput success does not establish hard real-time deadlines.

Driver timestamps and pipeline PTS use different clock epochs. The [GStreamer conversion](https://github.com/GStreamer/gstreamer/blob/100c21e1faf68efe7f3830b6e9f856760697ab48/subprojects/gst-plugins-good/sys/v4l2/gstv4l2src.c) separately samples clocks per buffer. The final run's offset spanned 43 µs, with equal successive-third medians and only 2 µs first-to-last difference. This bounded sampling variation is not accumulating drift or missing pictures. The strict 10 µs constant-offset check remains explicitly false; exact source/DMA/encoder/decoded association checks pass. Timestamps were not rewritten to hide the distinction.
