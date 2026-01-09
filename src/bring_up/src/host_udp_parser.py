#!/usr/bin/env python3
"""
Host side UDP parser and reassembler for AVS record packets.

It mirrors the sender format:

  header | topic_utf8 | locator_utf8 | payload_fragment

Header (little endian):
  magic(4s) version(u16) flags(u16) msg_id(u64) topic_hash(u64) ts_ns(u64)
  frag_index(u32) frag_count(u32) topic_len(u32) locator_len(u32) frag_len(u32)

This receiver:
  1. Listens on UDP
  2. Parses and validates headers
  3. Reassembles fragments per msg_id
  4. Emits each complete encrypted record to a dummy consumer, one by one

Notes:
  - UDP may drop or reorder packets. This code tolerates reordering; drops cause timeouts.
  - It uses a per message TTL and a bounded in memory assembly table to avoid leaks.
"""

import argparse
import socket
import struct
import time
from dataclasses import dataclass
from typing import Dict, Optional


MAGIC = b"AVSR"
VERSION = 1

# Header layout (little endian), must match sender.
_HDR_FMT = "<4sHHQQQIIIII"
_HDR_SZ = struct.calcsize(_HDR_FMT)


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


@dataclass
class CompleteRecord:
    msg_id: int
    ts_ns: int
    topic: str
    locator: str
    encrypted_payload: bytes


class DummyConsumer:
    """
    Replace this with your real pipeline.
    The contract is: consume() is called once per fully reassembled record, sequentially.
    """

    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self.count = 0
        self.total_bytes = 0

    def consume(self, rec: CompleteRecord) -> None:
        self.count += 1
        self.total_bytes += len(rec.encrypted_payload)
        if self.verbose:
            print(
                f"CONSUME msg_id={rec.msg_id} ts_ns={rec.ts_ns} "
                f"topic={rec.topic} locator={rec.locator} "
                f"encrypted_bytes={len(rec.encrypted_payload)}"
            )


class ReassemblyError(Exception):
    pass


