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

// CUDA_CHECK wraps every CUDA runtime, cuFFT, cuBLAS and cuSOLVER call.
// Each library reports failure with its own status type, so gpuCallOk has
// one overload per type: true on success, otherwise false with a
// description in pWhy (formatted only on failure, so the check is free on the
// timed path).
// Only CUDA translation units (.cu) see it, so this header stays includable
// from plain C++.
#if defined(__CUDACC__)
#include <cstddef>
#include <cublas_v2.h>
#include <cuda_runtime.h>
#include <cufft.h>
#include <cusolverDn.h>

namespace pbr {

inline bool gpuCallOk(cudaError_t eStatus, char* pWhy, size_t uWhyBytes)
{
    if (eStatus == cudaSuccess)
        return true;
    std::snprintf(pWhy, uWhyBytes, "%s", cudaGetErrorString(eStatus));
    return false;
}

// cuFFT, cuBLAS and cuSOLVER have no strerror-style helper in CUDA 10.2,
// so the numeric status is printed.
inline bool gpuCallOk(cufftResult eStatus, char* pWhy, size_t uWhyBytes)
{
    if (eStatus == CUFFT_SUCCESS)
        return true;
    std::snprintf(pWhy, uWhyBytes, "cuFFT status %d", static_cast<int>(eStatus));
    return false;
}

inline bool gpuCallOk(cublasStatus_t eStatus, char* pWhy, size_t uWhyBytes)
{
    if (eStatus == CUBLAS_STATUS_SUCCESS)
        return true;
    std::snprintf(pWhy, uWhyBytes, "cuBLAS status %d", static_cast<int>(eStatus));
    return false;
}

inline bool gpuCallOk(cusolverStatus_t eStatus, char* pWhy, size_t uWhyBytes)
{
    if (eStatus == CUSOLVER_STATUS_SUCCESS)
        return true;
    std::snprintf(pWhy, uWhyBytes, "cuSOLVER status %d", static_cast<int>(eStatus));
    return false;
}

} // namespace pbr

#define CUDA_CHECK(call)                                                      \
    do {                                                                      \
        char achWhy[64];                                                      \
        if (!pbr::gpuCallOk((call), achWhy, sizeof(achWhy))) {                \
            std::fprintf(stderr, "CUDA_CHECK failed: %s\n  (%s) at %s:%d\n",  \
                         achWhy, #call, __FILE__, __LINE__);                  \
            return false;                                                     \
        }                                                                     \
    } while (0)
#endif

#endif // PBR_CHECK_HPP
