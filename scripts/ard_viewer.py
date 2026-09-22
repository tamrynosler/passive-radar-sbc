#!/usr/bin/env python3
"""Serve a pbr_run output directory to a browser as a live range-Doppler view.

A sidecar: pbr_run never knows this exists, so it cannot add latency to a CPI
or crash the chain. The server does not touch the map -- it validates a frame's
size against its own header and sends the bytes. dB conversion, colour mapping
and axis drawing all happen in scripts/ard_viewer.html, where the cycles are
free.

    python3 scripts/ard_viewer.py --root /data/results     # newest run, live
    python3 scripts/ard_viewer.py --dir data --port 8765    # pinned, testing

Python 3.6 on the board (L4T Ubuntu 18.04), standard library only. Nothing
newer than 3.6 may appear here: it would be a runtime error there and invisible
on the Mac.

Layout (include/pbr/ArdFile.hpp): 64-byte header, then 8*R*D bytes of
interleaved float32 I/Q, then 48 bytes per detection.
"""

import argparse
import http.server
import json
import os
import re
import shutil
import socketserver
import struct
import sys
import threading
import time

MAGIC = b"PBRARD01"
HEADER_SIZE = 64
DETECTION_SIZE = 48

# Sanity bounds on a corrupt header, matching src/ard/ArdFile.cpp. Without them
# a garbage header yields a huge expected size and every frame looks incomplete.
MAX_CELLS = 1 << 28
MAX_DETECTIONS = 1 << 24

# Enough of the tail of telemetry.csv to hold the last row several times over.
TAIL_BYTES = 4096

# The only config keys the page shows.
CONFIG_KEYS = ("DSI_ALGORITHM", "DSI_BACKEND", "ARD_ALGORITHM", "CFAR_PFA",
               "CPI_LENGTH_s")

FRAME_RE = re.compile(r"^ard_([0-9]+)\.bin$")
CPI_RE = re.compile(r"^[0-9]{1,9}$")


# ---------------------------------------------------------------------------
# The .bin format
# ---------------------------------------------------------------------------

def parse_header(raw):
    """Decode a 64-byte ARD header. Returns a dict, or None if it is not one.

    'expectedSize' is what a complete file must measure. writeArdFile is
    fopen/fwrite/fclose, so a file can be seen half written; comparing the size
    on disk against this is how a complete frame is told from one still being
    written.
    """
    if len(raw) != HEADER_SIZE or raw[:8] != MAGIC:
        return None

    fields = struct.unpack("<7Q", raw[8:HEADER_SIZE])
    header = {
        "rangeBins": fields[0],
        "dopplerBins": fields[1],
        "sampleRateHz": fields[2],
        "dopplerDecimation": fields[3],
        "cpiStartIndex": fields[4],
        "detections": fields[5],
    }

    cells = header["rangeBins"] * header["dopplerBins"]
    if (header["rangeBins"] == 0 or header["dopplerBins"] == 0 or
            cells > MAX_CELLS or header["detections"] > MAX_DETECTIONS):
        return None

    header["expectedSize"] = (HEADER_SIZE + 8 * cells +
                              DETECTION_SIZE * header["detections"])
    return header


def read_header(path):
    """Header of one .bin, or None if it is missing or not an ARD file."""
    try:
        with open(path, "rb") as f:
            return parse_header(f.read(HEADER_SIZE))
    except OSError:
        return None


def open_complete_frame(path):
    """Open a frame for sending. Returns (file, size), or None if the file is
    absent, malformed, or still being written.

    The size is taken from the open handle, so the file cannot be replaced
    between the check and the send.
    """
    try:
        f = open(path, "rb")
    except OSError:
        return None

    header = parse_header(f.read(HEADER_SIZE))
    if header is None or os.fstat(f.fileno()).st_size != header["expectedSize"]:
        f.close()
        return None

    f.seek(0)
    return f, header["expectedSize"]


