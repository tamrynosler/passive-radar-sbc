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
RSYNC := rsync -a --delete \
         --filter='P data' --filter='P results' --filter='P build' \
         --filter=':- .gitignore' \
         --exclude '.git' --exclude '.DS_Store'

.PHONY: help build sync test run replay verify bench pull shell clean \
        host host-test

help:
	@echo "passive-radar-sbc"
	@echo
	@echo "  make sync       push config + source to the Jetson (no compile)"
	@echo "  make build      sync, then compile on the Jetson"
	@echo "  make test       run the unit tests on the Jetson"
	@echo "  make run        live run against FERS, clocks pinned"
	@echo "  make replay     replay the recorded capture on the Jetson"
	@echo "  make verify     golden-file checks against MATLAB"
	@echo "  make bench      DSI + ARD timing sweeps, clocks pinned"
	@echo "  make pull       copy results back to ./results"
	@echo "  make shell      interactive shell on the Jetson"
	@echo "  make clean      wipe the Jetson build directory"
	@echo
	@echo "  make host       build every tool locally into ./bin (no CUDA)"
	@echo "  make host-test  run the unit tests locally"
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

## replay: run the recorded capture through the whole chain on the Jetson.
##         Each run gets its own directory named after the algorithm and the
##         time, with a copy of the config beside it, so runs of different
##         algorithms can be pulled back and compared side by side.
replay:
	@ssh $(JETSON) 'cd $(REMOTE) && \
	    A=$$(sed -n "s/^DSI_ALGORITHM *= *//p" config/prb.conf) && \
	    OUT=/data/results/replay_$${A}_$$(date +%Y%m%d_%H%M%S) && \
	    mkdir -p $$OUT && cp config/prb.conf $$OUT/prb.conf && \
	    echo "output: $$OUT" && \
	    ./build/pbr_run --file data/fers_capture.pbrcap --out $$OUT $(ARGS) \
	        | tee $$OUT/run.log'

## verify: every golden-file check. Run after changing src/dsi or src/ard.
verify:
	@ssh $(JETSON) 'cd $(REMOTE) && \
	    ./build/dsi_verify --cpi data/cpi_0000.bin \
	        --golden data/golden_ecab.bin --algorithm ECA-B --tolerance 1e-2 && \
	    ./build/ard_verify --cpi data/cpi_0000.bin \
	        --golden data/golden_ard.bin --chained'

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
	@rsync -a --stats '$(JETSON):/data/results/' results/ \
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

LIB_SRC := src/ingest/CircularBuffer.cpp src/ingest/CpiFile.cpp \
           src/ingest/Config.cpp src/ingest/Vita49Packet.cpp \
           src/ingest/UdpReceiver.cpp src/ingest/ChannelIngest.cpp \
           src/ingest/CaptureFile.cpp \
           src/dsi/DSICanceller.cpp src/dsi/cpu/EcaCpu.cpp \
           src/dsi/cpu/CglsCpu.cpp src/dsi/cpu/EcaCdCpu.cpp \
           src/ard/ArdProcessor.cpp src/ard/ArdFile.cpp src/ard/cpu/ArdCpu.cpp

HOST_APPS  := src/app/pbr_run.cpp src/app/ingest_replay.cpp \
              src/app/ingest_probe.cpp src/app/dsi_verify.cpp \
              src/app/ard_verify.cpp bench/dsi_bench.cpp bench/ard_bench.cpp
HOST_TESTS := tests/test_circular_buffer.cpp tests/test_vita49_packet.cpp \
              tests/test_config.cpp tests/test_cpi_extraction.cpp \
              tests/test_eca.cpp tests/test_cgls.cpp tests/test_ecacd.cpp \
              tests/test_ard.cpp

## host: build every tool locally into ./bin
host:
	@mkdir -p $(HOST_BIN)
	@for a in $(HOST_APPS); do \
	    n=$$(basename $$a .cpp); \
	    $(HOST_CXX) $(HOST_CXXFLAGS) $$a $(LIB_SRC) -o $(HOST_BIN)/$$n || exit 1; \
	done
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
