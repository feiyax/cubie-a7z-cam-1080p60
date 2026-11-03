#!/usr/bin/env python3
"""Independently decode native OSD digits and match actual encode submission log.
python3 validation/validate_dynamic.py stream.h264 run.log --output result.json
No OCR dependencies: sample known 5x7 bitmap cells after independent FFmpeg decode.
"""

import argparse
import av
import collections
import json
import re
from pathlib import Path
import numpy as np

FONT = np.array(
    [
        [14, 17, 19, 21, 25, 17, 14],
        [4, 12, 4, 4, 4, 4, 14],
        [14, 17, 1, 2, 4, 8, 31],
        [30, 1, 1, 14, 1, 1, 30],
        [2, 6, 10, 18, 31, 2, 2],
        [31, 16, 16, 30, 1, 1, 30],
        [14, 16, 16, 30, 17, 17, 14],
        [31, 1, 2, 4, 8, 8, 8],
        [14, 17, 17, 14, 17, 17, 14],
        [14, 17, 17, 15, 1, 1, 14],
        [31, 16, 16, 30, 16, 16, 16],
        [31, 4, 4, 4, 4, 4, 4],
    ]
)
BITS = ((FONT[:, :, None] >> np.arange(4, -1, -1)) & 1).astype(bool)


def read_cell(gray, row, col):
    x = 32 + 12 + 24 * col
    y = 32 + 12 + 40 * row
    patch = gray[y : y + 28, x : x + 20]
    bits = np.median(patch.reshape(7, 4, 5, 4), axis=(1, 3)) > 128
    distance = np.count_nonzero(BITS != bits, axis=(1, 2))
    idx = int(distance.argmin())
    return ("0123456789FT"[idx], int(distance[idx]))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("stream")
    p.add_argument("log")
    p.add_argument("--output", required=True)
    p.add_argument("--snapshot", type=int, default=150)
    p.add_argument("--mode", choices=["dynamic", "static", "off"], default="dynamic")
    p.add_argument("--source", choices=["camera", "synthetic"], default="camera")
    a = p.parse_args()
    text = Path(a.log).read_text(errors="replace")
    records = []
    for line in text.splitlines():
        if ("SYNTH_FRAME " if a.source == "synthetic" else "TS_FRAME ") not in line:
            continue
        values = dict(re.findall(r"(\w+)=(-?\d+)", line))
        if all(k in values for k in ("seq", "pts", "rc")):
            records.append({k: int(v) for k, v in values.items()})
    expected = {r["seq"]: r for r in records if r["rc"] == 0}
    audit = {k: [] for k in ("INPUT", "ENCODE", "USED")}
    for line in text.splitlines():
        for kind in audit:
            if "BENCH " + kind + " " in line:
                values = dict(re.findall(r"(\w+)=(-?\d+)", line))
                if "id" in values and "pts" in values:
                    audit[kind].append((int(values["id"]), int(values["pts"])))
    association_log_pass = (
        bool(audit["INPUT"]) and audit["INPUT"] == audit["ENCODE"] == audit["USED"]
    )
    if a.source == "synthetic":
        summaries = [
            dict(re.findall(r"(\w+)=(-?\d+)", l))
            for l in text.splitlines()
            if "SYNTH_SUMMARY " in l
        ]
        association_log_pass = bool(summaries) and summaries[-1].get("status") == "0"
        # Synthetic source checks buffer return before each next input; source audit
        # plus its completion status substitutes for camera INPUT/USED trace lists.
    duplicate_encode_sequences = len(records) - len(set(r["seq"] for r in records))
    frames = []
    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    snapshot = None
    last = None
    with av.open(a.stream) as container:
        for index, frame in enumerate(container.decode(video=0)):
            # Use decoded luma plane through explicit gray conversion; avoids RGB allocations.
            gray = frame.to_ndarray(format="gray")
            rows = []
            errors = []
            for row, n in [(0, 7), (1, 11)]:
                cells = [read_cell(gray, row, col) for col in range(n)]
                rows.append("".join(c[0] for c in cells))
                errors.extend(c[1] for c in cells)
            valid = (
                rows[0].startswith("F")
                and rows[1].startswith("T")
                and rows[0][1:].isdigit()
                and rows[1][1:].isdigit()
                and max(errors) == 0
            )
            d = {
                "decoded_index": index,
                "text": rows,
                "max_cell_bit_errors": max(errors),
                "total_cell_bit_errors": sum(errors),
                "recognized": valid,
            }
            if valid:
                d["sequence"] = int(rows[0][1:])
                d["pts_ms"] = int(rows[1][1:])
                e = expected.get(d["sequence"])
                d["matches_encode_log"] = (
                    e is not None and d["pts_ms"] == e["pts"] // 1000
                )
            frames.append(d)
            if index + 1 == a.snapshot:
                snapshot = frame.to_image()
            last = frame
    if snapshot is None and last is not None:
        snapshot = last.to_image()
    if snapshot is not None:
        snapshot.save(out.with_suffix(".png"))
    good = [f for f in frames if f["recognized"]]
    seqs = [f["sequence"] for f in good]
    pts = [f["pts_ms"] for f in good]
    counts = collections.Counter(seqs)
    actual = set(seqs)
    missing = sorted(set(expected) - actual)
    interior = [x for x in missing if seqs and min(seqs) < x < max(seqs)]
    summary = {
        "stream": a.stream,
        "log": a.log,
        "mode": a.mode,
        "source": a.source,
        "association_evidence": (
            "input_encode_used_trace"
            if a.source == "camera"
            else "direct_harness_source_and_status"
        ),
        "decoded_frames": len(frames),
        "recognized_frames": len(good),
        "successful_encode_records": len(expected),
        "input_encode_used_order_match": (
            association_log_pass if a.source == "camera" else None
        ),
        "direct_harness_status_pass": (
            association_log_pass if a.source == "synthetic" else None
        ),
        "audit_record_counts": {k: len(v) for k, v in audit.items()},
        "duplicate_encode_sequence_records": duplicate_encode_sequences,
        "nonzero_encode_results": [r for r in records if r["rc"] != 0],
        "sequence_unique": len(actual),
        "repeated_sequences": {str(k): v for k, v in counts.items() if v > 1},
        "sequence_discontinuities": [
            {
                "decoded_index": good[i]["decoded_index"],
                "previous": seqs[i - 1],
                "current": seqs[i],
            }
            for i in range(1, len(seqs))
            if seqs[i] != seqs[i - 1] + 1
        ],
        "repeated_adjacent_timestamps": sum(
            pts[i] == pts[i - 1] for i in range(1, len(pts))
        ),
        "timestamp_log_mismatches": [
            f["decoded_index"] for f in good if not f["matches_encode_log"]
        ],
        "missing_encoded_sequences": missing,
        "missing_interior_sequences": interior,
        "snapshot": str(out.with_suffix(".png")),
        "snapshot_frame": min(a.snapshot, len(frames)),
        "dynamic_association_pass": a.mode == "dynamic"
        and association_log_pass
        and duplicate_encode_sequences == 0
        and all(r["rc"] == 0 for r in records)
        and bool(frames)
        and len(good) == len(frames)
        and len(actual) == len(frames)
        and len(records) > 0
        and not interior
        and (a.source != "synthetic" or not missing)
        and all(f["matches_encode_log"] for f in good)
        and all(seqs[i] == seqs[i - 1] + 1 for i in range(1, len(seqs)))
        and all(pts[i] > pts[i - 1] for i in range(1, len(pts))),
        "static_marker_pass": a.mode == "static"
        and bool(frames)
        and len(good) == len(frames)
        and all(f["sequence"] == 0 and f["pts_ms"] == 0 for f in good),
        "frames": frames,
    }
    summary["all_submitted_frames_present"] = not missing and len(frames) == len(
        records
    )
    summary["complete_dynamic_stream_pass"] = (
        summary["dynamic_association_pass"] and summary["all_submitted_frames_present"]
    )
    out.write_text(json.dumps(summary, indent=2))
    print(
        json.dumps(
            {
                k: (len(v) if isinstance(v, list) else v)
                for k, v in summary.items()
                if k not in ("frames",)
            },
            indent=2,
        )
    )
    passed = (
        summary["complete_dynamic_stream_pass"]
        if a.mode == "dynamic"
        else (
            summary["static_marker_pass"]
            if a.mode == "static"
            else bool(frames) and not good
        )
    )
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