def is_complete(path):
    """True if the file on disk is a whole ARD frame."""
    opened = open_complete_frame(path)
    if opened is None:
        return False
    opened[0].close()
    return True


# ---------------------------------------------------------------------------
# The run directory
# ---------------------------------------------------------------------------

def frame_path(run_dir, cpi):
    return os.path.join(run_dir, "ard", "ard_%05d.bin" % cpi)


def list_cpis(run_dir):
    """Sorted CPI indices of every ard_NNNNN.bin present, complete or not."""
    cpis = []
    try:
        names = os.listdir(os.path.join(run_dir, "ard"))
    except OSError:
        return cpis
    for name in names:
        match = FRAME_RE.match(name)
        if match is not None:
            cpis.append(int(match.group(1)))
    cpis.sort()
    return cpis


def newest_complete_cpi(run_dir):
    """Index of the newest complete frame, or None if there is not one yet.

    Walks backwards: the highest index is usually the one still being written,
    so the answer is normally the second file looked at.
    """
    for cpi in reversed(list_cpis(run_dir)):
        if is_complete(frame_path(run_dir, cpi)):
            return cpi
    return None


def pick_run_dir(root):
    """The newest subdirectory of root that holds an ard/ directory.

    Re-read on every request, so starting a new run switches the browser to it
    with no action from the user.
    """
    best = None
    best_mtime = None
    try:
        names = os.listdir(root)
    except OSError:
        return None
    for name in names:
        path = os.path.join(root, name)
        if not os.path.isdir(os.path.join(path, "ard")):
            continue
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        if best_mtime is None or mtime > best_mtime:
            best = path
            best_mtime = mtime
    return best


# ---------------------------------------------------------------------------
# telemetry.csv and prb.conf
# ---------------------------------------------------------------------------

def to_number(text):
    """Numbers as numbers, anything else left as it was written."""
    try:
        return float(text)
    except ValueError:
        return text


def read_telemetry(path):
    """Last row of a telemetry CSV keyed by its header row, or None.

    Read from the tail. The file grows for the whole run, so reading it whole
    would get slower with every CPI, and the poll runs at 2 Hz.
    """
    try:
        with open(path, "rb") as f:
            first = f.readline()
            size = os.fstat(f.fileno()).st_size
            f.seek(max(0, size - TAIL_BYTES))
            tail = f.read()
    except OSError:
        return None

    header_text = first.decode("utf-8", "replace").strip()
    if not header_text:
        return None
    keys = header_text.split(",")

    # The seek can land mid-row, and pbr_run can be part way through writing
    # the last one, so only rows with the full field count are considered.
    rows = []
    for raw in tail.split(b"\n"):
        text = raw.decode("utf-8", "replace").strip()
        if not text or text == header_text:
            continue
        fields = text.split(",")
        if len(fields) == len(keys):
            rows.append(fields)
    if not rows:
        return None

    values = {}
    for index, key in enumerate(keys):
        values[key] = to_number(rows[-1][index])
    return values


def read_config(path):
    """The config values the header strip shows, as strings, or None if the
    file is absent.

    Rules from prb.conf's own header: one KEY = VALUE per line, '#' starts a
    comment to end of line, whitespace stripped from both sides.
    """
    # Explicit encoding: an ssh session on the board has no LANG, so Python
    # falls back to ASCII and one accented byte would raise UnicodeDecodeError
    # -- not an OSError, so it would escape and kill every /api/status.
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return None

    values = {}
    for line in lines:
        text = line.split("#", 1)[0].strip()
        if "=" not in text:
            continue
        key, value = text.split("=", 1)
        key = key.strip()
        if key in CONFIG_KEYS:
            values[key] = value.strip()
    return values


