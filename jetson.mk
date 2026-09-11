CUDA_PATH ?= /usr/local/cuda-10.2
NVCC      := $(CUDA_PATH)/bin/nvcc
ARCH      := -gencode arch=compute_53,code=sm_53
CXXFLAGS  := -O3 -std=c++14
LIBS      := -lcufft -lcublas

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/cufft_harness

all: $(TARGET)

$(TARGET): src/cufft_harness.cu | $(BUILD_DIR)
	$(NVCC) $(ARCH) $(CXXFLAGS) $< -o $@ $(LIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean
