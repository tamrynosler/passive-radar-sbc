#ifndef PBR_CONFIG_HPP
#define PBR_CONFIG_HPP

#include <cstdint>
#include <string>

namespace pbr {

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

    // ---- ingest ----------------------------------------------------------
    uint16_t    uUdpPort           = 0;
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

} // namespace pbr

#endif // PBR_CONFIG_HPP
