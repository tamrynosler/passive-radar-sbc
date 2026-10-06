# passive-radar-sbc — top-level build
#
# Target platform: Jetson Nano (Tegra X1, Maxwell sm_53), CUDA 10.2,
# stock CMake 3.10 on L4T / Ubuntu 18.04.

cmake_minimum_required(VERSION 3.10)

project(passive_radar_sbc LANGUAGES CXX CUDA)

# CUDA 10.2 tops out at C++14 for device code.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# sm_53 only. CMake 3.10 has no CMAKE_CUDA_ARCHITECTURES.
string(APPEND CMAKE_CUDA_FLAGS " -gencode arch=compute_53,code=sm_53")

# CMake 3.10 has no CUDA::cufft imported targets, so locate the libraries
# by hand. cuSOLVER provides the batched Cholesky of the GPU cancellers.
set(PBR_CUDA_LIBS "")
foreach(lib cufft cublas cusolver)
  find_library(${lib}_LIBRARY ${lib}
    HINTS ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES} /usr/local/cuda-10.2/lib64)
  if(NOT ${lib}_LIBRARY)
    message(FATAL_ERROR "${lib} not found. Check CUDA 10.2 is installed at /usr/local/cuda-10.2.")
  endif()
  list(APPEND PBR_CUDA_LIBS ${${lib}_LIBRARY})
endforeach()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Core library: everything under src/ except src/app/. The glob is re-read
# because `make build` re-runs cmake every time.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PBR_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cu
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER PBR_SOURCES EXCLUDE REGEX "/src/app/")

add_library(pbr STATIC ${PBR_SOURCES})
target_link_libraries(pbr ${PBR_CUDA_LIBS})
# The GPU backends are compiled in. The host-only Makefile build does not
# define this, and asking for the GPU there fails loudly instead of falling
# back to the CPU.
target_compile_definitions(pbr PUBLIC PBR_WITH_CUDA)

# ---------------------------------------------------------------------------
# Benchmarks (bench/)
# ---------------------------------------------------------------------------
add_executable(cufft_bench bench/cufft_bench.cu)
target_link_libraries(cufft_bench ${PBR_CUDA_LIBS})

add_executable(dsi_bench bench/dsi_bench.cpp)
target_link_libraries(dsi_bench pbr)

add_executable(ard_bench bench/ard_bench.cpp)
target_link_libraries(ard_bench pbr)

# ---------------------------------------------------------------------------
# Executables: one per source file in src/app/
# ---------------------------------------------------------------------------
file(GLOB APP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp)
foreach(app_src ${APP_SOURCES})
  get_filename_component(app_name ${app_src} NAME_WE)
  add_executable(${app_name} ${app_src})
  target_link_libraries(${app_name} pbr)
endforeach()

# UHD is optional: without it uhd_to_vita49 builds with --synth only.
# Ubuntu 18.04's libuhd-dev 3.10.3 ships uhd.pc and no UHDConfig.cmake, so
# pkg-config is the only route that works on the board.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(UHD QUIET uhd)
endif()
if(UHD_FOUND)
  target_compile_definitions(uhd_to_vita49 PRIVATE PBR_HAVE_UHD)
  target_include_directories(uhd_to_vita49 PRIVATE ${UHD_INCLUDE_DIRS})
  target_link_libraries(uhd_to_vita49 ${UHD_LIBRARIES})
  message(STATUS "UHD ${UHD_VERSION} found via pkg-config: uhd_to_vita49 has USRP support")
else()
  message(STATUS "UHD not found: uhd_to_vita49 built with --synth only")
endif()

# ---------------------------------------------------------------------------
# Unit tests: one executable per source file in tests/
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} pbr)
  # A Release build defines NDEBUG, which turns every assert() into a no-op;
  # without this the suite reports PASS having checked nothing.
  target_compile_options(${test_name} PRIVATE -UNDEBUG)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
