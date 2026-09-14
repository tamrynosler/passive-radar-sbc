cmake_minimum_required(VERSION 3.18)

# ------------------------------------------------------------------------------
# smartpol-pbr : FM passive bistatic radar processing on embedded GPU
#
# Requires a CMake >= 3.18 for first-class CUDA + CUDAToolkit support.
# The Ubuntu 18.04 (L4T R32.7) apt CMake is 3.10 and will NOT work. Get a modern
# one on the Nano with:
#     python3 -m pip install --user "cmake>=3.18"
# and ensure ~/.local/bin is on PATH, or use the Kitware apt repository.
#
# If you would rather not touch CMake at all, the benchmark builds standalone:
#     nvcc -O3 -arch=sm_53 bench/cufft_bench.cu -lcufft -o cufft_bench
# ------------------------------------------------------------------------------

project(smartpol_pbr LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# Jetson Nano / Tegra X1 is compute capability 5.3 (sm_53). Override on the
# command line for other targets, e.g. -DCMAKE_CUDA_ARCHITECTURES=87 for Orin.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 53)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CUDAToolkit REQUIRED)

# --- benchmark harness --------------------------------------------------------
add_executable(cufft_bench bench/cufft_bench.cu)
target_link_libraries(cufft_bench PRIVATE CUDA::cufft CUDA::cudart)

# Processing-chain libraries (ingest, dsp, fft, linalg) get added here as the
# port proceeds, e.g.:
#   add_subdirectory(src)
