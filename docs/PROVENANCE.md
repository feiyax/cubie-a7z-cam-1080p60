# Source provenance and distribution boundaries

This repository prepares a reproducible source release of an IMX214 1080p60 camera experiment on Radxa Cubie A7Z. It is not an official Radxa, Allwinner, Intel, Khronos or GStreamer release. The original project included private hardware evidence; the public repository contains selected source and sanitized findings instead of copying that workspace or its history.

## Source origins

| Component | Primary source and fixed revision | Treatment |
|---|---|---|
| Kernel packaging | [radxa-pkg/linux-aw2511](https://github.com/radxa-pkg/linux-aw2511/tree/cb131cf8f6d6622beea7846b59fb2a7f8a24b782), release `6.6.98-5` | Compatibility provenance; obtain matching headers separately |
| Kernel | [radxa/kernel](https://github.com/radxa/kernel/tree/61f3fe291c7a0ca97a038194bb242b133971b76e) | Matching kernel source provenance |
| BSP sensor and VIN | [radxa/allwinner-bsp](https://github.com/radxa/allwinner-bsp/tree/f9a496c24bf6b7cc31c4b5c716ece58b2c287f01) | Two scoped source files are imported under `kernel/` byte-for-byte in a baseline commit, then modified in separate commits; fetch remaining build dependencies |
| IMX214 mode tables | [Intel camera drivers](https://github.com/intel/intel-camera-drivers/blob/f06e5fc31eb36edb15118cd04b29c53b56dc126a/drivers/media/i2c/crlmodule/crl_imx214_configuration.h) | Exact upstream header retained under `upstream/intel/`; generate the adapted GPL-2.0-only table with attribution |
| OpenMAX headers / reference | [GStreamer gst-omx](https://github.com/GStreamer/gst-omx/tree/7efb633970f5816e377f3c861fa0d63698740cc1), tag `1.18.3` | Exact public API headers retained under `upstream/openmax/`; preserve per-file Khronos grants and distinguish LGPL implementation source |
| V4L2 timestamp interpretation | [GStreamer 1.26.2](https://github.com/GStreamer/gstreamer/blob/100c21e1faf68efe7f3830b6e9f856760697ab48/subprojects/gst-plugins-good/sys/v4l2/gstv4l2src.c) | Reference for per-buffer clock conversion; not a claim of byte-identical vendor plugin code |
| ISP profile and exposure-policy reference | [Lindenis vendor ISP source](https://github.com/lindenis-org/lindenis-v853-external/tree/bf42476c330aa41aada55e7c030e4be22e46107d/eyesee-mpp/middleware/sun8iw21/media/LIBRARY/libisp) | Related-generation implementation used to understand public exported APIs; not redistributed as the installed ISP602 source |

The upstream manifest, fetch manifest and scripts are authoritative for the exact download URLs and SHA256 values used by builds. A commit identifier establishes source identity; a checked archive/file hash also detects an unexpected download. Generated or extracted vendor files belong in ignored build directories, not in the public source inventory unless intentionally reviewed and licensed.

## Exact upstream baseline

The history preserves the human initialization commit, then imports the two upstream BSP files and scoped Intel/OpenMAX reference headers without changing their notices or bytes. A later commit applies the project changes at the same paths, so ordinary Git diffs show the modifications. The BSP files retain their upstream relative paths below `kernel/`. The baseline revision is the BSP pin above. Baseline SHA256 values:

| Upstream relative path | SHA256 |
|---|---|
| `drivers/vin/modules/sensor/imx214.c` | `c3cd309c8f8993baa1b93fa43d8c4e6c5a3dc68d360e2b7370c6d629f0dc7057` |
| `drivers/vin/vin-video/vin_video.c` | `3c2074e28be739dae753fd9dd92471f4382fe34739ffbda124a6e79c07ce578e` |

These are upstream-baseline hashes, not hashes of the subsequently modified files. The import does not cover the entire BSP tree.

## Attribution and modifications

See [NOTICE](../NOTICE), [LICENSE](../LICENSE), and [GPL version 2](../LICENSES/GPL-2.0-only.txt). Original standalone tools are MIT; vendored BSP files, vendor-derived patches and the generated sensor table are not covered by a blanket MIT grant. Preserve original copyright, author and license statements. The generated mode should also identify its generator and local changes.

The mode changes include binning/crop/PLL timing, truthful mode selection, receiver timing selection and bounded capture buffering. The original compatibility adapters address callback ordering, ISP profile lookup, automatic-exposure policy and observable validation. Descriptions of these changes do not imply ownership of the vendor implementation or permission to redistribute its binaries.

## Runtime dependencies and limits

The target's ISP and codec libraries must be installed separately from the board's supported packages. This repository supplies neither those proprietary libraries nor proprietary SDK dumps. Compatibility fingerprints document the binaries inspected during qualification; a matching hash is not a license. Public API headers retain their own notices. Optional tools needing additional vendor headers must obtain them separately under the vendor's applicable terms.

The ISP workaround calls exported functions and deliberately checks the inspected runtime compatibility. Related open vendor source helped establish function meaning, but the inspected ISP602 binary was not reconstructed from a proven matching public source release. Avoid presenting that related source as a complete corresponding-source release for the proprietary runtime.

## Privacy and evidence

Do not publish private workspace history, raw capture or kernel logs, camera images/video, saved ISP contexts, binary dumps/disassembly, host inventories, network addresses, machine identifiers, credentials, or user-specific filesystem paths. Public examples should use configurable paths and placeholders. Aggregate measured results may be documented with their limitations, without importing private images or logs as fixtures.

Test output generated by users may contain camera content, timestamps, paths and hardware identifiers. Keep such output ignored by version control and review it separately before sharing. Do not infer optical motion or calibrated color accuracy from timestamps or hashes alone.

## AI-assisted contribution records

New migration commits should describe the actual change and retain the requested agent/model attribution. Commit trailers must identify the actual contributor and configured model/effort for that commit: the implementation lead uses `gpt-6-astra` with `high` reasoning effort; the licensing/provenance reviewer uses `gpt-6-astra` with `medium` reasoning effort. Do not infer a single effort setting for all contributors. The exact model snapshot is not exposed; record that fact rather than inventing a dated snapshot or implying measured model identity. Agent attribution describes assistance, not ownership of upstream vendor work. Preserve the repository's existing human initialization commit and remote configuration. Preparation does not itself authorize pushing or publishing.
