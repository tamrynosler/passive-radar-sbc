#ifndef PBR_CHECK_HPP
#define PBR_CHECK_HPP

#include <cstdio>

// Two error macros, per the project standard. Neither throws.
//
// CHECK is always on: unlike assert it survives Release / NDEBUG, so it is
// the right tool for correctness guards -- packet sanity, index bounds,
// library return values. It logs to stderr and returns false, so it belongs
// in bool-returning functions. Keep it out of innermost timed loops: guard
// per-packet and per-CPI boundaries, not per-sample.
//
// Plain assert is for expensive invariants only.
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "CHECK failed: %s\n  (%s) at %s:%d\n",       \
                         (msg), #cond, __FILE__, __LINE__);                   \
            return false;                                                     \
        }                                                                     \
    } while (0)

// CUDA_CHECK wraps CUDA / cuFFT / cuBLAS calls. It is only defined where the
// CUDA runtime headers are already in scope, so this header stays includable
// from plain C++ translation units.
#if defined(__CUDACC__) || defined(CUDART_VERSION)
#define CUDA_CHECK(call)                                                      \
    do {                                                                      \
        cudaError_t eErr = (call);                                            \
        if (eErr != cudaSuccess) {                                            \
            std::fprintf(stderr, "CUDA_CHECK failed: %s\n  (%s) at %s:%d\n",  \
                         cudaGetErrorString(eErr), #call, __FILE__, __LINE__);\
            return false;                                                     \
        }                                                                     \
    } while (0)
#endif

#endif // PBR_CHECK_HPP
