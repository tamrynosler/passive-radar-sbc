#ifndef PBR_DSI_CANCELLER_HPP
#define PBR_DSI_CANCELLER_HPP

#include <cstdint>
#include <memory>

#include "pbr/Config.hpp"
#include "pbr/types.hpp"

namespace pbr {

// Direct-signal-interference cancellation: the interface every algorithm
// implements, so the benchmark can swap one for another on identical input.
//
// All of them do the same job. The surveillance channel is dominated by the
// direct signal from the transmitter and by echoes off stationary things.
// Every one of those is some delayed copy of the reference signal, so the
// clutter lives in the subspace spanned by delayed copies of the reference.
// Find the combination of those copies that best matches the surveillance
// channel, subtract it, and what remains should be the moving targets.
//
// What the algorithms disagree about is how the weights are estimated:
// over what stretch of signal, how often they are updated, in the time or
// the frequency domain, and by a direct solve or by iteration.
//
// References:
//   [1] Colone, O'Hagan, Lombardo, Baker, "A multistage processing algorithm
//       for disturbance removal and target detection in passive bistatic
//       radar", IEEE TAES 45(2), 2009.            -- ECA, ECA-B
//   [2] Colone, Palmarini, Martelli, Tilli, "Sliding extensive cancellation
//       algorithm for disturbance removal in passive radar", IEEE TAES
//       52(3), 2016.                              -- ECA-S, and the efficient
//                                                    implementation in its
//                                                    Section VI
//   [3] Schwark, Cristallini, "Advanced multipath clutter cancellation in
//       OFDM-based passive radar systems", IEEE RadarConf 2016.  -- ECA-CD
//   [4] Schuepbach, Paine, O'Hagan, "Efficient direct signal cancellation
//       for FM-based passive radar", IEEE RadarConf 2020.  -- the comparison
//       this thesis reproduces on embedded GPU hardware
//   [5] Ferrara, "Fast implementation of LMS adaptive filters", IEEE Trans.
//       ASSP 28(4), 1980.                         -- FBLMS
//   [6] Shynk, "Frequency-domain and multirate adaptive filtering", IEEE
//       Signal Processing Magazine 9(1), 1992.    -- the per-bin step
//                                                    normalisation (Table I)
//   [7] Lu, Song, Peng, Xiong, "A Levinson-Durbin-based clutter suppression
//       algorithm for passive radar", Digital Signal Processing 177, 2026.
//                                                 -- ECA-LD
//   [8] Golub, Van Loan, Matrix Computations, 4th ed., Sec. 4.7.
//                                                 -- the Levinson recursion

enum class DSIAlgorithm {
    // One least-squares solve over the whole CPI, with the absolute diagonal
    // loading the MATLAB reference chain uses. Defined as "whatever
    // matlab/fm_passive_radar_process.m does", so it is the golden-file
    // baseline rather than an algorithm choice in its own right.
    EcaM,

    // Batches [1]. The CPI is split into equal fragments and each is solved
    // independently, so the weights track clutter that changes during the
    // CPI. Costs Doppler ambiguities at 1/T_B spacing around low-Doppler
    // targets -- see the note on uUpdateSamples below.
    EcaB,

    // Sliding [2]. Weights estimated over a long window centred on a short
    // fragment, applied to that fragment only, then slid on. Decouples the
    // estimation quality from the update rate and pushes the ambiguities
    // out of the Doppler span of interest. At the ends of the CPI the
    // estimation window is clamped inside it -- see dsiEstimationStart().
    // Same solve as ECA-B, so the same code; only the window bounds differ.
    EcaS,

    // By carrier and Doppler [3]. Works on batched FFTs rather than delayed
    // copies, because a delay in time is a phase shift in frequency. The
    // subspace shrinks from K(2P+1) columns to (2P+1), which is where its
    // speed comes from. Uses its own batch length and Doppler tap count,
    // uCdBatchSamples and uCdDopplerTaps; ignores the ECA window fields.
    EcaCd,

    // Conjugate gradient least squares. The same per-window least-squares
    // problem as ECA-B, but stepped towards iteratively instead of solved
    // directly, so a fixed iteration count buys a fixed execution time and a
    // fixed memory footprint [4]. Uses its own window, uCglsBatchSamples, and
    // uMaxIterations; ignores the ECA window fields and diagonal loading
    // (stopping early is the regularisation).
    Cgls,

