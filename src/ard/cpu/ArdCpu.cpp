// Range-Doppler map formation and CFAR detection on the CPU: the correctness
// reference for src/ard/gpu/ArdGpu.cu.
//
// ---------------------------------------------------------------------------
// The CAF, in three steps
// ---------------------------------------------------------------------------
//
// For each delay bin tau:
//
//   1. Lag product. Multiply the cleaned surveillance signal by the reference
//      delayed by tau and conjugated. Where the surveillance channel really
//      does contain a copy of the reference delayed by tau, these products
//      all point the same way and add up. Everywhere else they point in
//      random directions and cancel.
//
//   2. Integrate and dump. Chop the result into blocks of uDopplerDecimation
//      and sum each block into one number. That is a crude low-pass filter
//      plus a downsample: it throws away variation faster than we care about
//      and shrinks 204800 samples to 1600.
//
//   3. Transform. An FFT across those 1600 numbers says what frequencies are
//      present, and frequency here means Doppler, which means speed.
//
// Then magnitude, and the map is the picture.
//
// ---------------------------------------------------------------------------
// Why a direct DFT and not an FFT
// ---------------------------------------------------------------------------
//
// 1600 = 2^6 * 5^2, so a radix-2 FFT does not apply, and a mixed-radix or
// Bluestein implementation is a lot of subtle code to write and verify.
//
// More importantly this is a REFERENCE. Its job is to be obviously right, and
// to be wrong in different ways than cuFFT if either is wrong at all. A
// direct DFT is ten lines whose correctness is inspectable, and it shares no
// algorithm with the thing it is checking. The cost -- 120 * 1600^2 complex
// operations, a few seconds per CPI -- is the point of the trade, not an
// oversight.
//
// ---------------------------------------------------------------------------
// CFAR
// ---------------------------------------------------------------------------
//
// Every cell is compared against a threshold built from its own
// neighbourhood: sum the training cells, divide by how many there are,
// multiply by a factor chosen so that noise alone produces false alarms at
// the configured rate. Guard cells around the cell under test are left out,
// because a strong echo smears into its neighbours and would otherwise
// inflate its own threshold.
//
// EDGE BEHAVIOUR, deliberate: cells near the border have part of their window
// off the map. Those contributions are taken as zero and the sum is still
// divided by the full training count, exactly as MATLAB's conv2(..., 'same')
// does. The consequence is a lower background estimate, and so over-detection,
// within uCfarTrainRange + uCfarGuardRange bins of the range edges and the
// equivalent in Doppler. This matches the reference exactly, which is what
// makes the golden comparison meaningful; ArdStats::uDetectionsInBorder
// counts how many detections land there so the effect is visible rather than
// silent. Fixing it properly is deferred.

// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// FP32, like the GPU, even though nothing here forces it. A CPU reference in
// double would make every CPU-vs-GPU comparison measure two things at once --
// the backend and the precision -- and the thesis needs it to measure one.
// The direct DFT is still the independent algorithm; only its arithmetic
// width changed.

#include "BatchesArdCpu.hpp"

#include "pbr/ArdProcessor.hpp"
#include "pbr/check.hpp"
#include "../ArdInternal.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace pbr {

namespace {

class ArdCpu : public ArdProcessor {
public:
    bool init(const ArdParams& oParams) override;

    bool process(const ComplexF* pcRef, const ComplexF* pcCleaned,
                 ComplexF* pcMap,
                 Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats) override;

    bool processDevice(const void* d_pRef, const void* d_pCleaned,
                       ComplexF* pcMap,
                       Detection* pDetections, uint64_t uMaxDetections,
                       ArdStats& oStats) override;

    bool detect(const ComplexF* pcMap,
                Detection* pDetections, uint64_t uMaxDetections,
                ArdStats& oStats) override;

    const char* name() const override { return machName; }

private:
    // The map, in steps: the correlation matrix into mvcDecimated, then the
    // Doppler transform into mvcMap.
    void correlateDirect(const ComplexF* pcRef, const ComplexF* pcCleaned);
    void dopplerDft();

    // CFAR over mvfPower.
    bool runCfar(Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats);
    float peakPower() const;
    float trainingSum(uint64_t uDoppler, uint64_t uRange) const;