def build_status(run_dir, server_time):
    """The /api/status payload for one run directory."""
    if run_dir is None:
        return {"runDir": None, "runName": None, "frameCount": 0,
                "latestCpi": None, "telemetry": None, "config": None,
                "serverTime": server_time}

    return {
        "runDir": run_dir,
        "runName": os.path.basename(os.path.normpath(run_dir)),
        "frameCount": len(list_cpis(run_dir)),
        "latestCpi": newest_complete_cpi(run_dir),
        "telemetry": read_telemetry(os.path.join(run_dir, "telemetry.csv")),
        "config": read_config(os.path.join(run_dir, "prb.conf")),
        "serverTime": server_time,
    }


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

class ViewerHandler(http.server.BaseHTTPRequestHandler):

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            self.send_page()
        elif path == "/api/status":
            self.send_status()
        elif path == "/api/frame/latest":
            self.send_frame(None)
        elif path.startswith("/api/frame/"):
            tail = path[len("/api/frame/"):]
            if CPI_RE.match(tail) is None:
                self.send_error(404)
            else:
                self.send_frame(int(tail))
        else:
            self.send_error(404)

    def send_page(self):
        page = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "ard_viewer.html")
        try:
            with open(page, "rb") as f:
                body = f.read()
        except OSError:
            self.send_error(500, "ard_viewer.html is missing")
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_status(self):
        status = build_status(self.server.run_dir(), time.time())
        body = json.dumps(status).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_frame(self, cpi):
        """Send one frame verbatim. cpi None means the newest complete one."""
        run_dir = self.server.run_dir()
        if run_dir is None:
            self.send_error(404)
            return

        if cpi is None:
            cpi = newest_complete_cpi(run_dir)
            if cpi is None:
                self.send_error(404)
                return

        opened = open_complete_frame(frame_path(run_dir, cpi))
        if opened is None:
            self.send_error(404)
            return
        handle, size = opened

        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("X-Pbr-Cpi", str(cpi))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            # Chunked copy: the map is never read into this process.
            shutil.copyfileobj(handle, self.wfile)
        except (BrokenPipeError, ConnectionResetError):
            pass          # the browser moved on; nothing to report
        finally:
            handle.close()

    def log_message(self, fmt, *args):
        pass              # one line per request at 2 Hz is noise

    def log_error(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


class ViewerServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """ThreadingMixIn, not ThreadingHTTPServer: that class is 3.7 and the board
    runs 3.6. A client slow to take a 1.5 MB frame must not block the status
    poll."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, root, pinned_dir):
        http.server.HTTPServer.__init__(self, address, ViewerHandler)
        self.root = root
        self.pinned_dir = pinned_dir
        self.lock = threading.Lock()
        self.last_run_dir = None

    def run_dir(self):
        if self.pinned_dir is not None:
            return self.pinned_dir
        found = pick_run_dir(self.root)
        with self.lock:
            changed = found != self.last_run_dir
            self.last_run_dir = found
        if changed:
            sys.stderr.write("run directory: %s\n" % found)
        return found


def main():
    ap = argparse.ArgumentParser(
        description="Serve a pbr_run output directory as a live ARD view.")
    ap.add_argument("--root", default="/data/results",
                    help="watch this directory and show its newest run")
    ap.add_argument("--dir", default=None,
                    help="pin one run directory; overrides --root")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()

    pinned = os.path.abspath(args.dir) if args.dir is not None else None
    if pinned is not None and not os.path.isdir(pinned):
        sys.exit("no such directory: %s" % pinned)

    try:
        server = ViewerServer((args.host, args.port),
                              os.path.abspath(args.root), pinned)
    except OSError as e:
        sys.exit("cannot listen on %s:%d: %s\n"
                 "another ard_viewer is probably still running: "
                 "pkill -f ard_viewer.py" % (args.host, args.port, e.strerror))
    if pinned is not None:
        print("serving %s on http://%s:%d" % (pinned, args.host, args.port))
    else:
        print("watching %s, serving on http://%s:%d"
              % (args.root, args.host, args.port))
    sys.stdout.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
