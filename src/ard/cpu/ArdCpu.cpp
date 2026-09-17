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

#include "pbr/ArdProcessor.hpp"
#include "pbr/check.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

typedef std::complex<double> ComplexAcc;

const double kPi = 3.14159265358979323846;

class ArdCpu : public ArdProcessor {
public:
    ArdCpu() { std::memset(machName, 0, sizeof(machName)); }

    bool init(const ArdParams& oParams) override;

    bool process(const ComplexF* pcRef, const ComplexF* pcCleaned,
                 ComplexF* pcMap,
                 Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats) override;

    bool processDevice(const void* pdRef, const void* pdCleaned,
                       ComplexF* pcMap,
                       Detection* pDetections, uint64_t uMaxDetections,
                       ArdStats& oStats) override;

    bool detect(const ComplexF* pcMap,
                Detection* pDetections, uint64_t uMaxDetections,
                ArdStats& oStats) override;

    const char* name() const override { return machName; }

private:
    void buildMap(const ComplexF* pcRef, const ComplexF* pcCleaned);
    bool runCfar(Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats);

    ArdParams               moParams;
    uint64_t                muCells       = 0;   // range * Doppler
    uint64_t                muTrainCount  = 0;   // cells feeding the estimate
    double                  mdAlpha       = 0.0; // CFAR scale factor

    std::vector<ComplexAcc> mvcDecimated;  // [range][block], pre-transform
    std::vector<ComplexAcc> mvcMap;        // [doppler][range], display layout
    std::vector<double>     mvdPower;      // |map|^2, same layout

    char                    machName[32];
    bool                    mbReady       = false;
};

bool ArdCpu::init(const ArdParams& oParams)
{
    CHECK(!mbReady, "ArdCpu::init called twice");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uSampleRateHz > 0, "sample rate must be non-zero");
    CHECK(oParams.uRangeBins > 0, "range bin count must be non-zero");
    CHECK(oParams.uDopplerDecimation > 0, "Doppler decimation must be non-zero");
    CHECK(oParams.uCpiSamples % oParams.uDopplerDecimation == 0,
          "the decimation factor must divide the CPI exactly, or the last "
          "block is short and the Doppler axis ends up subtly wrong");
    CHECK(oParams.uDopplerBins ==
              oParams.uCpiSamples / oParams.uDopplerDecimation,
          "uDopplerBins must equal uCpiSamples / uDopplerDecimation");
    CHECK(oParams.uDopplerBins % 2 == 0,
          "an odd Doppler bin count has no exact centre to put zero Doppler at");
    CHECK(oParams.uRangeBins <= oParams.uCpiSamples,
          "more range bins than samples in a CPI");
    CHECK(oParams.dCfarPfa > 0.0 && oParams.dCfarPfa < 1.0,
          "CFAR Pfa must be strictly between 0 and 1");

    // The CFAR window must fit inside the map, or every cell is a border cell
    // and the background estimate means nothing.
    const uint64_t uHalfRange   = oParams.uCfarGuardRange + oParams.uCfarTrainRange;
    const uint64_t uHalfDoppler = oParams.uCfarGuardDoppler + oParams.uCfarTrainDoppler;
    CHECK(2 * uHalfRange + 1 <= oParams.uRangeBins,
          "the CFAR window is wider than the map in range");
    CHECK(2 * uHalfDoppler + 1 <= oParams.uDopplerBins,
          "the CFAR window is taller than the map in Doppler");

    moParams = oParams;
    muCells  = oParams.uRangeBins * oParams.uDopplerBins;

    // Training cells: the whole window minus the guard block (which includes
    // the cell under test).
    const uint64_t uWindow = (2 * uHalfRange + 1) * (2 * uHalfDoppler + 1);
    const uint64_t uGuard  = (2 * oParams.uCfarGuardRange + 1) *
                             (2 * oParams.uCfarGuardDoppler + 1);
    muTrainCount = uWindow - uGuard;
    CHECK(muTrainCount > 0, "the CFAR window has no training cells");

    // The scale factor that turns "average of N training cells" into a
    // threshold achieving the requested false alarm rate, for exponentially
    // distributed noise power. Computed once, not per cell.
    const double dN = static_cast<double>(muTrainCount);
    mdAlpha = dN * (std::pow(oParams.dCfarPfa, -1.0 / dN) - 1.0);

    mvcDecimated.assign(static_cast<size_t>(oParams.uRangeBins * oParams.uDopplerBins),
                        ComplexAcc());
    mvcMap.assign(static_cast<size_t>(muCells), ComplexAcc());
    mvdPower.assign(static_cast<size_t>(muCells), 0.0);

    std::snprintf(machName, sizeof(machName), "ARD (CPU)");
    mbReady = true;
    return true;
}

