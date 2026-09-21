// Synthetic signals shared by the DSI canceller tests.
//
// Header-only and test-only. CMake globs tests/*.cpp, so this is not built on
// its own. tests/test_eca.cpp predates it and carries its own copy of the RNG
// with the history of how it went wrong; this is the same generator.
#ifndef PBR_TEST_SIGNALS_HPP
#define PBR_TEST_SIGNALS_HPP

#include "pbr/types.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace pbrtest {

// splitmix64 with a hashed seed. See tests/test_eca.cpp for why the seed is
// hashed and why the output is 53 bits over 2^53: both were bugs once.
struct Rng {
    uint64_t uState;

    explicit Rng(uint64_t uSeed)
    {
        uint64_t z = uSeed + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        uState = z ^ (z >> 31);
    }

    double next()   // uniform in [-1, 1)
    {
        uState += 0x9E3779B97F4A7C15ULL;
        uint64_t z = uState;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z =  z ^ (z >> 31);
        return (static_cast<double>(z >> 11) / 9007199254740992.0) * 2.0 - 1.0;
    }
};

inline std::vector<pbr::ComplexF> makeNoise(uint64_t uCount, uint64_t uSeed)
{
    Rng oRng(uSeed);
    std::vector<pbr::ComplexF> vc(static_cast<size_t>(uCount));
    for (size_t u = 0; u < vc.size(); ++u)
        vc[u] = pbr::ComplexF(static_cast<float>(oRng.next()),
                              static_cast<float>(oRng.next()));
    return vc;
}

inline double totalPower(const std::vector<pbr::ComplexF>& vc)
{
    double d = 0.0;
    for (size_t u = 0; u < vc.size(); ++u)
        d += double(vc[u].real()) * vc[u].real() + double(vc[u].imag()) * vc[u].imag();
    return d;
}

// sqrt(|a - b|^2 / |b|^2)
inline double relativeL2(const std::vector<pbr::ComplexF>& vcA,
                         const std::vector<pbr::ComplexF>& vcB)
{
    double dDiff = 0.0, dRef = 0.0;
    for (size_t u = 0; u < vcA.size(); ++u) {
        const double dRe = double(vcA[u].real()) - vcB[u].real();
        const double dIm = double(vcA[u].imag()) - vcB[u].imag();
        dDiff += dRe * dRe + dIm * dIm;
        dRef  += double(vcB[u].real()) * vcB[u].real() +
                 double(vcB[u].imag()) * vcB[u].imag();
    }
    return (dRef > 0.0) ? std::sqrt(dDiff / dRef) : 0.0;
}

// Stationary clutter: sum over k of w[k] r[n-k], with the reference zero
// before the start of the CPI. Weights strongest at zero delay, the way the
// direct signal is.
inline std::vector<pbr::ComplexF> makeClutter(const std::vector<pbr::ComplexF>& vcRef,
                                              uint64_t uTaps)
{
    std::vector<pbr::ComplexF> vcSur(vcRef.size());
    for (uint64_t n = 0; n < vcRef.size(); ++n) {
        std::complex<double> cAcc(0.0, 0.0);
        const uint64_t uK = (n + 1 < uTaps) ? (n + 1) : uTaps;
        for (uint64_t k = 0; k < uK; ++k) {
            const std::complex<double> cW(1.0 / (1.0 + double(k)), 0.3 / (1.0 + double(k)));
            const pbr::ComplexF& cR = vcRef[static_cast<size_t>(n - k)];
            cAcc += cW * std::complex<double>(cR.real(), cR.imag());
        }
        vcSur[static_cast<size_t>(n)] = pbr::ComplexF(static_cast<float>(cAcc.real()),
                                                      static_cast<float>(cAcc.imag()));
    }
    return vcSur;
}

// A weak echo: the reference delayed by uDelay samples, shifted by dCyclesPerSample
// in frequency, scaled by dAmp. Zero before the delay.
inline std::vector<pbr::ComplexF> makeTarget(const std::vector<pbr::ComplexF>& vcRef,
                                             uint64_t uDelay, double dCyclesPerSample,
                                             double dAmp)
{
    std::vector<pbr::ComplexF> vc(vcRef.size(), pbr::ComplexF(0.0f, 0.0f));
    for (uint64_t n = uDelay; n < vcRef.size(); ++n) {
        const double dPhase = 2.0 * 3.14159265358979323846 * dCyclesPerSample * double(n);
        const pbr::ComplexF& cR = vcRef[static_cast<size_t>(n - uDelay)];
        const std::complex<double> c =
            dAmp * std::complex<double>(std::cos(dPhase), std::sin(dPhase)) *
            std::complex<double>(cR.real(), cR.imag());
        vc[static_cast<size_t>(n)] = pbr::ComplexF(static_cast<float>(c.real()),
                                                   static_cast<float>(c.imag()));
    }
    return vc;
}

inline std::vector<pbr::ComplexF> add(const std::vector<pbr::ComplexF>& vcA,
                                      const std::vector<pbr::ComplexF>& vcB)
{
    std::vector<pbr::ComplexF> vc(vcA.size());
    for (size_t u = 0; u < vc.size(); ++u)
        vc[u] = vcA[u] + vcB[u];
    return vc;
}

// How much of vcOut differs from the target that was put in, as a fraction
// of the target's power. Zero means the target came through untouched.
inline double targetResidualFraction(const std::vector<pbr::ComplexF>& vcOut,
                                     const std::vector<pbr::ComplexF>& vcTarget)
{
    double dResidual = 0.0;
    for (size_t u = 0; u < vcOut.size(); ++u) {
        const double dRe = double(vcOut[u].real()) - vcTarget[u].real();
        const double dIm = double(vcOut[u].imag()) - vcTarget[u].imag();
        dResidual += dRe * dRe + dIm * dIm;
    }
    return dResidual / totalPower(vcTarget);
}

} // namespace pbrtest

#endif // PBR_TEST_SIGNALS_HPP
