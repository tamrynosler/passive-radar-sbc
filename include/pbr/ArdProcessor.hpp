#ifndef PBR_ARD_PROCESSOR_HPP
#define PBR_ARD_PROCESSOR_HPP

#include <cstdint>
#include <memory>

#include "pbr/Config.hpp"
#include "pbr/types.hpp"

namespace pbr {

// Range-Doppler map formation and detection: the stage after DSI cancellation.
//
// ---------------------------------------------------------------------------
// What it produces
// ---------------------------------------------------------------------------
//
// A picture. Two axes:
//
//   range    how much further the echo travelled than the direct signal,
//            measured as a delay. 1.46 km per bin at 204800 Hz.
//   Doppler  how much the echo's frequency shifted, which is how fast the
//            thing that reflected it is moving. 1 Hz per bin over a 1 s CPI.
//
// Every cell answers "how much energy is coming back from something at this
// distance moving at this speed". Mostly noise; where an aircraft is, one
// cell lights up.
//
// Built by the cross-ambiguity function (CAF): correlate the cleaned
// surveillance signal against the reference at every delay, then look at the
// frequency content of each correlation. Correlating over a whole second is
// what pulls an echo out of noise you could never see directly -- noise
// cancels itself, a real echo adds up.
//
// Then 2D cell-averaging CFAR turns the picture into a list of targets. It
// judges every cell against its own neighbourhood rather than a fixed
// threshold, so it adapts to a background that varies across the map and
// changes with time of day, weather and which station is being used.
//
// ---------------------------------------------------------------------------
// Map layout: the array IS the picture
// ---------------------------------------------------------------------------
//
//   map[uDopplerBin * uRangeBins + uRangeBin]
//
// Range is the x axis and varies fastest; Doppler is the y axis. That is the
// convention used throughout the literature this work is measured against
// (Schuepbach RadarConf 2020 Figs 3-7, Colone TAES 52(3) 2016 Figs 4-5,
// Laptev DSPA 2026 Fig 4) -- all plot bistatic range horizontally.
//
// The MATLAB reference chain stores the transpose of this and plots Doppler
// horizontally. It is the outlier, and `matlab/ard_golden.m` flips once on
// the way out so that nothing downstream has to think about it.
//
// Doppler bin 0 is the most negative Doppler, bin uDopplerBins/2 is zero
// Doppler, which is the `fftshift` the reference applies.
//
// References:
//   [1] Petri et al., TIWDC/ESAV 2011 -- the batches (decimation) CAF.
//   [2] Schuepbach, Paine, O'Hagan, RadarConf 2020 -- the chain this
//       reproduces on embedded GPU hardware, including its CA-CFAR settings.

enum class ArdBackend {
    // Plain C++14 reference. The Doppler transform is a direct DFT rather
    // than a fast one: 1600 is not a power of two, and an independent,
    // obviously correct algorithm is worth more here than speed. Slow on
    // purpose. FP32 throughout, so that a CPU-vs-GPU comparison measures the
    // backend rather than the precision.
    Cpu,
    // What the thesis measures. cuFFT plus custom kernels.
    Gpu
};

// How the correlation matrix that feeds the Doppler transform is built.
//
// BOTH PRODUCE THE SAME MAP. This is not an accuracy trade-off and not an
// approximation: with the transform long enough (see below) the Batches route
// computes exactly the same sums as Direct, in exact arithmetic, and the two
// differ only by floating-point rounding. Only the cost differs.
enum class ArdAlgorithm {
    // For each delay, multiply the cleaned signal by the delayed reference and
    // sum each block of uDopplerDecimation samples. One pass, no transform,
    // trivially parallel. Cost is uRangeBins * uCpiSamples multiply-adds.
    Direct,

    // The same correlation done one block at a time with a short FFT, so all
    // uRangeBins delays for a block come out of one transform instead of
    // uRangeBins separate sums (Petri et al., TIWDC/ESAV 2011).
    //
    // Circular correlation of length L gives
    //     Z[r] = sum_i X[i] conj(Y[(i - r) mod L]).
    // Packing a block starting at n0 = b * S (S = uDopplerDecimation) as
    //     X[i] = cleaned[n0 + i]      i in [0, S),     zero elsewhere
    //     Y[j] = ref[n0 + j]          j in [0, S)
    //     Y[j] = ref[n0 + j - L]      j in [L-K+1, L), zero elsewhere
    // makes Z[r] equal the Direct sum for every r in [0, K), K = uRangeBins.
    // The wrapped tail of Y supplies the K-1 reference samples from BEFORE
    // the block, which is exactly what the Direct form reaches back for; the
    // zeros in X mean only S surveillance samples contribute, so the
    // integration length -- and therefore the map -- is unchanged.
    //
    // That requires L >= S + K - 1, rounded up to a power of two. Derived,
    // not configured: any larger L gives the same answer and costs more.
    //
    // Batches wins when uRangeBins is large relative to log2(L). At the
    // shipped 120 bins and L = 256 it is roughly 4x fewer operations; at a
    // handful of range bins Direct would be cheaper.
    Batches
};

struct ArdParams {
    ArdBackend   eBackend         = ArdBackend::Gpu;
    ArdAlgorithm eAlgorithm       = ArdAlgorithm::Direct;

    uint64_t   uCpiSamples        = 0;   // 204800
    uint64_t   uSampleRateHz      = 0;   // 204800

    // Delay bins in the map. Need not equal the canceller's tap count,
    // though in the shipped config both are 120.
    uint64_t   uRangeBins         = 0;

