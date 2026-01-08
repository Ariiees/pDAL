#!/usr/bin/env python3

import argparse
import socket
import struct
import sys
from pathlib import Path
from typing import Iterable, Optional

# Import your existing API.
# If retrieve_api.py is not on PYTHONPATH, adjust sys.path here.
# Typical repo layout per your screenshot:
#   src/avs/src/retrieve/retrieve_api.py
_THIS = Path(__file__).resolve()
_REPO_SRC = _THIS.parents[2]
sys.path.insert(0, str(_REPO_SRC))
_DEFAULT_API_DIR = _THIS.parents[2] / "avs" / "src" / "retrieve"  # bring_up/src/ -> src/avs/src/retrieve
if _DEFAULT_API_DIR.exists():
    sys.path.insert(0, str(_DEFAULT_API_DIR))

try:
    from retrieve_api import RetrieveAPI  # type: ignore
except Exception as e:
    raise RuntimeError(
        "Failed to import RetrieveAPI. Ensure retrieve_api.py is discoverable. "
        f"Tried adding: {_DEFAULT_API_DIR}"
    ) from e

from avs.src.retrieve.retrieve_api import RetrieveAPI

MAGIC = b"AVSR"
VERSION = 1

# Keep packet well below MTU.
DEFAULT_PKT_MAX = 1200

# Header layout (little endian):
# magic(4s) version(u16) flags(u16) msg_id(u64) topic_hash(u64) ts_ns(u64)
# frag_index(u32) frag_count(u32) topic_len(u32) locator_len(u32) frag_len(u32)
_HDR_FMT = "<4sHHQQQIIIII"
_HDR_SZ = struct.calcsize(_HDR_FMT)


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def build_packets(
    rec,
    msg_id: int,
    pkt_max: int,
) -> Iterable[bytes]:
    """
    Packet format:
      header | topic_utf8 | locator_utf8 | payload_fragment
    """
    topic_b = rec.sensor_topic.encode("utf-8", errors="strict")
    locator_b = rec.locator.encode("utf-8", errors="strict")

    overhead = _HDR_SZ + len(topic_b) + len(locator_b)
    if overhead >= pkt_max:
        raise ValueError(
            f"pkt_max too small: overhead={overhead} >= pkt_max={pkt_max}. "
            "Increase --pkt-max or shorten topic/locator."
        )

    max_frag = pkt_max - overhead
    payload = rec.payload
    if not payload:
        frag_count = 1
        frag_datas = [b""]
    else:
        frag_count = (len(payload) + max_frag - 1) // max_frag
        frag_datas = [
            payload[i * max_frag : (i + 1) * max_frag] for i in range(frag_count)
        ]

    topic_hash = fnv1a64(topic_b)
    for frag_index, frag in enumerate(frag_datas):
        hdr = struct.pack(
            _HDR_FMT,
            MAGIC,
            VERSION,
            0,
            msg_id & 0xFFFFFFFFFFFFFFFF,
            topic_hash,
            int(rec.ts_ns) & 0xFFFFFFFFFFFFFFFF,
            int(frag_index),
            int(frag_count),
            len(topic_b),
            len(locator_b),
            len(frag),
        )
        yield hdr + topic_b + locator_b + frag


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Retrieve AVS records and send via UDP.")
    p.add_argument(
        "--topic",
        action="append",
        required=True,
        help="Sensor topic to query. Can be provided multiple times.",
    )
    p.add_argument("--start-ns", type=int, required=True, help="Start timestamp (ns).")
    p.add_argument("--end-ns", type=int, required=True, help="End timestamp (ns).")
    p.add_argument(
        "--max-frames",
        type=int,
        default=0,
        help="Max records to send per topic (0 means no limit).",
    )
    p.add_argument("--ip", default="128.175.213.231", help="Destination IP.")
    p.add_argument("--port", type=int, default=9000, help="Destination UDP port.")
    p.add_argument(
        "--pkt-max",
        type=int,
        default=DEFAULT_PKT_MAX,
        help=f"Max UDP packet size in bytes. Default {DEFAULT_PKT_MAX}.",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()

    if args.end_ns < args.start_ns:
        raise ValueError("end_ns must be >= start_ns")

    api = RetrieveAPI()

    addr = (args.ip, int(args.port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Optional: increase send buffer to reduce drops under high throughput.
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    except OSError:
        pass

    msg_id = 1
    sent_pkts = 0
    sent_records = 0
    sent_bytes = 0

    for topic in args.topic:
        remaining: Optional[int] = args.max_frames if args.max_frames > 0 else None

        for rec in api.query(topic, int(args.start_ns), int(args.end_ns), max_frames=0):
            # Enforce per topic limit here to avoid relying on API max_frames semantics
            # across multi trip overlap.
            if remaining is not None and remaining <= 0:
                break

            for pkt in build_packets(rec, msg_id=msg_id, pkt_max=int(args.pkt_max)):
                sock.sendto(pkt, addr)
                sent_pkts += 1
                sent_bytes += len(pkt)

            sent_records += 1
            msg_id += 1
            if remaining is not None:
                remaining -= 1

    sock.close()

    # Print a minimal summary for operators.
    sys.stdout.write(
        f"sent_records={sent_records}\n"
        f"sent_packets={sent_pkts}\n"
        f"sent_bytes={sent_bytes}\n"
        f"dest={addr[0]}:{addr[1]}\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
