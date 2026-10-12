#ifndef PBR_CONFIG_HPP
#define PBR_CONFIG_HPP

#include <cstdint>
#include <string>

namespace pbr {

// The config every tool reads when not given --config: the USRP setup.
// config/fers.conf is the FERS scenario.
const char* const kDefaultConfigPath = "config/prb.conf";

// Parameters for the whole processing chain, loaded from a .conf file.
//
// The file format is the legacy Commensal Radar ProcServer convention:
// KEY = VALUE, one per line, '#' to end of line for comments, whitespace
// stripped. See config/prb.conf.
//
// Two policies matter more than the parsing:
//
//   * A key the program needs but cannot find is a FATAL startup error,
//     naming the key. Never a silent default -- a silent default is how you
//     end up with every sample scaled wrongly and no indication of it.
//   * A key the file contains but this struct does not know is IGNORED, so
//     one file can serve tools that read different subsets of it.
//
// Derived values are computed here rather than stored in the file, so they
// cannot drift out of step with what they are derived from.
struct PbrConfig {
    // ---- scenario: must match the .fersxml -------------------------------
    uint64_t    uSampleRateHz      = 0;
    std::string sReferenceName;
    std::string sSurveillanceName;
    std::string sSurveillance2Name;         // the second surveillance antenna;
                                            // "none" = a two-channel chain

    // ---- the two-element surveillance array (docs/third-channel-aoa-design.md)
    double      dCarrierFreqHz      = 0.0;  // sets the wavelength
    double      dArraySpacingM      = 0.0;  // element 2 from element 1
    double      dArrayBaselineAzDeg = 0.0;  // compass bearing, element 1 to 2;
                                            // the array looks 90 deg clockwise of it
    double      dPhaseCalDeg        = 0.0;  // added to the measured phase difference

    // ---- ingest ----------------------------------------------------------
    uint16_t    uUdpPort           = 0;
    uint16_t    uControlPort       = 0;     // live-change HTTP port; 0 = off
    double      dFullScale         = 0.0;   // must match --vita49-fullscale
    double      dBufferSeconds     = 0.0;

    // ---- processing ------------------------------------------------------
    double      dCpiSeconds        = 0.0;
    double      dHopSeconds        = 0.0;
    uint64_t    uRangeBins         = 0;
    uint64_t    uClutterTaps       = 0;
    double      dDopplerHalfSpanHz = 0.0;
    uint64_t    uDopplerDecimation = 0;
    double      dMinCpiCoverage    = 0.0;

    // ---- range-Doppler map formation -------------------------------------
    // How the CAF is computed. Both routes produce the SAME map; see
    // ArdProcessor.hpp. A string here for the same reason the DSI names are:
    // an ingest-only tool never has to know the algorithm names.
    std::string sArdAlgorithm;              // Direct or Batches

    // ---- DSI cancellation ------------------------------------------------
    // Kept as strings here and turned into enums by dsiAlgorithmFromString /
    // dsiBackendFromString, so this struct stays free of DSICanceller.hpp and
    // an ingest-only tool never has to know the algorithm names.
    std::string sDsiAlgorithm;              // ECA-m, ECA-B, ECA-S, ECA-CD, CGLS, FBLMS, ECA-LD
    std::string sDsiBackend;                // CPU or GPU
    std::string sDsiMathPrecision;          // FP64 or FP32 (CPU only; GPU is
                                            // always FP32 -- see DSICanceller.hpp)
    double      dEcaBatchSeconds   = 0.0;   // T_A, the weight-estimation window
    double      dEcaUpdateSeconds  = 0.0;   // T_S, the weight-application window
    uint64_t    uEcaDopplerTaps    = 0;     // P; subspace is K(2P+1) wide
    double      dEcaDiagLoading    = 0.0;   // fraction of the mean Gram diagonal
    double      dCdBatchSeconds    = 0.0;   // ECA-CD batch length
    uint64_t    uCdDopplerTaps     = 0;     // ECA-CD P; subspace is 2P+1 wide
    uint64_t    uArdSnapshotEvery  = 0;     // 1 = every CPI
    // Where each serialised ARD frame goes (docs/ard-stream-design.md): to
    // DIR/ard/ on disk, to the UDP destination, both, or neither.
    bool        bArdWriteFiles     = false;
    bool        bArdStream         = false;
    std::string sArdStreamHost;             // ARD_STREAM_DEST host part
    uint16_t    uArdStreamPort     = 0;     // ARD_STREAM_DEST port part
    uint64_t    uCglsIterations    = 0;
    double      dCglsBatchSeconds  = 0.0;   // CGLS window, independent of ECA's
    bool        bCglsPrime         = false; // start from the previous CPI's weights
    bool        bEcaGramRecurrence = true;  // GPU ECA: Gram from its first column, not cherk
    uint64_t    uFblmsTaps         = 0;     // FBLMS filter and block length, samples
    double      dFblmsStep         = 0.0;   // FBLMS mu
    double      dFblmsForget       = 0.0;   // FBLMS lambda
    double      dEcaLdBatchSeconds = 0.0;   // ECA-LD window, independent of ECA's