    // Block length for the integrate-and-dump that precedes the Doppler
    // transform. Averaging blocks of this many samples low-passes away the
    // fast variation we do not want and shrinks the transform by the same
    // factor. Must divide uCpiSamples exactly.
    uint64_t   uDopplerDecimation = 0;   // 128

    // Derived: uCpiSamples / uDopplerDecimation. 1600 for the shipped config,
    // giving 1 Hz per bin over +-800 Hz.
    uint64_t   uDopplerBins       = 0;

    // Doppler extent worth displaying. Does not change what is computed.
    double     dDopplerHalfSpanHz = 0.0;

    // ---- CFAR ------------------------------------------------------------
    // Probability of false alarm. The threshold moves to whatever achieves
    // this, everywhere, rather than being fixed.
    double     dCfarPfa           = 0.0;

    // Guard cells are excluded from the background estimate. A strong echo
    // smears into its neighbours, and without a guard region the target
    // would raise its own threshold and hide itself.
    uint64_t   uCfarGuardRange    = 0;
    uint64_t   uCfarGuardDoppler  = 0;

    // Training cells form the background estimate. More of them means a
    // steadier estimate but less ability to follow a background that changes
    // quickly -- the same trade-off as ECA-B's batch length.
    uint64_t   uCfarTrainRange    = 0;
    uint64_t   uCfarTrainDoppler  = 0;
};

// One cell that beat its local threshold.
struct Detection {
    uint64_t uRangeBin     = 0;
    uint64_t uDopplerBin   = 0;
    double   dRangeKm      = 0.0;
    double   dDopplerHz    = 0.0;
    double   dPowerDb      = 0.0;   // amplitude dB relative to the map peak
    double   dSnrDb        = 0.0;   // how far above its own threshold, in dB
};

struct ArdStats {
    uint64_t uDetections        = 0;
    // Detections that did not fit in the caller's array. Non-zero means the
    // list is incomplete, which matters more than it sounds: a truncated list
    // looks exactly like a quiet scene.
    uint64_t uDetectionsDropped = 0;
    // Detections inside the border where the CFAR window runs off the edge of
    // the map. Those cells get an artificially low background estimate and so
    // over-detect. Reported rather than hidden -- see the note in ArdCpu.cpp.
    uint64_t uDetectionsInBorder = 0;
    double   dPeakDb            = 0.0;   // peak amplitude of the map, dB
};

class ArdProcessor {
public:
    virtual ~ArdProcessor() {}

    // Allocate and prepare. Every allocation, cuFFT plan and library handle is
    // created here and reused, so nothing allocates inside the timed path.
    virtual bool init(const ArdParams& oParams) = 0;

    // Build the map and detect, from host samples. Uploads and downloads
    // internally, so this is the standalone and validation path.
    //
    // pcRef and pcCleaned each hold uCpiSamples samples. pcMap, if not null,
    // receives uRangeBins * uDopplerBins complex cells in the layout above.
    // Detections are written to the caller's array up to uMaxDetections;
    // anything beyond that is counted in oStats.uDetectionsDropped.
    virtual bool process(const ComplexF* pcRef, const ComplexF* pcCleaned,
                         ComplexF* pcMap,
                         Detection* pDetections, uint64_t uMaxDetections,
                         ArdStats& oStats) = 0;

    // Same, but the samples are already on the device -- fed straight from a
    // GPU canceller's deviceReference() and deviceOutput(), with no copies.
    // Returns false on a backend that has no device.
    virtual bool processDevice(const void* pdRef, const void* pdCleaned,
                               ComplexF* pcMap,
                               Detection* pDetections, uint64_t uMaxDetections,
                               ArdStats& oStats) = 0;

    // Run only the detector, on a map the caller supplies. This is what makes
    // the CFAR testable in isolation: feed it MATLAB's golden map and any
    // disagreement is in the detector, not in the map that fed it.
    virtual bool detect(const ComplexF* pcMap,
                        Detection* pDetections, uint64_t uMaxDetections,
                        ArdStats& oStats) = 0;

    virtual const char* name() const = 0;
};

// Build a processor. Returns null, having logged the reason, for a backend
// that is not compiled in.
std::unique_ptr<ArdProcessor> makeArdProcessor(ArdBackend eBackend);

bool ardBackendFromString(const char* pName, ArdBackend& eBackend);
const char* ardBackendName(ArdBackend eBackend);

bool ardAlgorithmFromString(const char* pName, ArdAlgorithm& eAlgorithm);
const char* ardAlgorithmName(ArdAlgorithm eAlgorithm);

// Transform length for ArdAlgorithm::Batches: the smallest power of two that
// is at least uStride + uRangeBins - 1. Both backends and the tests derive it
// here so there is one definition of the rule. Returns 0 on bad arguments.
uint64_t ardBatchTransformLength(uint64_t uStride, uint64_t uRangeBins);

// Fill oParams from a loaded config.
bool ardParamsFromConfig(const PbrConfig& oConfig, ArdParams& oParams);

// ---- axes ------------------------------------------------------------------
// Bistatic range for a delay bin, in km: the extra distance the echo
// travelled compared with the direct signal.
double ardRangeKm(uint64_t uRangeBin, uint64_t uSampleRateHz);

// Doppler for a bin, in Hz. Bin uDopplerBins/2 is zero Doppler.
double ardDopplerHz(uint64_t uDopplerBin, const ArdParams& oParams);

} // namespace pbr

#endif // PBR_ARD_PROCESSOR_HPP
