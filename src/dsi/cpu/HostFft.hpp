#ifndef PBR_HOST_FFT_HPP
#define PBR_HOST_FFT_HPP

// In-place radix-2 FFT on the host, for one power-of-two length.
//
// The arithmetic is a copy of EcaCdCpu's buildFftTables / fftT (same twiddle
// table, same bit reversal, same butterfly order), so the two give identical
// results. EcaCdCpu keeps its own copy for now.
//
// Values are stored as std::complex<double> and every operation is done in
// Acc (float or double), so a float result is held exactly, as in the
// canceller references. The inverse is unnormalised: the caller divides by
// the length.

#include <complex>
#include <cstdint>
#include <vector>

namespace pbr {

class HostFft {
public:
    // Build the tables. The length must be a power of two, at least 2.
    bool init(uint64_t uLength);

    uint64_t length() const { return muLength; }

    template <typename Acc>
    void transform(std::complex<double>* pc, bool bInverse) const
    {
        const uint64_t uL = muLength;

        for (uint64_t i = 0; i < uL; ++i) {
            const uint64_t j = mvuBitReverse[static_cast<size_t>(i)];
            if (j > i) {
                const std::complex<double> cSwap = pc[i];
                pc[i] = pc[j];
                pc[j] = cSwap;
            }
        }

        // Butterflies of span 2, 4, ..., L. The twiddle for position i in a
        // span of uLen is exp(-j 2 pi i / uLen) = table[i * L / uLen].
        for (uint64_t uLen = 2; uLen <= uL; uLen <<= 1) {
            const uint64_t uHalf = uLen / 2;
            const uint64_t uStep = uL / uLen;

            for (uint64_t uBlock = 0; uBlock < uL; uBlock += uLen) {
                for (uint64_t i = 0; i < uHalf; ++i) {
                    const std::complex<double>& cTw = mvcTwiddle[static_cast<size_t>(i * uStep)];
                    const Acc tTr = Acc(cTw.real());
                    const Acc tTi = bInverse ? -Acc(cTw.imag()) : Acc(cTw.imag());

                    std::complex<double>& cA = pc[uBlock + i];
                    std::complex<double>& cB = pc[uBlock + i + uHalf];
                    const Acc tBr = Acc(cB.real()), tBi = Acc(cB.imag());
                    const Acc tPr = tBr * tTr - tBi * tTi;   // b * twiddle
                    const Acc tPi = tBr * tTi + tBi * tTr;
                    const Acc tAr = Acc(cA.real()), tAi = Acc(cA.imag());

                    cA = std::complex<double>(static_cast<double>(tAr + tPr),
                                              static_cast<double>(tAi + tPi));
                    cB = std::complex<double>(static_cast<double>(tAr - tPr),
                                              static_cast<double>(tAi - tPi));
                }
            }
        }
    }

private:
    uint64_t                          muLength = 0;
    std::vector<std::complex<double>> mvcTwiddle;     // L/2 entries, exp(-j 2 pi i / L)
    std::vector<uint32_t>             mvuBitReverse;  // L entries
};

} // namespace pbr

#endif // PBR_HOST_FFT_HPP