    // Fast block LMS [5][6]. Adaptive and frequency-domain: the weights are
    // updated every block of uFblmsTaps samples, with the convolution and the
    // gradient done by 2N-point FFTs. Stateful across CPIs: the weights, the
    // power estimate and the last N reference samples carry over. Uses its
    // own fields uFblmsTaps, dFblmsStep and dFblmsForget; ignores every
    // ECA and CGLS field, including uClutterTaps.
    Fblms,

    // ECA-B's problem solved per batch on the Toeplitz approximation of the
    // Gram matrix, by FFT correlations and the Levinson recursion [7][8]: no
    // K x K Gram is formed and the solve is O(K^2), not O(K^3). Uses
    // uClutterTaps, dDiagonalLoading (relative, as ECA-B) and its own window
    // uEcaLdBatchSamples; ignores the ECA estimate/update windows,
    // uDopplerTaps and every CGLS and FBLMS field. Stateless.
    EcaLd
};

// Which implementation of the chosen algorithm to build.
//
// Both exist on purpose and are expected to produce the same answer: the
// CPU version is the correctness reference and the GPU version is what the
// thesis measures. Keeping them separately selectable is what makes the
// comparison possible.
//
// The CPU implementations are plain C++14 and build on the Mac. The GPU
// implementations are only compiled when the build defines PBR_WITH_CUDA;
// without it, asking for Gpu fails at the factory with a clear message
// rather than silently falling back to the CPU and quietly invalidating a
// benchmark.
enum class DSIBackend { Cpu, Gpu };

// Everything an algorithm needs to know, filled from PbrConfig by
// dsiParamsFromConfig(). No canceller reads the config file itself.
//
// Fields that do not apply to the chosen algorithm are ignored, not
// validated -- one struct serves all seven, the same way one .conf file
// serves tools that read different subsets of it.
struct DSIParams {
    DSIAlgorithm eAlgorithm     = DSIAlgorithm::EcaM;
    DSIBackend   eBackend       = DSIBackend::Cpu;

    // Samples per CPI. Both input buffers and the output buffer are this
    // long, and the canceller is allocated for exactly this size.
    uint64_t     uCpiSamples    = 0;

    // K: how many delay taps of clutter to cancel, so how far out in
    // bistatic range the cancellation extends: c / fs per tap, so 120 taps at
    // 200 kHz is about 180 km.
    uint64_t     uClutterTaps   = 0;

    // N_A in [2]: the stretch of signal the weights are ESTIMATED from.
    // Longer means a better-conditioned, less noisy estimate; shorter means
    // the weights track changing clutter more closely. Colone's FM results
    // put the optimum around 25 ms.
    uint64_t     uEstimateSamples = 0;

    // N_S in [2]: the stretch of signal those weights are APPLIED to, and
    // therefore also the interval between weight updates.
    //
    // This is the parameter that separates the three time-domain variants:
    //
    //   ECA-m   uEstimateSamples == uUpdateSamples == uCpiSamples
    //   ECA-B   uEstimateSamples == uUpdateSamples <  uCpiSamples
    //   ECA-S   uEstimateSamples >  uUpdateSamples
    //
    // It also sets where ECA-B's Doppler ambiguities land. Independent
    // per-fragment weights are themselves a modulation, which puts false
    // peaks at 1/T_S spacing around any target inside the cancellation
    // notch ([2] eq. 17). They are a property of the algorithm, not a bug:
    // at 25 ms they sit every 40 Hz, inside our +-200 Hz Doppler span. ECA-S
    // exists to move them outside it.
    uint64_t     uUpdateSamples = 0;

    // P: half the number of Doppler bins in the clutter subspace, which
    // becomes K(2P+1) columns wide. Real clutter is not perfectly
    // stationary -- trees sway, sea moves -- so it sits at a small non-zero
    // Doppler that P > 0 can reach.
    //
    // Only P = 0 is implemented. The MATLAB reference is P = 0 and the
    // field is here so the config file does not have to change later;
    // init() REJECTS a non-zero value rather than ignoring it.
    uint64_t     uDopplerTaps   = 0;

    // Diagonal loading, added to the Gram matrix before the solve to keep
    // it well conditioned. Expressed as a FRACTION of the mean Gram
    // diagonal, so it means the same thing whatever amplitude the samples
    // arrive at. An absolute value silently changes strength with the
    // signal level, which is how the same number can be meaningful
    // regularisation on one scaling and nothing at all on another.
    double       dDiagonalLoading = 0.0;

    // ECA-m only: interpret dDiagonalLoading as an absolute value rather
    // than a fraction, reproducing the MATLAB reference's 1e-3 exactly.
    // Set by dsiParamsFromConfig() for EcaM and by nothing else.
    bool         bAbsoluteLoading = false;

