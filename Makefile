# passive-radar-sbc — host-side driver (runs on the Mac)
#
# All editing happens here on the Mac; the Jetson is a disposable build/run
# target. NEVER edit files directly on the Jetson — rsync --delete will
# clobber them on the next sync.
#
# ---------------------------------------------------------------------------
# The workflow
# ---------------------------------------------------------------------------
#
#   edited config/prb.conf only .....  make sync    (2 s, no compile)
#   edited any C++ / CUDA ...........  make build   (~2 min)
#   check nothing broke .............  make test
#   live run against FERS ...........  make run
#   replay the recorded capture .....  make replay
#   bring results back to the Mac ...  make pull
#
# Everything about a run — algorithm, backend, CFAR and ECA settings — lives
# in config/prb.conf, which is read at RUN time. So the whole tuning loop is
# `make sync && make run`: no rebuild, no command-line flags to remember.
#
# Requires a `Host jetson` entry in ~/.ssh/config with key-based auth.

JETSON ?= jetson
REMOTE ?= ~/passive-radar-sbc
NVCC   := /usr/local/cuda-10.2/bin/nvcc
CMAKE_ARGS := -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=$(NVCC)

# Exclusions follow .gitignore, so anything git ignores stays off the Jetson.
# 'P' protects a path from --delete: data/, results/ and build/ live only on
# the board and must survive every sync.
#
# macOS 15.7 ships openrsync as /usr/bin/rsync, which dies with "unexpected
# end of file" on --delete plus filter rules against the board's rsync 3.1.2
# (2026-10-02; each option alone works, the combination does not). The real
# rsync from Homebrew works, so it is preferred whenever it is installed.
RSYNC_BIN := $(shell test -x /opt/homebrew/bin/rsync && echo /opt/homebrew/bin/rsync || echo rsync)
RSYNC := $(RSYNC_BIN) -a --delete \
         --filter='P data' --filter='P results' --filter='P build' \
         --filter=':- .gitignore' \
         --exclude '.git' --exclude '.DS_Store'

.PHONY: help build sync test run replay verify bench pull shell clean \
        view drive stop host host-test host-view

help:
	@echo "passive-radar-sbc"
	@echo
	@echo "  make sync       push config + source to the Jetson (no compile)"
	@echo "  make build      sync, then compile on the Jetson"
	@echo "  make test       run the unit tests on the Jetson"
	@echo "  make run        live run, clocks pinned (from the USRP or FERS)"
	@echo "  make drive      the USRP driver on the Jetson: samples -> multicast"
	@echo "  make stop       end every radar process left on the Jetson"
	@echo "  make replay     replay the recorded capture on the Jetson"
	@echo "  make view       watch the newest run in a browser (second terminal)"
	@echo "  make verify     golden-file checks against MATLAB"
	@echo "  make bench      DSI + ARD timing sweeps, clocks pinned"
	@echo "  make pull       copy results back to ./results"
	@echo "  make shell      interactive shell on the Jetson"
	@echo "  make clean      wipe the Jetson build directory"
	@echo
	@echo "  make host       build every tool locally into ./bin (no CUDA)"
	@echo "  make host-test  run the unit tests locally"
	@echo "  make host-view  serve one recorded frame from ./data in the viewer"
	@echo
	@echo "  ARGS=... passes extra arguments to run and bench, e.g."
	@echo "    make run ARGS='--seconds 145'"

# ---------------------------------------------------------------------------
# The Jetson
# ---------------------------------------------------------------------------

## sync: push config and source. Use this after editing config/prb.conf —
##       the config is read at RUN time, so no rebuild is needed.
sync:
	@$(RSYNC) ./ $(JETSON):$(REMOTE)/
	@echo "synced to $(JETSON)"

## build: sync, then compile on the Jetson.
build: sync
	@ssh $(JETSON) 'cd $(REMOTE) && mkdir -p build && cd build && \
	    cmake $(CMAKE_ARGS) .. > /dev/null && make -j2'
	@echo "built on $(JETSON)"

## test: unit tests on the Jetson. Does not build — run `make build` first if
##       the source changed.
test:
	@ssh $(JETSON) 'cd $(REMOTE)/build && ctest --output-on-failure'

