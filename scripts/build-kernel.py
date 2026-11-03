#!/usr/bin/env python3
"""Build isolated ARM64 camera modules against matching, unmodified vendor headers."""

import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--headers", type=pathlib.Path, default=os.environ.get("KERNEL_HEADERS"))
p.add_argument(
    "--work", type=pathlib.Path, default=os.environ.get("WORK_DIR", ROOT / ".work")
)
p.add_argument(
    "--out", type=pathlib.Path, default=os.environ.get("OUTPUT_DIR", ROOT / "out")
)
p.add_argument(
    "--cross-compile", default=os.environ.get("CROSS_COMPILE", "aarch64-linux-gnu-")
)
p.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "4")))
a = p.parse_args()
if not a.headers or not (a.headers / "Makefile").is_file():
    p.error("--headers must name the matching vendor kernel headers")
release = a.headers / "include/config/kernel.release"
if not release.is_file() or release.read_text().strip() != "6.6.98-5-aw2511":
    p.error("Only vendor kernel release 6.6.98-5-aw2511 is qualified")
if a.jobs < 1:
    p.error("--jobs must be positive")
a.work = a.work.resolve()
a.out = a.out.resolve()
a.headers = a.headers.resolve()
a.work.mkdir(parents=True, exist_ok=True)
a.out.mkdir(parents=True, exist_ok=True)
if any(" " in str(x) for x in (ROOT, a.work, a.out, a.headers)):
    p.error("Kernel make requires paths without spaces")
spec = importlib.util.spec_from_file_location(
    "fetch", ROOT / "scripts/fetch-upstream.py"
)
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)
bsp_spec = json.loads((ROOT / "sources.json").read_text())["bsp"]
archive = fetch.fetch(bsp_spec, a.work / "downloads" / bsp_spec["file"])
build = pathlib.Path(tempfile.mkdtemp(prefix="kernel-", dir=a.work))
print("Isolated build directory:", build, flush=True)
log = (build / "build.log").open("w")


def run(args):
    print(" ".join(map(str, args)), flush=True)
    subprocess.run(
        list(map(str, args)), stdout=log, stderr=subprocess.STDOUT, check=True
    )


try:
    headers = build / "headers"
    shutil.copytree(a.headers, headers, symlinks=True)
    hostcc = os.environ.get("HOSTCC", "gcc")
    run(
        [
            hostcc,
            "-O2",
            "-o",
            headers / "scripts/basic/fixdep",
            headers / "scripts/basic/fixdep.c",
        ]
    )
    run(
        [
            hostcc,
            "-O2",
            "-I" + str(headers / "scripts/mod"),
            "-o",
            headers / "scripts/mod/modpost",
            *[
                headers / "scripts/mod" / f
                for f in ["modpost.c", "file2alias.c", "sumversion.c", "symsearch.c"]
            ],
        ]
    )
    bsp = build / bsp_spec["archive_root"]
    with tarfile.open(archive) as tar:
        members = [
            m
            for m in tar.getmembers()
            if any(
                m.name.startswith(bsp_spec["archive_root"] + "/" + x)
                for x in ["drivers/vin/", "include/"]
            )
        ]
        tar.extractall(build, members=members, filter="data")
    manifest = json.loads((ROOT / "upstream/manifest.json").read_text())["files"]
    for item in manifest:
        if item["baseline_only"]:
            original = bsp / pathlib.Path(item["path"]).relative_to("kernel")
            if fetch.sha(original) != item["sha256"]:
                raise RuntimeError("BSP baseline mismatch: " + str(original))
    generated = build / "imx214-1080p60.h"
    run(["python3", ROOT / "scripts/generate-mode.py", "--output", generated])
    if (
        generated.read_bytes()
        != (ROOT / "kernel/drivers/vin/modules/sensor/imx214-1080p60.h").read_bytes()
    ):
        raise RuntimeError("Generated mode differs from versioned mode")
    for source in (ROOT / "kernel").rglob("*"):
        if source.is_file():
            shutil.copy2(source, bsp / source.relative_to(ROOT / "kernel"))
    sensor = build / "sensor"
    sensor.mkdir()
    shutil.copy2(bsp / "drivers/vin/modules/sensor/imx214.c", sensor)
    shutil.copy2(generated, sensor)
    (sensor / "Kbuild").write_text(
        "obj-m += imx214.o\nccflags-y += -I"
        + str(bsp / "drivers/vin/modules/sensor")
        + " -I"
        + str(bsp / "include")
        + " -I"
        + str(bsp / "include/uapi")
        + "\n"
    )
    vin = bsp / "drivers/vin"
    makefile = (vin / "Makefile").read_text()
    makefile = makefile[makefile.index("obj-$(CONFIG_CSI_VIN) += vin_v4l2.o") :]
    (vin / "Kbuild").write_text(
        makefile.replace("obj-$(CONFIG_CSI_VIN) += vin_v4l2.o", "obj-m += vin_v4l2.o")
        + "\nccflags-y += -I"
        + str(bsp / "include")
        + " -I"
        + str(bsp / "include/uapi")
        + "\n"
    )
    for module in (sensor, vin):
        run(
            [
                "make",
                "-C",
                headers,
                "M=" + str(module),
                "ARCH=arm64",
                "CROSS_COMPILE=" + a.cross_compile,
                "-j" + str(a.jobs),
                "modules",
            ]
        )
    for source in [sensor / "imx214.ko", vin / "vin_v4l2.ko"]:
        shutil.copy2(source, a.out / source.name)
    (a.out / "modules.sha256").write_text(
        "".join(
            fetch.sha(a.out / n) + "  " + n + "\n" for n in ["imx214.ko", "vin_v4l2.ko"]
        )
    )
    print("Built modules:", a.out)
finally:
    log.close()
    print("Build log retained:", build / "build.log")
