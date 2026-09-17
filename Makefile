# passive-radar-sbc — host-side driver (runs on the Mac)
#
# All editing happens here on the Mac; the Jetson is a disposable
# build/run target. NEVER edit files directly on the Jetson —
# rsync --delete will clobber them on the next sync.
#
# Requires: `Host jetson` alias in ~/.ssh/config with key auth.

JETSON  ?= jetson
REMOTE  ?= ~/passive-radar-sbc
NVCC := /usr/local/cuda-10.2/bin/nvcc

# Exclusions are driven by .gitignore (per-directory merge), so anything
# ignored by git is also kept off the Jetson. The explicit excludes below
# are belt-and-braces for paths that must never sync regardless.
# 'P' is a protect rule: it stops --delete removing these on the Jetson.
# data/ is gitignored, so the filter above hides it from the transfer, and
# without protection every `make build` would wipe the recorded captures and
# golden files the benchmark needs. build/ and results/ are protected for the
# same reason.
RSYNC := rsync -av --delete \
         --filter='P data' \
         --filter='P results' \
         --filter='P build' \
         --filter=':- .gitignore' \
         --exclude '.git' \
         --exclude 'build' \
         --exclude 'results' \
         --exclude '.DS_Store'

.PHONY: sync configure build test bench pull-results shell clean-remote \
        host-probe host-replay host-test host-bench host-verify host-ard \
        host-run \
        host-clean \
        dsi-bench

# ---------------------------------------------------------------------------
# Host-only builds — run on the Mac, no Jetson and no CUDA involved.
#
# The ingest layer (VITA 49 decode, UDP socket, circular buffer) is plain
# C++14 and POSIX sockets, so it builds and runs locally. CMake cannot be
# used for this: CMakeLists.txt declares CUDA as a project language, so
# `cmake ..` fails outright on a machine without a CUDA toolkit. These
# targets therefore call the compiler directly.
#
# Local runs prove FUNCTION only. All timing and performance numbers must
# come from the Jetson via `make bench`.
# ---------------------------------------------------------------------------
HOST_CXX      ?= c++
HOST_CXXFLAGS := -std=c++14 -O2 -Wall -Wextra -Iinclude -pthread
HOST_BIN      := bin
INGEST_SRC    := src/ingest/CircularBuffer.cpp \
                 src/ingest/CpiFile.cpp \
                 src/ingest/Config.cpp \
                 src/ingest/Vita49Packet.cpp \
                 src/ingest/UdpReceiver.cpp \
                 src/ingest/ChannelIngest.cpp \
                 src/ingest/CaptureFile.cpp
DSI_SRC       := src/dsi/DSICanceller.cpp \
                 src/dsi/cpu/EcaCpu.cpp
ARD_SRC       := src/ard/ArdProcessor.cpp \
                 src/ard/ArdFile.cpp \
                 src/ard/cpu/ArdCpu.cpp
LIB_SRC       := $(INGEST_SRC) $(DSI_SRC) $(ARD_SRC)
HOST_TESTS    := tests/test_circular_buffer.cpp tests/test_vita49_packet.cpp \
                 tests/test_config.cpp tests/test_cpi_extraction.cpp \
                 tests/test_eca.cpp tests/test_ard.cpp

## host-probe: build the live FERS ingest checkpoint locally -> bin/ingest_probe
host-probe:
	@mkdir -p $(HOST_BIN)
	$(HOST_CXX) $(HOST_CXXFLAGS) src/app/ingest_probe.cpp $(LIB_SRC) \
	    -o $(HOST_BIN)/ingest_probe
	@echo "built $(HOST_BIN)/ingest_probe  —  run it BEFORE starting fers-cli"

## host-replay: build the capture replay tool locally -> bin/ingest_replay
host-replay:
	@mkdir -p $(HOST_BIN)
	$(HOST_CXX) $(HOST_CXXFLAGS) src/app/ingest_replay.cpp $(LIB_SRC) \
	    -o $(HOST_BIN)/ingest_replay
	@echo "built $(HOST_BIN)/ingest_replay"