class Reassembler:
    """
    Reassembles fragmented UDP payloads into complete records.

    Keyed by msg_id.
    Validates:
      - MAGIC / VERSION
      - topic hash (FNV1a64 over topic UTF8)
      - consistent topic/locator across fragments of the same msg_id
      - frag_index in range
    """

    def __init__(
        self,
        ttl_s: float = 2.0,
        max_inflight: int = 4096,
        max_record_bytes: int = 64 * 1024 * 1024,
        drop_on_hash_mismatch: bool = True,
    ):
        self.ttl_s = float(ttl_s)
        self.max_inflight = int(max_inflight)
        self.max_record_bytes = int(max_record_bytes)
        self.drop_on_hash_mismatch = bool(drop_on_hash_mismatch)

        # msg_id -> state
        self._inflight: Dict[int, dict] = {}

        # Stats
        self.pkts_ok = 0
        self.pkts_bad = 0
        self.records_completed = 0
        self.records_dropped_timeout = 0
        self.records_dropped_overflow = 0
        self.records_dropped_mismatch = 0

    def _evict_if_needed(self) -> None:
        if len(self._inflight) <= self.max_inflight:
            return
        # Evict oldest by last_update
        items = sorted(self._inflight.items(), key=lambda kv: kv[1]["last_update"])
        # Evict enough to be within limit.
        to_evict = len(self._inflight) - self.max_inflight
        for i in range(to_evict):
            _, st = items[i]
            st["dropped"] = True
            self.records_dropped_overflow += 1
            del self._inflight[items[i][0]]

    def purge_timeouts(self) -> None:
        now = time.monotonic()
        expired = [mid for mid, st in self._inflight.items() if now - st["last_update"] > self.ttl_s]
        for mid in expired:
            del self._inflight[mid]
            self.records_dropped_timeout += 1

    def feed(self, pkt: bytes) -> Optional[CompleteRecord]:
        """
        Returns a CompleteRecord if this packet completes a message, else None.
        """
        if len(pkt) < _HDR_SZ:
            self.pkts_bad += 1
            return None

        try:
            (
                magic,
                ver,
                flags,
                msg_id,
                topic_hash,
                ts_ns,
                frag_index,
                frag_count,
                topic_len,
                locator_len,
                frag_len,
            ) = struct.unpack(_HDR_FMT, pkt[:_HDR_SZ])
        except struct.error:
            self.pkts_bad += 1
            return None

        if magic != MAGIC or ver != VERSION:
            self.pkts_bad += 1
            return None

        # Bounds
        if frag_count <= 0 or frag_index >= frag_count:
            self.pkts_bad += 1
            return None

        # Compute expected total packet length
        need = _HDR_SZ + topic_len + locator_len + frag_len
        if len(pkt) != need:
            # Be strict: sender always sends exact sized packet.
            self.pkts_bad += 1
            return None

        # Decode variable fields
        off = _HDR_SZ
        topic_b = pkt[off : off + topic_len]
        off += topic_len
        locator_b = pkt[off : off + locator_len]
        off += locator_len
        frag_b = pkt[off : off + frag_len]

        try:
            topic = topic_b.decode("utf-8", errors="strict")
            locator = locator_b.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            self.pkts_bad += 1
            return None

        # Topic hash validation (optional strictness)
        calc_hash = fnv1a64(topic_b)
        if calc_hash != topic_hash:
            self.pkts_bad += 1
            if self.drop_on_hash_mismatch:
                self.records_dropped_mismatch += 1
                # Drop whole message state if present.
                self._inflight.pop(int(msg_id), None)
                return None

        self.pkts_ok += 1

        # Create state if first time seeing this msg_id
        st = self._inflight.get(int(msg_id))
        now = time.monotonic()
        if st is None:
            self._evict_if_needed()
            st = {
                "msg_id": int(msg_id),
                "ts_ns": int(ts_ns),
                "topic": topic,
                "locator": locator,
                "frag_count": int(frag_count),
                "frags": {},  # index -> bytes
                "bytes_accum": 0,
                "last_update": now,
            }
            self._inflight[int(msg_id)] = st
        else:
            st["last_update"] = now
            # Consistency checks across fragments
            if (
                st["frag_count"] != int(frag_count)
                or st["topic"] != topic
                or st["locator"] != locator
                or st["ts_ns"] != int(ts_ns)
            ):
                # Mismatch: drop state.
                self.records_dropped_mismatch += 1
                del self._inflight[int(msg_id)]
                return None

        # Deduplicate fragment if resend
        if int(frag_index) in st["frags"]:
            return None

        # Record size guardrail
        st["bytes_accum"] += len(frag_b)
        if st["bytes_accum"] > self.max_record_bytes:
            self.records_dropped_overflow += 1
            del self._inflight[int(msg_id)]
            return None

        st["frags"][int(frag_index)] = frag_b

        # Completed?
        if len(st["frags"]) == st["frag_count"]:
            # Assemble in order
            payload = b"".join(st["frags"][i] for i in range(st["frag_count"]))
            rec = CompleteRecord(
                msg_id=st["msg_id"],
                ts_ns=st["ts_ns"],
                topic=st["topic"],
                locator=st["locator"],
                encrypted_payload=payload,
            )
            del self._inflight[int(msg_id)]
            self.records_completed += 1
            return rec

        return None


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Host side UDP record parser and reassembler.")
    p.add_argument("--bind-ip", default="0.0.0.0", help="IP to bind to.")
    p.add_argument("--port", type=int, default=9000, help="UDP port to listen on.")
    p.add_argument("--rcvbuf-mb", type=int, default=8, help="SO_RCVBUF in MB (best effort).")
    p.add_argument("--ttl-s", type=float, default=2.0, help="Fragment reassembly TTL in seconds.")
    p.add_argument("--max-inflight", type=int, default=4096, help="Max inflight messages tracked.")
    p.add_argument(
        "--max-record-mb",
        type=int,
        default=64,
        help="Max assembled record size in MB; larger records get dropped.",
    )
    p.add_argument("--verbose", action="store_true", help="Print each consumed record.")
    p.add_argument("--stats-every-s", type=float, default=2.0, help="Print stats every N seconds.")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind_ip, int(args.port)))

    # Increase recv buffer (best effort)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, int(args.rcvbuf_mb) * 1024 * 1024)
    except OSError:
        pass

    # Non blocking receive loop
    sock.settimeout(0.2)

    consumer = DummyConsumer(verbose=bool(args.verbose))
    reasm = Reassembler(
        ttl_s=float(args.ttl_s),
        max_inflight=int(args.max_inflight),
        max_record_bytes=int(args.max_record_mb) * 1024 * 1024,
    )

    last_stats = time.monotonic()
    last_purge = time.monotonic()

    print(f"listening_udp={args.bind_ip}:{args.port}")

    try:
        while True:
            try:
                pkt, src = sock.recvfrom(65535)
            except socket.timeout:
                pkt = b""
                src = None

            now = time.monotonic()

            # Periodic maintenance
            if now - last_purge > 0.5:
                reasm.purge_timeouts()
                last_purge = now

            if pkt:
                rec = reasm.feed(pkt)
                if rec is not None:
                    # Send encrypted record one by one to dummy consumer.
                    consumer.consume(rec)

            if now - last_stats >= float(args.stats_every_s):
                print(
                    "STATS "
                    f"pkts_ok={reasm.pkts_ok} pkts_bad={reasm.pkts_bad} "
                    f"completed={reasm.records_completed} "
                    f"dropped_timeout={reasm.records_dropped_timeout} "
                    f"dropped_overflow={reasm.records_dropped_overflow} "
                    f"dropped_mismatch={reasm.records_dropped_mismatch} "
                    f"inflight={len(reasm._inflight)} "
                    f"consumed={consumer.count} consumed_bytes={consumer.total_bytes}"
                )
                last_stats = now

    except KeyboardInterrupt:
        print("exit=keyboard_interrupt")
    finally:
        sock.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