## run: live run against FERS, clocks pinned and tegrastats logged. Needs an
##      interactive sudo password, hence ssh -t. Start fers-cli on the other
##      machine when it prints "Listening now".
run:
	@ssh -t $(JETSON) 'cd $(REMOTE) && bash scripts/run_live.sh $(ARGS)'

## drive: the USRP driver on the Jetson, streaming both channels to the
##        sample multicast group (239.192.0.1:4991) until Ctrl-C. Start it
##        AFTER `make run` prints "Listening now". Runs in the foreground of
##        its own terminal; Ctrl-C here stops it cleanly.
##
##          make drive                          the USRP2 at 192.168.10.2
##          make drive ARGS='--seconds 60'      timed
##          make drive ARGS='--synth'           no hardware: planted target
##          make drive ARGS='--synth --synth-silent'   dead-input test
##
##        Three terminals for a live session: `make view`, `make run
##        ARGS='--group 239.192.0.1'`, `make drive`.
drive:
	@ssh -t $(JETSON) 'cd $(REMOTE) && ./build/uhd_to_vita49 \
	    --args addr=192.168.10.2 --config config/prb.conf $(ARGS)'

## stop: end every radar process on the Jetson -- viewer, chain, driver,
##       tegrastats logger -- for when a terminal was closed on one of them
##       and a port is still held. SIGTERM first (clean summaries), then
##       SIGKILL for anything that ignored it. The [_] keeps pkill -f from
##       matching the shell that runs it.
stop:
	@ssh $(JETSON) 'for p in "build/ard[_]view" "build/pbr[_]run" "build/uhd_to[_]vita49" \
	        "build/ingest[_]probe" "scripts/ard_viewer[.]py"; do \
	        pkill -TERM -f "$$p" 2>/dev/null && echo "stopped $$p"; done; \
	    sleep 2; \
	    for p in "build/ard[_]view" "build/pbr[_]run" "build/uhd_to[_]vita49" \
	        "build/ingest[_]probe" "scripts/ard_viewer[.]py"; do \
	        pkill -KILL -f "$$p" 2>/dev/null && echo "killed $$p (ignored SIGTERM)"; done; \
	    pgrep -af "build/(ard_view|pbr_run|uhd_to_vita49|ingest_probe)" | grep -v pgrep \
	        || echo "nothing running"'

## replay: run a scenario's recorded capture through the whole chain on the
##         Jetson with config/fers.conf (204800 Hz, fullscale 0.005; prb.conf
##         describes the USRP and would mis-index it). SCENARIO names the
##         directory under data/ (scenarios/README.md); the run lands in
##         /data/results/L2_performance/SCENARIO/ALG/STAMP with a copy of the
##         config beside it. The quick manual path; scripts/run_experiment.sh
##         is the controlled one with the full manifest.
SCENARIO ?= S0_clean
replay:
	@ssh $(JETSON) 'cd $(REMOTE) && \
	    A=$$(sed -n "s/^DSI_ALGORITHM *= *//p" config/fers.conf) && \
	    OUT=/data/results/L2_performance/$(SCENARIO)/$${A}/$$(date +%Y%m%d_%H%M%S) && \
	    mkdir -p $$OUT && cp config/fers.conf $$OUT/config.conf && \
	    echo "output: $$OUT" && \
	    ./build/pbr_run --file data/$(SCENARIO)/capture.pbrcap --config config/fers.conf \
	        --out $$OUT $(ARGS) \
	        | tee $$OUT/run.log'

## view: watch the running chain in a browser, live. Run this ON THE MAC, like
##       every other target, in a second terminal alongside `make run` or
##       `make replay`. ard_view on the board joins the ARD multicast group
##       pbr_run streams to (ARD_STREAM_DEST) and serves the page; nothing is
##       written to disk. It is a separate process, so it cannot add latency
##       to a CPI.
##
##       Forwarded over the existing ssh path rather than served straight to
##       the subnet: the board's firewall blocks ICMP and may block inbound
##       8080 too, and forwarding needs no board configuration. Where the
##       firewall allows it, http://<board-ip>:8080 also works directly.
##
##       A viewer left running on the board from an earlier session holds
##       8080, so any old instance is stopped first. The [_] in the pattern
##       stops pkill -f matching the very shell that runs it (Linux pkill
##       does not exclude its ancestors; it would kill the ssh command).
##
##       ExitOnForwardFailure: if 8080 is taken on the Mac (a `make host-view`
##       still running, say) fail loudly. Otherwise ssh carries on and the
##       browser silently talks to the wrong server.
view:
	@ssh $(JETSON) 'pkill -f "build/ard[_]view" || true'
	@echo "open http://localhost:8080 -- Ctrl-C here to stop"
	@ssh -o ExitOnForwardFailure=yes -L 8080:localhost:8080 $(JETSON) \
	    'cd $(REMOTE) && ./build/ard_view --group 239.192.0.2'