    // ---- detection -------------------------------------------------------
    double      dCfarPfa           = 0.0;
    uint64_t    uCfarGuardRange    = 0;
    uint64_t    uCfarGuardDoppler  = 0;
    uint64_t    uCfarTrainRange    = 0;
    uint64_t    uCfarTrainDoppler  = 0;

    // ---- derived at load time, never present in the file -----------------
    uint64_t    uCpiSamples        = 0;   // round(dCpiSeconds * fs)
    uint64_t    uHopSamples        = 0;   // round(dHopSeconds * fs)
    uint64_t    uBufferCapacity    = 0;   // round(dBufferSeconds * fs)
    uint64_t    uDopplerBins       = 0;   // uCpiSamples / uDopplerDecimation
    uint64_t    uEcaEstimateSamples= 0;   // round(dEcaBatchSeconds * fs)
    uint64_t    uEcaUpdateSamples  = 0;   // round(dEcaUpdateSeconds * fs)
    uint64_t    uCdBatchSamples    = 0;   // round(dCdBatchSeconds * fs)
    uint64_t    uCglsBatchSamples  = 0;   // round(dCglsBatchSeconds * fs)
    uint64_t    uEcaLdBatchSamples = 0;   // round(dEcaLdBatchSeconds * fs)
    bool        bHaveSurveillance2 = false; // sSurveillance2Name is not "none"
};

// Load and validate a config file.
//
// Returns false, having logged the reason to stderr, if the file cannot be
// read, a line is malformed, a key is duplicated, a required key is missing,
// or a value fails validation. On success oConfig is fully populated,
// including the derived fields, and needs no further checking by callers.
bool loadPbrConfig(const char* pPath, PbrConfig& oConfig);

// Print the loaded configuration, so a run's log records exactly what it ran
// with.
void printPbrConfig(const PbrConfig& oConfig);

// The same loader on text instead of a file. pMessages, if given, collects
// every message the loader printed, so a caller can pass the reason on.
bool loadPbrConfigFromText(const std::string& sText, PbrConfig& oConfig,
                           std::string* pMessages = nullptr);

// Every key as KEY = VALUE lines, one per line, in a form the loader reads
// back to the same values. The live parameter set in the file's own syntax.
std::string pbrConfigToText(const PbrConfig& oConfig);

// A live change: KEY = VALUE lines laid over oCurrent and validated as a
// whole. On success oOut is the new, fully derived config; on failure it is
// untouched and sMessagesOut says why (an unknown key, a bad value, a
// combination the chain cannot run). Nothing is written to disk.
bool applyPbrConfigLines(const PbrConfig& oCurrent, const std::string& sLines,
                         PbrConfig& oOut, std::string& sMessagesOut);

// The first key in sLines that cannot change while the chain runs (ports,
// receiver names, full scale, buffer length, backend, output switches) and
// that sLines sets to something other than its value in oCurrent; empty if
// there is none. The chain refuses such a change by name, before validation,
// so the answer says why rather than whatever the validator trips on.
std::string restartKeyChanged(const PbrConfig& oCurrent, const std::string& sLines);

// The keys whose values differ between two sets, as "KEY: old -> new" lines
// in key order. Empty if the sets are the same.
std::string pbrConfigDiff(const PbrConfig& oOld, const PbrConfig& oNew);

// The source declares a sample rate the config did not expect. Re-express
// every time-based window at the source's rate, each snapped to the nearest
// value the chain allows, and the CPI to the nearest length they all divide,
// then validate. sReportOut lists every value that moved, as the warning the
// chain prints. False, with the reason in the report, if no valid set exists.
bool adaptPbrConfigToSampleRate(const PbrConfig& oCurrent, uint64_t uSourceRateHz,
                                PbrConfig& oOut, std::string& sReportOut);

} // namespace pbr

#endif // PBR_CONFIG_HPP
