#!/usr/bin/env python3
"""Compare encoder retrieval byte counts against independent H264 demux packets."""

import argparse
import importlib.util
import json
from pathlib import Path
import av

p = argparse.ArgumentParser()
p.add_argument("stream")
p.add_argument("log")
p.add_argument("--output", required=True)
a = p.parse_args()
sp = importlib.util.spec_from_file_location(
    "review", str(Path(__file__).resolve().with_name("review_camera.py"))
)
m = importlib.util.module_from_spec(sp)
sp.loader.exec_module(m)
records = m.records(Path(a.log).read_text(errors="replace"), "CAM_OUTPUT")
sizes = [int(r["bytes"]) for r in records]
packets = []
with av.open(a.stream) as c:
    for packet in c.demux(video=0):
        if packet.size:
            packets.append(packet.size)
header = packets[0] - sizes[0] if sizes and packets else None
mismatch = [
    {"index": i, "retrieved_bytes": s, "file_packet_bytes": p}
    for i, (s, p) in enumerate(zip(sizes, packets))
    if p != s + (header if i == 0 else 0)
]
r = {
    "stream": a.stream,
    "retrieved_packets": len(sizes),
    "file_demux_packets": len(packets),
    "file_bytes": Path(a.stream).stat().st_size,
    "retrieved_bytes": sum(sizes),
    "first_packet_header_extra_bytes": header,
    "paired_packet_size_mismatches": mismatch,
    "missing_tail_retrieval_records": records[len(packets) :],
    "file_equals_header_plus_retrieved_prefix": not mismatch
    and Path(a.stream).stat().st_size == sum(sizes[: len(packets)]) + (header or 0),
    "complete_packet_count": len(packets) == len(sizes),
    "limits": (
        "Sizes alone are not byte-content identity; arbitrary "
        "split/merged packets can invalidate one-packet-per-retrieval "
        "comparison. Full glyph decode is separately required."
    ),
}
Path(a.output).write_text(json.dumps(r, indent=2) + "\n")
print(json.dumps(r, indent=2))
