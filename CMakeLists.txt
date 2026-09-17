# passive-radar-sbc — top-level build
#
# Target platform: Jetson Nano (Tegra X1, Maxwell sm_53), CUDA 10.2,
# stock CMake 3.10 on L4T / Ubuntu 18.04. Everything below is written to
# work on 3.10 but upgrades gracefully if a newer CMake (e.g. pip-installed)
# is present.

cmake_minimum_required(VERSION 3.10)

project(passive_radar_sbc LANGUAGES CXX CUDA)

# ---------------------------------------------------------------------------
# Standards — CUDA 10.2 tops out at C++14 for device code
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# GPU architecture: sm_53 only (Jetson Nano)
# CMAKE_CUDA_ARCHITECTURES exists from CMake 3.18; fall back to raw
# -gencode flags on the Nano's stock 3.10.
# ---------------------------------------------------------------------------
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.18")
  if(NOT CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 53)
  endif()
else()
  string(APPEND CMAKE_CUDA_FLAGS " -gencode arch=compute_53,code=sm_53")
endif()



# ---------------------------------------------------------------------------
# CUDA math libraries (cuFFT, cuBLAS)
# CMake 3.10 has no CUDA::cufft imported targets, so locate them manually.
# ---------------------------------------------------------------------------
find_library(CUFFT_LIBRARY cufft
  HINTS ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES}
        /usr/local/cuda-10.2/lib64
        /usr/local/cuda/lib64)

find_library(CUBLAS_LIBRARY cublas
  HINTS ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES}
        /usr/local/cuda-10.2/lib64
        /usr/local/cuda/lib64)

# cuSOLVER ships with the CUDA toolkit (verified present on the Nano at
# /usr/local/cuda-10.2/lib64). Used for the batched Cholesky that solves all
# of a CPI's cancellation windows in one call.
find_library(CUSOLVER_LIBRARY cusolver
  HINTS ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES}
        /usr/local/cuda-10.2/lib64
        /usr/local/cuda/lib64)

if(NOT CUFFT_LIBRARY OR NOT CUBLAS_LIBRARY OR NOT CUSOLVER_LIBRARY)
  message(FATAL_ERROR
    "cuFFT/cuBLAS/cuSOLVER not found. Check CUDA 10.2 is installed at /usr/local/cuda-10.2.")
endif()

set(PBR_CUDA_LIBS ${CUFFT_LIBRARY} ${CUBLAS_LIBRARY} ${CUSOLVER_LIBRARY})

# Project-wide public headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Optional helper modules in cmake/ (kept non-fatal while that dir evolves)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---------------------------------------------------------------------------
# Core library: everything under src/ except src/app/
# NOTE: file(GLOB) means CMake must be re-run after adding new source files
# ("make build" from the Mac always re-runs cmake, so this is handled).
# Once the source set stabilises, replace the glob with an explicit list.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PBR_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cu
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER PBR_SOURCES EXCLUDE REGEX "/src/app/")

if(PBR_SOURCES)
  add_library(pbr STATIC ${PBR_SOURCES})
  target_link_libraries(pbr ${PBR_CUDA_LIBS})
  # This build has CUDA, so the GPU cancellers are compiled in and selectable.
  # The host-only Makefile build does not define this, and asking for the GPU
  # backend there fails loudly rather than falling back to the CPU.
  target_compile_definitions(pbr PUBLIC PBR_WITH_CUDA)
  set(PBR_LIB pbr)
else()
  # src/ is still empty scaffolding — build benchmarks standalone for now.
  set(PBR_LIB "")
endif()

# ---------------------------------------------------------------------------
# Standalone benchmarks (bench/)
# ---------------------------------------------------------------------------
add_executable(cufft_bench bench/cufft_bench.cu)
target_link_libraries(cufft_bench ${PBR_LIB} ${PBR_CUDA_LIBS})

# DSI cancellation timing harness. Plain C++ -- it drives whichever backend
# the config or the command line selects and times only the cancel() call.
add_executable(dsi_bench bench/dsi_bench.cpp)
target_link_libraries(dsi_bench ${PBR_LIB} ${PBR_CUDA_LIBS})

# ---------------------------------------------------------------------------
# Application executables: one per source file in src/app/
# ---------------------------------------------------------------------------
file(GLOB APP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cu
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp)

foreach(app_src ${APP_SOURCES})
  get_filename_component(app_name ${app_src} NAME_WE)
  add_executable(${app_name} ${app_src})
  target_link_libraries(${app_name} ${PBR_LIB} ${PBR_CUDA_LIBS})
endforeach()

# ---------------------------------------------------------------------------
# Tests (MATLAB-vs-CUDA validation) — enabled once tests/ gains sources
# ---------------------------------------------------------------------------
file(GLOB TEST_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cu
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)

if(TEST_SOURCES)
  enable_testing()
  foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} ${PBR_LIB} ${PBR_CUDA_LIBS})
    # The tests are built with assert(), and a Release build defines NDEBUG,
    # which turns every one of them into a no-op. Without this the suite runs
    # to completion and reports PASS having checked nothing at all.
    target_compile_options(${test_name} PRIVATE -UNDEBUG)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
