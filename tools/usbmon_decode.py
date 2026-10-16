#!/usr/bin/env python3
"""Decode a Linux usbmon pcap (LINKTYPE_USB_LINUX_MMAPPED, 220) into a readable transcript.

Control transfers print their setup packet and payload; runs of bulk transfers on the same
endpoint collapse into one line with the total size and the first bytes of the first packet.
Usage: usbmon_decode.py capture.pcap [--dev N] [--all-bulk]
"""
import struct
import sys

STD_REQ = {0: "GET_STATUS", 1: "CLEAR_FEATURE", 3: "SET_FEATURE", 5: "SET_ADDRESS", 6: "GET_DESCRIPTOR",
           7: "SET_DESCRIPTOR", 8: "GET_CONFIGURATION", 9: "SET_CONFIGURATION", 10: "GET_INTERFACE",
           11: "SET_INTERFACE"}
XFER = {0: "ISO", 1: "INTR", 2: "CTRL", 3: "BULK"}


def hexdump(data, limit=64):
    shown = data[:limit]
    hx = shown.hex(" ")
    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in shown)
    more = f" (+{len(data) - limit} more)" if len(data) > limit else ""
    return f"{hx}  |{asc}|{more}"


def packets(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        magic = struct.unpack("<I", hdr[:4])[0]
        endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        linktype = struct.unpack(endian + "I", hdr[20:24])[0]
        if linktype not in (189, 220):
            sys.exit(f"unexpected linktype {linktype}")
        hlen = 64 if linktype == 220 else 48
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            _, _, incl, _ = struct.unpack(endian + "IIII", rec)
            body = f.read(incl)
            (urb_id, typ, xfer, ep, dev, bus, flag_setup, flag_data, ts_sec, ts_usec, status, length,
             len_cap) = struct.unpack("<QcBBBHbbqiiII", body[:40])
            setup = body[40:48]
            yield dict(id=urb_id, type=typ.decode(), xfer=xfer, ep=ep, dev=dev, bus=bus,
                       has_setup=(flag_setup == 0), ts=ts_sec + ts_usec / 1e6, status=status,
                       length=length, data=body[hlen:hlen + len_cap], setup=setup)


def describe_setup(s):
    bm, req, wval, widx, wlen = struct.unpack("<BBHHH", s)
    direction = "IN " if bm & 0x80 else "OUT"
    kind = ("std", "class", "vendor", "rsvd")[(bm >> 5) & 3]
    recip = ("dev", "if", "ep", "other")[bm & 3] if (bm & 3) < 4 else str(bm & 31)
    name = STD_REQ.get(req, f"req=0x{req:02x}") if kind == "std" else f"req=0x{req:02x}"
    return (f"{direction} {kind}/{recip} bmRT=0x{bm:02x} {name} wValue=0x{wval:04x} wIndex=0x{widx:04x} "
            f"wLength={wlen}")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    path = args[0]
    only_dev = int(args[args.index("--dev") + 1]) if "--dev" in args else None
    all_bulk = "--all-bulk" in args

    pending = {}
    t0 = None
    bulk_run = None  # [ep, dev, count, bytes, first_data, t_start, t_end, errors]

    def flush_bulk():
        nonlocal bulk_run
        if bulk_run:
            ep, dev, count, total, first, ts, te, errs = bulk_run
            d = "IN " if ep & 0x80 else "OUT"
            err = f" errors={errs}" if errs else ""
            print(f"{ts:9.3f}  dev{dev} BULK {d} ep{ep & 0x7f:x}: {count} xfers, {total} bytes "
                  f"over {te - ts:.3f}s{err}")
            if first:
                print(f"           first: {hexdump(first, 48)}")
            bulk_run = None

    for p in packets(path):
        if only_dev is not None and p["dev"] != only_dev:
            continue
        if t0 is None:
            t0 = p["ts"]
        t = p["ts"] - t0
        if p["type"] == "S":
            pending[p["id"]] = p
            continue
        sub = pending.pop(p["id"], None)
        if sub is None:
            continue
        xfer = p["xfer"]
        status = p["status"]
        if xfer == 3 or (xfer == 1 and not all_bulk and False):
            payload = sub["data"] if not (p["ep"] & 0x80) else p["data"]
            size = sub["length"] if not (p["ep"] & 0x80) else p["length"]
            if all_bulk:
                flush_bulk()
                d = "IN " if p["ep"] & 0x80 else "OUT"
                print(f"{t:9.3f}  dev{p['dev']} BULK {d} ep{p['ep'] & 0x7f:x} len={size} st={status} "
                      f"{hexdump(payload, 32)}")
                continue
            if bulk_run and (bulk_run[0] != p["ep"] or bulk_run[1] != p["dev"]):
                flush_bulk()
            if not bulk_run:
                bulk_run = [p["ep"], p["dev"], 0, 0, payload, t, t, 0]
            bulk_run[2] += 1
            bulk_run[3] += size
            bulk_run[6] = t
            if status:
                bulk_run[7] += 1
            continue
        flush_bulk()
        if xfer == 2:
            line = describe_setup(sub["setup"]) if sub["has_setup"] else "(no setup)"
            data = p["data"] if (sub["setup"][0] & 0x80) else sub["data"]
            st = f" status={status}" if status else ""
            print(f"{t:9.3f}  dev{p['dev']} CTRL {line}{st}")
            if data:
                print(f"           data: {hexdump(data)}")
        elif xfer == 1:
            st = f" status={status}" if status else ""
            print(f"{t:9.3f}  dev{p['dev']} INTR ep{p['ep'] & 0x7f:x} {'IN' if p['ep'] & 0x80 else 'OUT'}"
                  f" len={p['length']}{st}  {hexdump(p['data'], 32)}")
    flush_bulk()


if __name__ == "__main__":
    main()
