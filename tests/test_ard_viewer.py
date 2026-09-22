#!/usr/bin/env python3
"""Unit test for scripts/ard_viewer.py. Mac only, run by `make host-test`.

Plain main() and asserts, like the C++ tests. The fixture is data/, which is
gitignored, so a fresh clone has nothing to test against: that prints a skip
and exits 0 rather than failing.

The interesting case is completeness. writeArdFile is fopen/fwrite/fclose, so
a frame can be seen half written; the server must skip a short file and serve
the previous one instead of sending a truncated map to the browser.
"""

import json
import os
import shutil
import sys
import tempfile
import threading
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import ard_viewer

DATA = os.path.join(ROOT, "data")
ARD = os.path.join(DATA, "ard")


def test_header():
    path = os.path.join(ARD, "ard_00000.bin")
    header = ard_viewer.read_header(path)
    assert header is not None, "header did not parse"
    assert header["rangeBins"] == 120, header["rangeBins"]
    assert header["dopplerBins"] == 1600, header["dopplerBins"]
    assert header["sampleRateHz"] == 204800, header["sampleRateHz"]
    assert header["dopplerDecimation"] == 128, header["dopplerDecimation"]
    assert header["expectedSize"] == os.path.getsize(path), "size mismatch"
    assert ard_viewer.is_complete(path)

    assert ard_viewer.read_header(os.path.join(DATA, "telemetry.csv")) is None, \
        "a non-ARD file parsed as a header"
    print("  header      R=%d D=%d fs=%d dec=%d %d B" %
          (header["rangeBins"], header["dopplerBins"], header["sampleRateHz"],
           header["dopplerDecimation"], header["expectedSize"]))


def test_completeness(tmp):
    run = os.path.join(tmp, "run_truncated")
    os.makedirs(os.path.join(run, "ard"))
    shutil.copy(os.path.join(ARD, "ard_00000.bin"),
                ard_viewer.frame_path(run, 0))
    shutil.copy(os.path.join(ARD, "ard_00001.bin"),
                ard_viewer.frame_path(run, 1))

    short = ard_viewer.frame_path(run, 1)
    with open(short, "r+b") as f:
        f.truncate(os.path.getsize(short) - 1000)

    assert ard_viewer.list_cpis(run) == [0, 1]
    assert not ard_viewer.is_complete(short)
    assert ard_viewer.newest_complete_cpi(run) == 0, "did not skip the short file"

    os.remove(ard_viewer.frame_path(run, 0))
    assert ard_viewer.newest_complete_cpi(run) is None, \
        "a short file alone must not count"
    print("  complete    short frame skipped, then None")


def test_run_selection(tmp):
    root = os.path.join(tmp, "results")
    with_ard = os.path.join(root, "live_old")
    without = os.path.join(root, "notes")
    newer = os.path.join(root, "live_new")
    os.makedirs(os.path.join(with_ard, "ard"))
    os.makedirs(without)

    assert ard_viewer.pick_run_dir(root) == with_ard, "the only run must win"

    os.makedirs(os.path.join(newer, "ard"))
    os.utime(with_ard, (1000, 1000))
    assert ard_viewer.pick_run_dir(root) == newer, "the newer run must win"

    assert ard_viewer.pick_run_dir(os.path.join(tmp, "nothing-here")) is None
    print("  runselect   newest directory holding ard/ wins")


def test_telemetry():
    row = ard_viewer.read_telemetry(os.path.join(DATA, "telemetry.csv"))
    assert row is not None, "telemetry did not parse"

    with open(os.path.join(DATA, "telemetry.csv"), "r") as f:
        keys = f.readline().strip().split(",")
        last = f.readlines()[-1].strip().split(",")
    assert sorted(row.keys()) == sorted(keys), "keys do not match the header"
    for key in ("lagS", "dsiMs", "totalMs"):
        assert isinstance(row[key], float), "%s is not a float" % key
    assert row["cpi"] == float(last[0]), "not the last row"
    print("  telemetry   cpi=%g lagS=%g totalMs=%g" %
          (row["cpi"], row["lagS"], row["totalMs"]))