    // Work in single precision rather than double.
    //
    // The GPU backend has no choice: the Jetson Nano's Maxwell GPU runs
    // double-precision arithmetic at 1/32 the single-precision rate, which
    // would make it slower than the CPU. It therefore ignores this flag and
    // always works in single precision.
    //
    // On the CPU the flag rounds the heavy arithmetic the way the GPU does:
    // the Gram sums and the loading for ECA (its small solve stays in
    // double), every step for CGLS and ECA-CD. Comparing the two then
    // isolates structural bugs -- a wrong index, a race -- from the precision
    // difference, which would otherwise mask them. Measured cost of single
    // precision against the MATLAB reference: ~5e-3 relative L2, with the
    // suppression figure unchanged to four decimals
    // (docs/design_decisions.md 1d.4).
    bool         bFloat32Math = false;

    // ECA-CD: samples per batch (a power of two that divides the CPI), and
    // Doppler columns either side of zero in the per-carrier subspace, so
    // the projection removes 2P+1 columns per carrier. At most
    // kMaxCdDopplerTaps.
    uint64_t     uCdBatchSamples = 0;
    uint64_t     uCdDopplerTaps  = 0;

    // CGLS window: weights are estimated from and applied to fragments of
    // this many samples, as ECA-B does with uUpdateSamples. Its own field so
    // the two algorithms can be tuned apart; must divide the CPI exactly.
    uint64_t     uCglsBatchSamples = 0;

    // CGLS. Iterating a fixed number of times rather than to a tolerance is
    // what gives a fixed, predictable execution time, and it is also the
    // regularisation: the iteration fits the strong, well-conditioned
    // clutter first and the noise last, so more is not better.
    uint64_t     uMaxIterations = 0;

    // CGLS: start each window from the weights the same window reached in
    // the previous CPI instead of from zero. Makes the canceller stateful.
    bool         bCglsPrime     = false;

    // FBLMS. N: filter length AND block length; a power of two that divides
    // the CPI. The FFTs are 2N points. Also the reach in range: c/fs per tap.
    uint64_t     uFblmsTaps     = 0;

    // FBLMS. mu, Shynk Table I, applied per bin as mu / P_m. In (0, 1]. Sets
    // convergence speed and the width of the zero-Doppler notch.
    double       dFblmsStep     = 0.0;

    // FBLMS. lambda of the per-bin power estimate
    // P_m(k) = lambda P_m(k-1) + (1 - lambda) |X_m(k)|^2. In [0, 1). Its
    // memory is 1/(1-lambda) blocks.
    double       dFblmsForget   = 0.0;

    // ECA-LD window: weights estimated from and applied to batches of this
    // length, as ECA-B does with uUpdateSamples. Its own field so the two can
    // be tuned apart; must divide the CPI and exceed uClutterTaps.
    uint64_t     uEcaLdBatchSamples = 0;
};

// What an algorithm reports about the work it just did. Intrinsic
// properties only -- timing belongs to the benchmark harness, which knows
// how to warm up, repeat and take a minimum. A canceller timing itself
// would measure the wrong thing.
struct DSIStats {
    // 10*log10(input power / output power) over the CPI. The headline
    // suppression figure, and on its own a misleading one: an algorithm can
    // score well here while also cancelling the targets. Target attenuation
    // has to be measured separately, from the range-Doppler map.
    double   dSuppressionDb = 0.0;

    // Iterations actually run. Zero for the direct methods.
    uint64_t uIterations    = 0;
};

class DSICanceller {
public:
    virtual ~DSICanceller() {}

    // Allocate and prepare for CPIs of oParams.uCpiSamples samples.
    //
    // EVERY allocation, plan and library handle is created here and reused
    // for the life of the object. Nothing allocates inside cancel(), or the
    // benchmark measures the allocator instead of the algorithm.
    //
    // Returns false, having logged the reason, on a parameter that does not
    // make sense or a failed allocation. Call it once; calling it again is
    // not supported.
    virtual bool init(const DSIParams& oParams) = 0;

    // Cancel one CPI. pcRef and pcSur hold uCpiSamples aligned samples
    // covering the same absolute index range -- exactly what
    // ChannelIngest::tryExtractCpi() produces. pcOut receives the same
    // number of samples.
    //
    // All three buffers are caller-owned. pcOut must not alias the inputs:
    // the original surveillance signal is still needed at the end to
    // measure suppression, and the benchmark re-runs several algorithms on
    // the same untouched input.
    //
    // Returns false on a bad argument or a library failure.
    virtual bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                        ComplexF* pcOut, DSIStats& oStats) = 0;

