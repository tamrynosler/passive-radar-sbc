#!/usr/bin/env python3
"""Replay a .pbrcap capture over UDP, paced at the real sample rate.

Stands in for fers-cli when it is not to hand. The capture holds the exact
datagrams FERS sent, so the receiving end cannot tell the difference -- which
makes this a real test of the live socket path, not a simulation of it.

Pacing matters: sending as fast as possible would overrun the receiver's
socket buffer and prove nothing about real-time behaviour. Datagrams go out in
the same pattern they arrived, timed from the sample rate.

    python3 scripts/replay_udp.py --host <jetson-ip> --port 4991
    python3 scripts/replay_udp.py --host 1.2.3.4 --repeat 10   # longer run

Layout (src/ingest/CaptureFile.cpp): 16-byte header "PBRCAP01" + version +
reserved, then per record a little-endian uint32 length followed by that many
bytes.
"""

import argparse
import socket
import struct
import sys
import time


def records(path):
    with open(path, "rb") as f:
        header = f.read(16)
        if len(header) != 16 or header[:8] != b"PBRCAP01":
            sys.exit(f"{path} is not a .pbrcap capture")
        while True:
            raw = f.read(4)
            if len(raw) < 4:
                return
            (length,) = struct.unpack("<I", raw)
            payload = f.read(length)
            if len(payload) < length:
                return
            yield payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="data/fers_capture.pbrcap")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=4991)
    ap.add_argument("--rate", type=float, default=204800.0,
                    help="sample rate, for pacing")
    ap.add_argument("--samples-per-datagram", type=int, default=342)
    ap.add_argument("--repeat", type=int, default=1,
                    help="loop the capture this many times, for a longer run")
    args = ap.parse_args()

    data = list(records(args.file))
    if not data:
        sys.exit("no datagrams in the capture")

    # Two channels interleaved, so the wall-clock gap between consecutive
    # datagrams is half a datagram's worth of samples.
    gap = args.samples_per_datagram / args.rate / 2.0
    total = len(data) * args.repeat
    print(f"{len(data)} datagrams x {args.repeat} = {total}, "
          f"{gap*1e6:.1f} us apart -> {total*gap:.1f} s")
    print(f"sending to {args.host}:{args.port}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    start = time.perf_counter()
    sent = 0
    try:
        for _ in range(args.repeat):
            for payload in data:
                sock.sendto(payload, (args.host, args.port))
                sent += 1
                # Absolute schedule, so small per-send delays do not accumulate
                # into a drifting rate over a long run.
                target = start + sent * gap
                slack = target - time.perf_counter()
                if slack > 0:
                    time.sleep(slack)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        sock.close()

    elapsed = time.perf_counter() - start
    print(f"sent {sent} datagrams in {elapsed:.2f} s "
          f"({sent/elapsed:.0f}/s, expected {1.0/gap:.0f}/s)")


if __name__ == "__main__":
    main()
