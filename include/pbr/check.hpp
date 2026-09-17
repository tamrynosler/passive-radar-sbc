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

// cuBLAS and cuSOLVER report status codes of their own rather than
// cudaError_t, and neither has a strerror-style helper in CUDA 10.2, so the
// numeric code is printed. Both are only defined when the corresponding
// library header is already in scope -- include <cublas_v2.h> / <cusolverDn.h>
// BEFORE this header.
#if defined(CUBLAS_V2_H_)
#define CUBLAS_CHECK(call)                                                    \
    do {                                                                      \
        cublasStatus_t eStatus = (call);                                      \
        if (eStatus != CUBLAS_STATUS_SUCCESS) {                               \
            std::fprintf(stderr, "CUBLAS_CHECK failed: status %d\n"           \
                                 "  (%s) at %s:%d\n",                         \
                         static_cast<int>(eStatus), #call, __FILE__, __LINE__);\
            return false;                                                     \
        }                                                                     \
    } while (0)
#endif

#if defined(CUSOLVERDN_H_)
#define CUSOLVER_CHECK(call)                                                  \
    do {                                                                      \
        cusolverStatus_t eStatus = (call);                                    \
        if (eStatus != CUSOLVER_STATUS_SUCCESS) {                             \
            std::fprintf(stderr, "CUSOLVER_CHECK failed: status %d\n"         \
                                 "  (%s) at %s:%d\n",                         \
                         static_cast<int>(eStatus), #call, __FILE__, __LINE__);\
            return false;                                                     \
        }                                                                     \
    } while (0)
#endif

#endif // PBR_CHECK_HPP