## verify: every golden-file check. Run after changing src/dsi or src/ard.
##         Goldens live under data/SCENARIO/golden/ (regenerate with the
##         MATLAB generators; none exist there yet as of 2026-10-01).
verify:
	@ssh $(JETSON) 'cd $(REMOTE) && \
	    ./build/dsi_verify --cpi data/$(SCENARIO)/cpi_0000.bin \
	        --golden data/$(SCENARIO)/golden/golden_ecab.bin --algorithm ECA-B --tolerance 1e-2 && \
	    ./build/ard_verify --cpi data/$(SCENARIO)/cpi_0000.bin \
	        --golden data/$(SCENARIO)/golden/golden_ard.bin --chained'

## prelim: ONE algorithm on the scenario's frozen CPI, on the Jetson with
##         clocks pinned: accuracy against the MATLAB golden (CPU FP64 and
##         GPU), time per CPI (dsi_bench), and the chain on the capture
##         (latency, suppression, maps). scripts/run_prelim.sh says exactly
##         what it does. Results land in results/prelim/SCENARIO/ALG/STAMP.
##           make prelim ARGS=ECA-m
prelim:
	@ssh -t $(JETSON) 'cd $(REMOTE) && SCENARIO=$(SCENARIO) STAMP=$(shell date +%Y%m%d_%H%M%S) \
	    bash scripts/run_prelim.sh $(ARGS)'
	@$(MAKE) pull

## push-data: copy data/SCENARIO (capture, frozen CPIs, truth, goldens) to the
##            board. Data never syncs with make build, so this is explicit.
push-data:
	@ssh $(JETSON) 'mkdir -p $(REMOTE)/data/$(SCENARIO)'
	@$(RSYNC_BIN) -a --stats data/$(SCENARIO)/ '$(JETSON):$(REMOTE)/data/$(SCENARIO)/' \
	    | grep -E "Number of files transferred|Total transferred file size" || true

## bench: DSI and ARD timing sweeps with clocks pinned, then pull the CSVs.
bench:
	@ssh -t $(JETSON) 'cd $(REMOTE) && bash scripts/run_dsi_bench.sh $(ARGS)'
	@$(MAKE) pull

## pull: copy everything under the Jetson's /data/results to ./results.
##
##       Incremental: rsync skips what is already here, so the cost is the
##       size of the NEW runs, not of the archive. The scp this replaced
##       re-copied every byte of every past run on every invocation, which is
##       why it grew slower with each run.
##
##       No --delete: removing a run on the board must not silently remove
##       the local copy. Prune the two sides independently.
##       --stats, not --info=stats1: macOS now ships openrsync, which speaks
##       protocol 29 and does not know the newer flag.
pull:
	@mkdir -p results
	@$(RSYNC_BIN) -a --stats '$(JETSON):/data/results/' results/ \
	    | grep -E "Number of files transferred|Total transferred file size|^sent" || true
	@echo "results in ./results"

shell:
	@ssh -t $(JETSON) 'cd $(REMOTE) && exec bash'

clean:
	@ssh $(JETSON) 'cd $(REMOTE) && rm -rf build'
	@echo "wiped the Jetson build directory"

# ---------------------------------------------------------------------------
# Local builds (the Mac)
#
# No CUDA here, so the GPU backends are absent and anything asking for them
# fails loudly rather than falling back. CMake cannot be used either:
# CMakeLists.txt declares CUDA as a project language and will not configure on
# a machine without the toolkit, so these targets call the compiler directly.
#
# Local runs prove FUNCTION only. Every timing number comes from the Jetson.
# ---------------------------------------------------------------------------
HOST_CXX      ?= c++
HOST_CXXFLAGS := -std=c++14 -O2 -Wall -Wextra -Iinclude -pthread
HOST_BIN      := bin