    ArdParams               moParams;
    uint64_t                muCells       = 0;   // range * Doppler
    // Computed once in double, then narrowed, exactly as the GPU passes it
    // into the kernel. Never recomputed per cell.
    float                   mfAlphaOverTrain = 0.0f;

    std::vector<ComplexF>   mvcDecimated;  // [range][block], pre-transform
    std::vector<ComplexF>   mvcMap;        // [doppler][range], display layout
    std::vector<float>      mvfPower;      // |map|^2, same layout

    // Only constructed when eAlgorithm is Batches.
    BatchesArdCpu           moBatches;

    char                    machName[48] = {};
    bool                    mbReady       = false;
};

bool ArdCpu::init(const ArdParams& oParams)
{
    CHECK(!mbReady, "ArdCpu::init called twice");
    if (!checkArdParams(oParams, mfAlphaOverTrain))
        return false;

    moParams = oParams;
    muCells  = oParams.uRangeBins * oParams.uDopplerBins;

    mvcDecimated.assign(static_cast<size_t>(muCells), ComplexF());
    mvcMap.assign(static_cast<size_t>(muCells), ComplexF());
    mvfPower.assign(static_cast<size_t>(muCells), 0.0f);

    if (oParams.eAlgorithm == ArdAlgorithm::Batches) {
        if (!moBatches.init(oParams.uCpiSamples, oParams.uDopplerDecimation,
                            oParams.uRangeBins))
            return false;
        std::snprintf(machName, sizeof(machName), "ARD-Batches (CPU, L=%llu)",
                      static_cast<unsigned long long>(moBatches.transformLength()));
    } else {
        std::snprintf(machName, sizeof(machName), "ARD-Direct (CPU)");
    }

    mbReady = true;
    return true;
}

// Steps 1 and 2 by the Direct route: for each delay, the lag products summed
// over each block of uDopplerDecimation samples.
void ArdCpu::correlateDirect(const ComplexF* pcRef, const ComplexF* pcCleaned)
{
    const uint64_t uRanges = moParams.uRangeBins;
    const uint64_t uBlocks = moParams.uDopplerBins;
    const uint64_t uDec    = moParams.uDopplerDecimation;

    for (uint64_t r = 0; r < uRanges; ++r) {
        for (uint64_t uBlock = 0; uBlock < uBlocks; ++uBlock) {
            float fRe = 0.0f;
            float fIm = 0.0f;

            const uint64_t uFirst = uBlock * uDec;
            for (uint64_t i = 0; i < uDec; ++i) {
                const uint64_t n = uFirst + i;
                // The reference is zero before the start of the CPI, so a
                // delay that reaches back past it contributes nothing.
                if (n < r)
                    continue;

                const ComplexF& cS = pcCleaned[n];
                const ComplexF& cR = pcRef[n - r];
                // cleaned * conj(delayed reference)
                const float fSr = cS.real(), fSi = cS.imag();
                const float fRr = cR.real(), fRi = cR.imag();
                fRe += fSr * fRr + fSi * fRi;
                fIm += fSi * fRr - fSr * fRi;
            }

            mvcDecimated[static_cast<size_t>(r * uBlocks + uBlock)] = ComplexF(fRe, fIm);
        }
    }
}

// Step 3: transform across blocks, per range bin. Direct DFT, and the
// fftshift and the transpose into display layout are folded in by writing
// straight to the destination index.
//
// The angle is formed in double because k*n reaches 1600*1600 and a float
// product would lose the low bits of the phase outright; the transform
// itself accumulates in float like everything else.
void ArdCpu::dopplerDft()
{
    const uint64_t uRanges = moParams.uRangeBins;
    const uint64_t uBlocks = moParams.uDopplerBins;
    const double dMinusTwoPiOverN = -2.0 * kPi / static_cast<double>(uBlocks);
    const uint64_t uHalf = uBlocks / 2;

    for (uint64_t r = 0; r < uRanges; ++r) {
        const ComplexF* pcRow = &mvcDecimated[static_cast<size_t>(r * uBlocks)];

        for (uint64_t k = 0; k < uBlocks; ++k) {
            float fRe = 0.0f;
            float fIm = 0.0f;
            for (uint64_t n = 0; n < uBlocks; ++n) {
                const double dAngle = dMinusTwoPiOverN * static_cast<double>(k) *
                                      static_cast<double>(n);
                const float fCos = static_cast<float>(std::cos(dAngle));
                const float fSin = static_cast<float>(std::sin(dAngle));
                const ComplexF& cIn = pcRow[static_cast<size_t>(n)];
                fRe += cIn.real() * fCos - cIn.imag() * fSin;
                fIm += cIn.real() * fSin + cIn.imag() * fCos;
            }

            // fftshift: transform bin k belongs at display row
            // (k + N/2) mod N, so that zero Doppler lands in the middle.
            const uint64_t uRow = (k + uHalf) % uBlocks;
            mvcMap[static_cast<size_t>(uRow * uRanges + r)] = ComplexF(fRe, fIm);
        }
    }
}

float ArdCpu::peakPower() const
{
    float fPeakPower = 0.0f;
    for (uint64_t u = 0; u < muCells; ++u) {
        if (mvfPower[static_cast<size_t>(u)] > fPeakPower)
            fPeakPower = mvfPower[static_cast<size_t>(u)];
    }
    return fPeakPower;
}

// The sum of the training cells around (uDoppler, uRange). Anything off the
// map counts as zero and the divisor stays the full training count -- see the
// note at the top of this file.
float ArdCpu::trainingSum(uint64_t uDoppler, uint64_t uRange) const
{
    const int64_t iRanges   = static_cast<int64_t>(moParams.uRangeBins);
    const int64_t iDopplers = static_cast<int64_t>(moParams.uDopplerBins);
    const int64_t iGuardR   = static_cast<int64_t>(moParams.uCfarGuardRange);
    const int64_t iGuardD   = static_cast<int64_t>(moParams.uCfarGuardDoppler);
    const int64_t iHalfR    = iGuardR + static_cast<int64_t>(moParams.uCfarTrainRange);
    const int64_t iHalfD    = iGuardD + static_cast<int64_t>(moParams.uCfarTrainDoppler);

    float fSum = 0.0f;
    for (int64_t iOffD = -iHalfD; iOffD <= iHalfD; ++iOffD) {
        const int64_t iD = static_cast<int64_t>(uDoppler) + iOffD;
        if (iD < 0 || iD >= iDopplers)
            continue;

        const bool bGuardD = (iOffD >= -iGuardD && iOffD <= iGuardD);

        for (int64_t iOffR = -iHalfR; iOffR <= iHalfR; ++iOffR) {
            if (bGuardD && iOffR >= -iGuardR && iOffR <= iGuardR)
                continue;   // guard block, including the cell itself

            const int64_t iR = static_cast<int64_t>(uRange) + iOffR;
            if (iR < 0 || iR >= iRanges)
                continue;

            fSum += mvfPower[static_cast<size_t>(
                static_cast<uint64_t>(iD) * moParams.uRangeBins +
                static_cast<uint64_t>(iR))];
        }
    }
    return fSum;
}

bool ArdCpu::runCfar(Detection* pDetections, uint64_t uMaxDetections,
                     ArdStats& oStats)
{
    const uint64_t uRanges   = moParams.uRangeBins;
    const uint64_t uDopplers = moParams.uDopplerBins;
    const uint64_t uHalfR    = moParams.uCfarGuardRange + moParams.uCfarTrainRange;
    const uint64_t uHalfD    = moParams.uCfarGuardDoppler + moParams.uCfarTrainDoppler;
    const uint64_t uCapacity = (uMaxDetections < kMaxArdDetections) ? uMaxDetections
                                                                    : kMaxArdDetections;

    // Peak, for the dB scale the map is reported on.
    const float fPeakPower = peakPower();
    oStats.dPeakDb = (fPeakPower > 0.0f)
                   ? 10.0 * std::log10(static_cast<double>(fPeakPower)) : 0.0;

    for (uint64_t uDoppler = 0; uDoppler < uDopplers; ++uDoppler) {
        for (uint64_t uRange = 0; uRange < uRanges; ++uRange) {
            const float fCell      = mvfPower[static_cast<size_t>(uDoppler * uRanges + uRange)];
            const float fThreshold = mfAlphaOverTrain * trainingSum(uDoppler, uRange);
            if (!(fCell > fThreshold))
                continue;

            if (oStats.uDetections >= uCapacity) {
                ++oStats.uDetectionsDropped;
                continue;
            }

            Detection& oDet = pDetections[static_cast<size_t>(oStats.uDetections)];
            oDet.uRangeBin   = uRange;
            oDet.uDopplerBin = uDoppler;
            oDet.dRangeKm    = ardRangeKm(uRange, moParams.uSampleRateHz);
            oDet.dDopplerHz  = ardDopplerHz(uDoppler, moParams);
            oDet.dPowerDb    = (fPeakPower > 0.0f)
                             ? 10.0 * std::log10(double(fCell) / double(fPeakPower)) : 0.0;
            oDet.dSnrDb      = (fThreshold > 0.0f)
                             ? 10.0 * std::log10(double(fCell) / double(fThreshold)) : 0.0;
            ++oStats.uDetections;

            const bool bBorder = uRange < uHalfR || uRange >= uRanges - uHalfR ||
                                 uDoppler < uHalfD || uDoppler >= uDopplers - uHalfD;
            if (bBorder)
                ++oStats.uDetectionsInBorder;
        }
    }

    return true;
}

bool ArdCpu::process(const ComplexF* pcRef, const ComplexF* pcCleaned,
                     ComplexF* pcMap,
                     Detection* pDetections, uint64_t uMaxDetections,
                     ArdStats& oStats)
{
    CHECK(mbReady, "ArdCpu::process before a successful init");
    CHECK(pcRef != nullptr && pcCleaned != nullptr,
          "null input buffer passed to ArdCpu::process");
    CHECK(pDetections != nullptr || uMaxDetections == 0,
          "null detection array with non-zero capacity");

    oStats = ArdStats();

    // Two routes to the same correlation matrix; see ArdAlgorithm in
    // ArdProcessor.hpp. Everything after it is shared.
    if (moParams.eAlgorithm == ArdAlgorithm::Batches)
        moBatches.correlate(pcRef, pcCleaned, mvcDecimated.data());
    else
        correlateDirect(pcRef, pcCleaned);
    dopplerDft();

    for (uint64_t u = 0; u < muCells; ++u) {
        const ComplexF& cCell = mvcMap[static_cast<size_t>(u)];
        mvfPower[static_cast<size_t>(u)] = cCell.real() * cCell.real() + cCell.imag() * cCell.imag();
        if (pcMap != nullptr)
            pcMap[static_cast<size_t>(u)] = cCell;
    }

    return runCfar(pDetections, uMaxDetections, oStats);
}

bool ArdCpu::processDevice(const void* /*d_pRef*/, const void* /*d_pCleaned*/,
                           ComplexF* /*pcMap*/,
                           Detection* /*pDetections*/, uint64_t /*uMaxDetections*/,
                           ArdStats& /*oStats*/)
{
    // No device to read from. Fail rather than quietly doing something else.
    std::fprintf(stderr, "ArdCpu: processDevice has no meaning on the CPU "
                         "backend; use process() with host samples\n");
    return false;
}

bool ArdCpu::detect(const ComplexF* pcMap,
                    Detection* pDetections, uint64_t uMaxDetections,
                    ArdStats& oStats)
{
    CHECK(mbReady, "ArdCpu::detect before a successful init");
    CHECK(pcMap != nullptr, "null map passed to ArdCpu::detect");
    CHECK(pDetections != nullptr || uMaxDetections == 0,
          "null detection array with non-zero capacity");

    oStats = ArdStats();

    for (uint64_t u = 0; u < muCells; ++u) {
        const ComplexF& cCell = pcMap[static_cast<size_t>(u)];
        mvfPower[static_cast<size_t>(u)] =
            cCell.real() * cCell.real() + cCell.imag() * cCell.imag();
    }

    return runCfar(pDetections, uMaxDetections, oStats);
}

} // namespace

std::unique_ptr<ArdProcessor> makeArdProcessorCpu()
{
    return std::unique_ptr<ArdProcessor>(new ArdCpu());
}

} // namespace pbr