def test_config(tmp):
    path = os.path.join(tmp, "prb.conf")
    with open(path, "w") as f:
        f.write("# a comment = not a key\n"
                "\n"
                "  DSI_ALGORITHM  =  ECA-S   # trailing comment\n"
                "DSI_BACKEND=GPU\n"
                "ARD_ALGORITHM = Direct\n"
                "CFAR_PFA = 1e-5\n"
                "CPI_LENGTH_s = 1.0\n"
                "SAMPLE_RATE_Hz = 204800\n"
                "no equals sign here\n")

    cfg = ard_viewer.read_config(path)
    assert cfg == {"DSI_ALGORITHM": "ECA-S", "DSI_BACKEND": "GPU",
                   "ARD_ALGORITHM": "Direct", "CFAR_PFA": "1e-5",
                   "CPI_LENGTH_s": "1.0"}, cfg
    assert ard_viewer.read_config(os.path.join(tmp, "absent.conf")) is None
    print("  config      5 keys, comments and whitespace stripped")


def get(base, path):
    """Returns (status, headers, body). A 404 comes back as a status, not a
    raised exception, so the tests can assert on it."""
    try:
        response = urllib.request.urlopen(base + path, timeout=10)
        return response.getcode(), response.info(), response.read()
    except urllib.error.HTTPError as err:
        return err.code, err.headers, err.read()


def test_server(tmp):
    run = os.path.join(tmp, "run_served")
    os.makedirs(run)
    os.symlink(ARD, os.path.join(run, "ard"))
    shutil.copy(os.path.join(DATA, "telemetry.csv"),
                os.path.join(run, "telemetry.csv"))

    server = ard_viewer.ViewerServer(("127.0.0.1", 0), tmp, run)
    thread = threading.Thread(target=server.serve_forever)
    thread.daemon = True
    thread.start()
    base = "http://127.0.0.1:%d" % server.server_address[1]

    try:
        code, headers, body = get(base, "/")
        assert code == 200, code
        assert headers.get("Content-Type").startswith("text/html"), headers
        assert b"<canvas" in body, "the page is not ard_viewer.html"

        code, _, body = get(base, "/api/status")
        assert code == 200, code
        status = json.loads(body.decode("utf-8"))
        latest = max(ard_viewer.list_cpis(run))
        assert status["latestCpi"] == latest, status["latestCpi"]
        assert status["frameCount"] == len(ard_viewer.list_cpis(run))
        assert status["runName"] == "run_served", status["runName"]
        assert status["telemetry"]["cpi"] == latest, status["telemetry"]
        assert status["config"] is None, "there is no prb.conf in the run dir"

        with open(ard_viewer.frame_path(run, latest), "rb") as f:
            expected = f.read()
        code, headers, body = get(base, "/api/frame/latest")
        assert code == 200, code
        assert headers.get("X-Pbr-Cpi") == str(latest), headers.get("X-Pbr-Cpi")
        assert body == expected, "latest frame bytes differ"

        with open(ard_viewer.frame_path(run, 1), "rb") as f:
            expected = f.read()
        code, _, body = get(base, "/api/frame/1")
        assert code == 200, code
        assert body == expected, "frame 1 bytes differ"

        for path in ("/api/frame/999", "/api/frame/abc", "/api/frame/-1",
                     "/etc/passwd", "/api/nope", "/../Makefile"):
            code, _, _ = get(base, path)
            assert code == 404, "%s returned %d" % (path, code)

        print("  server      /, status, frames and six 404s")
    finally:
        server.shutdown()
        server.server_close()


def main():
    if not os.path.isdir(ARD):
        print("  skip    test_ard_viewer (no %s)" % ARD)
        return 0

    tmp = tempfile.mkdtemp(prefix="ard_viewer_test_")
    try:
        test_header()
        test_completeness(tmp)
        test_run_selection(tmp)
        test_telemetry()
        test_config(tmp)
        test_server(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("  test_ard_viewer: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