# UHD on the Mac (brew install uhd) lets uhd_to_vita49 drive the N210 from
# here too, since the Mac is on the same switch. pkg-config finds it (UHD
# ships uhd.pc); without it the driver builds --synth only.
#
# Homebrew's UHD is 4.x and its headers need C++17, so this ONE host target
# is compiled as C++17 (the later -std wins). The project standard stays
# C++14: nothing in our sources uses C++17, and the Jetson build, which is
# the one that matters, compiles the same file as C++14 against UHD 3.10.
UHD_CFG := $(shell pkg-config --exists uhd 2>/dev/null && pkg-config --modversion uhd)
ifneq ($(UHD_CFG),)
HOST_UHD_FLAGS := -std=c++17 -DPBR_HAVE_UHD $(shell pkg-config --cflags --libs uhd)
endif

LIB_SRC := src/ingest/CircularBuffer.cpp src/ingest/CpiFile.cpp \
           src/ingest/Config.cpp src/ingest/Vita49Packet.cpp \
           src/ingest/UdpReceiver.cpp src/ingest/ChannelIngest.cpp \
           src/ingest/CaptureFile.cpp src/ingest/Vita49Writer.cpp \
           src/ingest/UdpSender.cpp src/ingest/SynthSource.cpp \
           src/dsi/DSICanceller.cpp src/dsi/cpu/EcaCpu.cpp \
           src/dsi/cpu/CglsCpu.cpp src/dsi/cpu/EcaCdCpu.cpp \
           src/ard/ArdProcessor.cpp src/ard/ArdFile.cpp src/ard/cpu/ArdCpu.cpp \
           src/ard/cpu/BatchesArdCpu.cpp src/ard/ArdStream.cpp \
           src/ard/Spectrum.cpp src/display/HttpServer.cpp

HOST_APPS  := src/app/pbr_run.cpp src/app/ingest_replay.cpp \
              src/app/ingest_probe.cpp src/app/dsi_verify.cpp \
              src/app/ard_verify.cpp src/app/uhd_to_vita49.cpp \
              src/app/ard_view.cpp bench/dsi_bench.cpp bench/ard_bench.cpp
HOST_TESTS := tests/test_circular_buffer.cpp tests/test_vita49_packet.cpp \
              tests/test_config.cpp tests/test_cpi_extraction.cpp \
              tests/test_eca.cpp tests/test_cgls.cpp tests/test_ecacd.cpp \
              tests/test_ard.cpp tests/test_batches_ard.cpp \
              tests/test_vita49_writer.cpp tests/test_ard_stream.cpp \
              tests/test_http.cpp tests/test_spectrum.cpp

## host: build every tool locally into ./bin
host:
	@mkdir -p $(HOST_BIN)
	@for a in $(HOST_APPS); do \
	    n=$$(basename $$a .cpp); \
	    extra=""; \
	    if [ "$$n" = uhd_to_vita49 ]; then extra="$(HOST_UHD_FLAGS)"; fi; \
	    $(HOST_CXX) $(HOST_CXXFLAGS) $$a $(LIB_SRC) $$extra -o $(HOST_BIN)/$$n || exit 1; \
	done
	@if [ -n "$(UHD_CFG)" ]; then echo "uhd_to_vita49 built WITH UHD $(UHD_CFG)"; \
	    else echo "uhd_to_vita49 built without UHD (--synth only); brew install uhd to add it"; fi
	@echo "built into $(HOST_BIN)/"

## host-test: build and run the unit tests locally
host-test:
	@mkdir -p $(HOST_BIN)
	@for t in $(HOST_TESTS); do \
	    n=$$(basename $$t .cpp); \
	    $(HOST_CXX) $(HOST_CXXFLAGS) $$t $(LIB_SRC) -o $(HOST_BIN)/$$n || exit 1; \
	    ./$(HOST_BIN)/$$n > /dev/null 2>&1 \
	        || { echo "  FAILED  $$n"; ./$(HOST_BIN)/$$n; exit 1; }; \
	    echo "  ok      $$n"; \
	done

## host-view: serve one recorded frame from ./data on the Mac, for working on
##            the page without the board. To watch a live board from the Mac
##            instead: bin/ard_view --group 239.192.0.2 (the Mac must be on
##            the switch), then http://localhost:8080.
host-view:
	@./$(HOST_BIN)/ard_view --file data/S0_clean/ard/ard_00000.bin --http 8080