void ArdCpu::buildMap(const ComplexF* pcRef, const ComplexF* pcCleaned)
{
    const uint64_t uRanges = moParams.uRangeBins;
    const uint64_t uBlocks = moParams.uDopplerBins;
    const uint64_t uDec    = moParams.uDopplerDecimation;

    // ---- steps 1 and 2: lag product, integrated and dumped ----------------
    for (uint64_t r = 0; r < uRanges; ++r) {
        for (uint64_t b = 0; b < uBlocks; ++b) {
            double dRe = 0.0;
            double dIm = 0.0;

            const uint64_t uFirst = b * uDec;
            for (uint64_t i = 0; i < uDec; ++i) {
                const uint64_t n = uFirst + i;
                // The reference is zero before the start of the CPI, so a
                // delay that reaches back past it contributes nothing.
                if (n < r)
                    continue;

                const ComplexF& cS = pcCleaned[n];
                const ComplexF& cR = pcRef[n - r];
                // cleaned * conj(delayed reference)
                const double dSr = cS.real(), dSi = cS.imag();
                const double dRr = cR.real(), dRi = cR.imag();
                dRe += dSr * dRr + dSi * dRi;
                dIm += dSi * dRr - dSr * dRi;
            }

            mvcDecimated[static_cast<size_t>(r * uBlocks + b)] = ComplexAcc(dRe, dIm);
        }
    }

    // ---- step 3: transform across blocks, per range bin -------------------
    // Direct DFT, and the fftshift and the transpose into display layout are
    // folded in by writing straight to the destination index.
    const double dTwoPiOverN = -2.0 * kPi / static_cast<double>(uBlocks);
    const uint64_t uHalf = uBlocks / 2;

    for (uint64_t r = 0; r < uRanges; ++r) {
        const ComplexAcc* pcRow = &mvcDecimated[static_cast<size_t>(r * uBlocks)];

        for (uint64_t k = 0; k < uBlocks; ++k) {
            double dRe = 0.0;
            double dIm = 0.0;
            for (uint64_t n = 0; n < uBlocks; ++n) {
                const double dAngle = dTwoPiOverN * static_cast<double>(k) *
                                      static_cast<double>(n);
                const double dCos = std::cos(dAngle);
                const double dSin = std::sin(dAngle);
                const ComplexAcc& c = pcRow[static_cast<size_t>(n)];
                dRe += c.real() * dCos - c.imag() * dSin;
                dIm += c.real() * dSin + c.imag() * dCos;
            }

            // fftshift: transform bin k belongs at display row
            // (k + N/2) mod N, so that zero Doppler lands in the middle.
            const uint64_t d = (k + uHalf) % uBlocks;
            mvcMap[static_cast<size_t>(d * uRanges + r)] = ComplexAcc(dRe, dIm);
        }
    }
}