    // Name for logs, CSV rows and figure captions, e.g. "ECA-B (CPU)".
    virtual const char* name() const = 0;

    // Device pointers to the reference and to the cleaned surveillance
    // signal, valid after a successful cancel(). Null on a backend with no
    // device, which is how a caller tells whether it can chain without
    // copying.
    //
    // Deliberately opaque void*: the next stage casts them back, and this
    // header stays free of CUDA types. The point is that a GPU canceller
    // feeding a GPU range-Doppler stage moves no samples through the host at
    // all -- the download below exists only for validation and for the CPU
    // path.
    virtual const void* deviceReference() const { return nullptr; }
    virtual const void* deviceOutput()    const { return nullptr; }

    // Skip the final device-to-host copy of the cleaned signal. Set this when
    // the next stage reads deviceOutput() directly; the host output buffer is
    // then left untouched. Ignored by backends with no device.
    virtual void setHostOutputEnabled(bool /*bEnabled*/) {}

    // Forget everything learnt from earlier cancel() calls, back to the state
    // init() left: what a stateful canceller (FBLMS) needs after the chain's
    // warm-up pass, whose synthetic input must not seed the first real CPI.
    // Stateless cancellers have nothing to forget. False on a device failure.
    virtual bool resetState() { return true; }
};

// Build a canceller. Returns null, having logged the reason, for a
// combination that is not implemented or not compiled in.
//
// The caller owns the result. Concrete classes are deliberately not
// exposed: nothing outside needs the type, and hiding them keeps each
// implementation free to change without touching a public header.
std::unique_ptr<DSICanceller> makeDSICanceller(DSIAlgorithm eAlgorithm,
                                               DSIBackend   eBackend);

// Parse the DSI_ALGORITHM / DSI_BACKEND config values. Both return false,
// having logged the offending string, on anything unrecognised.
bool dsiAlgorithmFromString(const char* pName, DSIAlgorithm& eAlgorithm);
bool dsiBackendFromString(const char* pName, DSIBackend& eBackend);

// Name of an algorithm or backend, in the same spelling the config file
// uses. Never null.
const char* dsiAlgorithmName(DSIAlgorithm eAlgorithm);
const char* dsiBackendName(DSIBackend eBackend);

// The per-carrier solve in ECA-CD is sized for 2P+1 <= 7 at compile time.
const uint64_t kMaxCdDopplerTaps = 3;

// Fill oParams from a loaded config.
//
// ECA-m forces the estimation and update windows to the whole CPI and
// switches on absolute diagonal loading; the other algorithms take theirs
// from the file. The window rules themselves are checked by the config
// loader and again by each canceller's init(). Returns false, having logged
// the reason, on an unknown algorithm or backend name.
bool dsiParamsFromConfig(const PbrConfig& oConfig, DSIParams& oParams);

// Where the estimation window for one update window starts.
//
// ECA-S estimates the weights for the update window beginning at
// uUpdateStart from a longer window centred on it. Two rules:
//
//   * CLAMPED at the ends of the CPI, where a centred window would run off
//     the edge. The first and last few update windows therefore share an
//     estimation window that is off-centre rather than short, so every
//     window is solved from exactly uEstimateSamples samples and
//     conditioning and diagonal loading mean the same thing throughout.
//
//   * QUANTISED to a whole number of update windows. The GPU shares one
//     partial Gram per update-window-length chunk between the M =
//     T_A / T_S estimation windows that span it, which is what makes ECA-S
//     affordable (see docs/design_decisions.md 1d.9), and that sharing is
//     only possible if every estimation window starts on a chunk boundary.
//     When M is even there is no boundary at the centre of the update
//     window, so the estimation window sits T_S/2 off-centre -- 5% of T_A
//     at the shipped settings. Deliberate; M odd centres exactly.
//
// Requires uUpdateSamples to divide both uEstimateSamples and uCpiSamples;
// init() and the config loader check both. With uEstimateSamples ==
// uUpdateSamples it returns uUpdateStart unchanged, so ECA-m and ECA-B are
// untouched. Both backends call this, so they cannot disagree.
uint64_t dsiEstimationStart(uint64_t uUpdateStart, uint64_t uUpdateSamples,
                            uint64_t uEstimateSamples, uint64_t uCpiSamples);

} // namespace pbr

#endif // PBR_DSI_CANCELLER_HPP
