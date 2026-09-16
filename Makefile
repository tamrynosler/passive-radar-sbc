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
RSYNC := rsync -av --delete \
         --filter=':- .gitignore' \
         --exclude '.git' \
         --exclude 'build' \
         --exclude 'results' \
         --exclude '.DS_Store'

.PHONY: sync configure build test bench pull-results shell clean-remote

## sync: mirror the local tree to the Jetson (build/ and results survive)
sync:
	$(RSYNC) ./ $(JETSON):$(REMOTE)/

## configure: run CMake configure step only (useful when debugging CMake)
configure: sync
		ssh $(JETSON) 'cd $(REMOTE) && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=$(NVCC) ..'
	
build: sync
		ssh $(JETSON) 'cd $(REMOTE) && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=$(NVCC) .. && make -j2'

## test: build then run the cuFFT harness interactively (output to terminal)
test: build
	ssh $(JETSON) 'cd $(REMOTE) && ./build/cufft_bench'

## bench: full benchmark run via scripts/run_cufft_bench.sh
##        (pins clocks, logs tegrastats, writes CSV to /data/results),
##        then pulls results back to ./results on the Mac
bench: build
	ssh $(JETSON) 'cd $(REMOTE) && bash scripts/run_cufft_bench.sh'
	$(MAKE) pull-results

## pull-results: copy CSV/logs from the Jetson SD card to ./results
pull-results:
	mkdir -p results
	-scp '$(JETSON):/data/results/*.csv' results/
	-scp '$(JETSON):/data/results/*.log' results/

## shell: interactive shell in the remote repo
shell:
	ssh -t $(JETSON) 'cd $(REMOTE) && exec bash'

## clean-remote: wipe the remote build directory
clean-remote:
	ssh $(JETSON) 'cd $(REMOTE) && rm -rf build'