## host-test: build and run every unattended unit test locally
host-test:
	@mkdir -p $(HOST_BIN)
	@for t in $(HOST_TESTS); do \
	    n=$$(basename $$t .cpp); \
	    $(HOST_CXX) $(HOST_CXXFLAGS) $$t $(LIB_SRC) -o $(HOST_BIN)/$$n || exit 1; \
	    ./$(HOST_BIN)/$$n || exit 1; \
	done

## host-bench: build the DSI cancellation benchmark locally -> bin/dsi_bench
host-bench:
	@mkdir -p $(HOST_BIN)
	$(HOST_CXX) $(HOST_CXXFLAGS) bench/dsi_bench.cpp $(LIB_SRC) \
	    -o $(HOST_BIN)/dsi_bench
	@echo "built $(HOST_BIN)/dsi_bench  —  timing here proves nothing; use the Jetson"

## host-verify: build the MATLAB golden-file comparison tool -> bin/dsi_verify
host-verify:
	@mkdir -p $(HOST_BIN)
	$(HOST_CXX) $(HOST_CXXFLAGS) src/app/dsi_verify.cpp $(LIB_SRC) \
	    -o $(HOST_BIN)/dsi_verify
	@echo "built $(HOST_BIN)/dsi_verify"

## host-ard: build the ARD verification tool locally -> bin/ard_verify
host-ard:
	@mkdir -p $(HOST_BIN)
	$(HOST_CXX) $(HOST_CXXFLAGS) src/app/ard_verify.cpp $(LIB_SRC) \
	    -o $(HOST_BIN)/ard_verify
	@echo "built $(HOST_BIN)/ard_verify"

## host-run: build the full chain runner locally -> bin/pbr_run
host-run:
	@mkdir -p $(HOST_BIN)
	$(HOST_CXX) $(HOST_CXXFLAGS) src/app/pbr_run.cpp $(LIB_SRC) \
	    -o $(HOST_BIN)/pbr_run
	@echo "built $(HOST_BIN)/pbr_run"

## host-clean: remove locally built binaries
host-clean:
	rm -rf $(HOST_BIN)

## sync: mirror the local tree to the Jetson (build/ and results survive)
sync:
	$(RSYNC) ./ $(JETSON):$(REMOTE)/

## configure: run CMake configure step only (useful when debugging CMake)
configure: sync
		ssh $(JETSON) 'cd $(REMOTE) && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=$(NVCC) ..'
	
build: sync
		ssh $(JETSON) 'cd $(REMOTE) && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=$(NVCC) .. && make -j2'

## test: build then run every unit test in tests/ via ctest
test: build
	ssh $(JETSON) 'cd $(REMOTE)/build && ctest --output-on-failure'

## bench: full benchmark run via scripts/run_cufft_bench.sh
##        (pins clocks, logs tegrastats, writes CSV to /data/results),
##        then pulls results back to ./results on the Mac
bench: build
	ssh $(JETSON) 'cd $(REMOTE) && bash scripts/run_cufft_bench.sh'
	$(MAKE) pull-results

## dsi-bench: DSI cancellation sweep on the Jetson with clocks pinned and
##            tegrastats logged, then pull the results back.
##            Needs an interactive sudo password on the board, so run it from
##            a terminal rather than unattended.
dsi-bench: build
	ssh -t $(JETSON) 'cd $(REMOTE) && bash scripts/run_dsi_bench.sh'
	$(MAKE) pull-results

## pull-results: copy CSV/logs from the Jetson SD card to ./results
pull-results:
	mkdir -p results
	-scp '$(JETSON):/data/results/*.csv' results/
	-scp '$(JETSON):/data/results/*.log' results/
	-scp -r '$(JETSON):/data/results/dsi_*' results/

## shell: interactive shell in the remote repo
shell:
	ssh -t $(JETSON) 'cd $(REMOTE) && exec bash'

## clean-remote: wipe the remote build directory
clean-remote:
	ssh $(JETSON) 'cd $(REMOTE) && rm -rf build'