bool ArdCpu::runCfar(Detection* pDetections, uint64_t uMaxDetections,
                     ArdStats& oStats)
{
    const uint64_t uRanges  = moParams.uRangeBins;
    const uint64_t uDoppler = moParams.uDopplerBins;

    const int64_t iGuardR = static_cast<int64_t>(moParams.uCfarGuardRange);
    const int64_t iGuardD = static_cast<int64_t>(moParams.uCfarGuardDoppler);
    const int64_t iHalfR  = iGuardR + static_cast<int64_t>(moParams.uCfarTrainRange);
    const int64_t iHalfD  = iGuardD + static_cast<int64_t>(moParams.uCfarTrainDoppler);

    // Peak, for the dB scale the map is reported on.
    double dPeakPower = 0.0;
    for (uint64_t u = 0; u < muCells; ++u) {
        if (mvdPower[static_cast<size_t>(u)] > dPeakPower)
            dPeakPower = mvdPower[static_cast<size_t>(u)];
    }
    oStats.dPeakDb = (dPeakPower > 0.0) ? 10.0 * std::log10(dPeakPower) : 0.0;

    const double dInvTrain = 1.0 / static_cast<double>(muTrainCount);

    for (uint64_t d = 0; d < uDoppler; ++d) {
        for (uint64_t r = 0; r < uRanges; ++r) {
            const double dCell = mvdPower[static_cast<size_t>(d * uRanges + r)];

            // Sum the training cells. Anything off the map counts as zero and
            // the divisor stays the full training count -- see the note at the
            // top of this file.
            double dSum = 0.0;
            for (int64_t dd = -iHalfD; dd <= iHalfD; ++dd) {
                const int64_t iD = static_cast<int64_t>(d) + dd;
                if (iD < 0 || iD >= static_cast<int64_t>(uDoppler))
                    continue;

                const bool bGuardD = (dd >= -iGuardD && dd <= iGuardD);

                for (int64_t dr = -iHalfR; dr <= iHalfR; ++dr) {
                    if (bGuardD && dr >= -iGuardR && dr <= iGuardR)
                        continue;   // guard block, including the cell itself

                    const int64_t iR = static_cast<int64_t>(r) + dr;
                    if (iR < 0 || iR >= static_cast<int64_t>(uRanges))
                        continue;

                    dSum += mvdPower[static_cast<size_t>(
                        static_cast<uint64_t>(iD) * uRanges +
                        static_cast<uint64_t>(iR))];
                }
            }

            const double dThreshold = mdAlpha * dSum * dInvTrain;
            if (!(dCell > dThreshold))
                continue;

            if (oStats.uDetections >= uMaxDetections) {
                ++oStats.uDetectionsDropped;
                continue;
            }

            Detection& oDet = pDetections[static_cast<size_t>(oStats.uDetections)];
            oDet.uRangeBin   = r;
            oDet.uDopplerBin = d;
            oDet.dRangeKm    = ardRangeKm(r, moParams.uSampleRateHz);
            oDet.dDopplerHz  = ardDopplerHz(d, moParams);
            oDet.dPowerDb    = (dPeakPower > 0.0)
                             ? 10.0 * std::log10(dCell / dPeakPower) : 0.0;
            oDet.dSnrDb      = (dThreshold > 0.0)
                             ? 10.0 * std::log10(dCell / dThreshold) : 0.0;
            ++oStats.uDetections;

            const bool bBorder =
                (static_cast<int64_t>(r) < iHalfR) ||
                (static_cast<int64_t>(r) >= static_cast<int64_t>(uRanges) - iHalfR) ||
                (static_cast<int64_t>(d) < iHalfD) ||
                (static_cast<int64_t>(d) >= static_cast<int64_t>(uDoppler) - iHalfD);
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

    buildMap(pcRef, pcCleaned);

    for (uint64_t u = 0; u < muCells; ++u) {
        const ComplexAcc& c = mvcMap[static_cast<size_t>(u)];
        mvdPower[static_cast<size_t>(u)] = c.real() * c.real() + c.imag() * c.imag();
        if (pcMap != nullptr) {
            pcMap[static_cast<size_t>(u)] =
                ComplexF(static_cast<float>(c.real()), static_cast<float>(c.imag()));
        }
    }

    return runCfar(pDetections, uMaxDetections, oStats);
}

bool ArdCpu::processDevice(const void* /*pdRef*/, const void* /*pdCleaned*/,
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
        const ComplexF& c = pcMap[static_cast<size_t>(u)];
        mvdPower[static_cast<size_t>(u)] =
            double(c.real()) * c.real() + double(c.imag()) * c.imag();
    }

    return runCfar(pDetections, uMaxDetections, oStats);
}

} // namespace

std::unique_ptr<ArdProcessor> makeArdProcessorCpu()
{
    return std::unique_ptr<ArdProcessor>(new ArdCpu());
}

} // namespace pbr
