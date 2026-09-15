# passive-radar-sbc — convenience wrapper for interactive use ON the Jetson.
#
# The real build system is CMake (see CMakeLists.txt); the Mac-side
# Makefile drives it over SSH. This file exists only so that, when
# SSH'd in directly, you can type `make -f jetson.mk` instead of
# remembering the cmake incantation. It contains NO build rules of
# its own — do not add compiler flags here; they belong in CMakeLists.txt.

BUILD_DIR := build

all:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_CUDA_COMPILER=/usr/local/cuda-10.2/bin/nvcc .. && make -j2

run: all
	./$(BUILD_DIR)/cufft_bench

bench: all
	bash scripts/run_cufft_bench.sh

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run bench clean
