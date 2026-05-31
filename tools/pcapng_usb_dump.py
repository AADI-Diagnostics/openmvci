#!/usr/bin/env python3
"""Minimal pcapng + USBPcap parser for offline analysis.

Extracts USB bulk OUT/IN payloads from a Windows USBPcap-format capture
and prints a flat, time-ordered timeline along with a per-direction
hex dump suitable for diffing against openmvci's serial trace.

Usage:
  python3 pcapng_usb_dump.py path/to/capture.pcapng [--max N] [--device-filter VID:PID]
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from typing import Iterator, Optional

PCAPNG_SHB = 0x0A0D0D0A
PCAPNG_IDB = 0x00000001
PCAPNG_EPB = 0x00000006
PCAPNG_SPB = 0x00000003

# USBPcap link-layer type (per pcap-linktype registry).
LINKTYPE_USBPCAP = 249

# USBPcap pseudo-header layout (little-endian):
#   headerLen U16, irpId U64, status U32, function U16, info U8,
#   bus U16, device U16, endpoint U8, transfer U8, dataLength U32
USBPCAP_HEADER = "<HQIHBHHBBI"
USBPCAP_HEADER_SIZE = struct.calcsize(USBPCAP_HEADER)

# Transfer type values per USBPcap.
TRANSFER_BULK = 3


@dataclass
class UsbRecord:
    ts_us: int
    bus: int
    device: int
    endpoint: int
    direction: str  # "OUT" or "IN"
    transfer: int
    data: bytes


def _read_block(stream) -> Optional[tuple[int, bytes]]:
    head = stream.read(8)
    if len(head) < 8:
        return None
    block_type, block_total_len = struct.unpack("<II", head)
    if block_total_len < 12:
        raise ValueError(f"Invalid block length {block_total_len}")
    body_len = block_total_len - 12
    body = stream.read(body_len)
    if len(body) != body_len:
        raise ValueError("Short read in block body")
    trailer = stream.read(4)
    if len(trailer) != 4:
        raise ValueError("Short read in block trailer")
    (trailer_len,) = struct.unpack("<I", trailer)
    if trailer_len != block_total_len:
        raise ValueError("Block length mismatch")
    return block_type, body


def iter_usb_records(path: str) -> Iterator[UsbRecord]:
    with open(path, "rb") as f:
        ifaces: list[int] = []  # link types per interface index
        ts_resolution = 1_000_000  # default microseconds
        while True:
            block = _read_block(f)
            if block is None:
                return
            btype, body = block
            if btype == PCAPNG_SHB:
                # Section header. Body: bom(4) major(2) minor(2) sectlen(8) opts
                if len(body) < 16:
                    continue
                bom = struct.unpack("<I", body[:4])[0]
                if bom != 0x1A2B3C4D:
                    raise ValueError("Unsupported byte order in SHB")
                ifaces = []
            elif btype == PCAPNG_IDB:
                # linktype U16, reserved U16, snaplen U32, options
                if len(body) < 8:
                    continue
                linktype, _resv, _snap = struct.unpack("<HHI", body[:8])
                ifaces.append(linktype)
            elif btype == PCAPNG_EPB:
                # iface U32, ts_high U32, ts_low U32, caplen U32, origlen U32, data, opts
                if len(body) < 20:
                    continue
                iface_id, ts_hi, ts_lo, caplen, _orig = struct.unpack("<IIIII", body[:20])
                if iface_id >= len(ifaces) or ifaces[iface_id] != LINKTYPE_USBPCAP:
                    continue
                ts = (ts_hi << 32) | ts_lo
                # padded to 32-bit
                pkt = body[20 : 20 + caplen]
                rec = _decode_usbpcap_packet(ts, pkt)
                if rec is not None:
                    yield rec


def _decode_usbpcap_packet(ts_us: int, pkt: bytes) -> Optional[UsbRecord]:
    if len(pkt) < USBPCAP_HEADER_SIZE:
        return None
    (
        header_len,
        _irp,
        _status,
        _function,
        info,
        bus,
        device,
        endpoint,
        transfer,
        data_len,
    ) = struct.unpack(USBPCAP_HEADER, pkt[:USBPCAP_HEADER_SIZE])
    if header_len > len(pkt):
        return None
    payload = pkt[header_len : header_len + data_len]
    # endpoint top bit = direction (1=IN device->host, 0=OUT host->device)
    direction = "IN" if (endpoint & 0x80) else "OUT"
    return UsbRecord(
        ts_us=ts_us,
        bus=bus,
        device=device,
        endpoint=endpoint & 0x7F,
        direction=direction,
        transfer=transfer,
        data=payload,
    )


def _hex(b: bytes, limit: int = 64) -> str:
    if len(b) <= limit:
        return b.hex(" ")
    return b[:limit].hex(" ") + f" ... ({len(b)} bytes)"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--max", type=int, default=0, help="Stop after N records (0=all)")
    ap.add_argument("--bulk-only", action="store_true", help="Only show bulk transfers")
    ap.add_argument("--min-len", type=int, default=1, help="Skip payloads shorter than this")
    ap.add_argument("--start-us", type=int, default=0, help="Skip records before this timestamp")
    args = ap.parse_args()

    count = 0
    out_total = 0
    in_total = 0
    first_ts: Optional[int] = None
    for rec in iter_usb_records(args.path):
        if args.bulk_only and rec.transfer != TRANSFER_BULK:
            continue
        if len(rec.data) < args.min_len:
            continue
        if first_ts is None:
            first_ts = rec.ts_us
        if rec.ts_us < args.start_us:
            continue
        rel_ms = (rec.ts_us - first_ts) / 1000.0
        tag = "TX" if rec.direction == "OUT" else "RX"
        if rec.direction == "OUT":
            out_total += len(rec.data)
        else:
            in_total += len(rec.data)
        print(
            f"t={rel_ms:9.3f}ms bus={rec.bus} dev={rec.device} "
            f"ep=0x{rec.endpoint:02X} {tag} xfer={rec.transfer} "
            f"len={len(rec.data):4d} {_hex(rec.data)}"
        )
        count += 1
        if args.max and count >= args.max:
            break
    sys.stderr.write(f"\n[summary] records={count} out_bytes={out_total} in_bytes={in_total}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
