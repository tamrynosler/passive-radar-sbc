#include "pbr/Config.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace pbr {

namespace {

std::string trim(const std::string& s)
{
    const char* pWhitespace = " \t\r\n";
    const size_t uFirst = s.find_first_not_of(pWhitespace);
    if (uFirst == std::string::npos)
        return std::string();
    const size_t uLast = s.find_last_not_of(pWhitespace);
    return s.substr(uFirst, uLast - uFirst + 1);
}

typedef std::map<std::string, std::string> KeyValueMap;

// Read the file into key/value pairs. Rejects malformed lines and duplicate
// keys rather than guessing what was meant.
bool readKeyValues(const char* pPath, KeyValueMap& mValues)
{
    std::FILE* pFile = std::fopen(pPath, "r");
    if (pFile == nullptr) {
        std::fprintf(stderr, "config: cannot open %s: %s\n", pPath, std::strerror(errno));
        return false;
    }

    char achLine[1024];
    int  iLineNo = 0;
    bool bOk     = true;

    while (std::fgets(achLine, sizeof(achLine), pFile) != nullptr) {
        ++iLineNo;

        std::string sLine(achLine);

        // '#' starts a comment, to end of line.
        const size_t uHash = sLine.find('#');
        if (uHash != std::string::npos)
            sLine = sLine.substr(0, uHash);

        sLine = trim(sLine);
        if (sLine.empty())
            continue;

        const size_t uEquals = sLine.find('=');
        if (uEquals == std::string::npos) {
            std::fprintf(stderr, "config: %s:%d has no '=': \"%s\"\n",
                         pPath, iLineNo, sLine.c_str());
            bOk = false;
            continue;
        }

        const std::string sKey   = trim(sLine.substr(0, uEquals));
        const std::string sValue = trim(sLine.substr(uEquals + 1));

        if (sKey.empty()) {
            std::fprintf(stderr, "config: %s:%d has an empty key\n", pPath, iLineNo);
            bOk = false;
            continue;
        }
        if (sValue.empty()) {
            std::fprintf(stderr, "config: %s:%d key %s has an empty value\n",
                         pPath, iLineNo, sKey.c_str());
            bOk = false;
            continue;
        }
        if (mValues.find(sKey) != mValues.end()) {
            std::fprintf(stderr, "config: %s:%d key %s is set more than once\n",
                         pPath, iLineNo, sKey.c_str());
            bOk = false;
            continue;
        }

        mValues[sKey] = sValue;
    }

    std::fclose(pFile);
    return bOk;
}

// Each getter reports a missing or unparseable key and clears bOk, so a single
// load reports every problem at once rather than one per run.
void getString(const KeyValueMap& mValues, const char* pKey,
               std::string& sOut, bool& bOk)
{
    const KeyValueMap::const_iterator it = mValues.find(pKey);
    if (it == mValues.end()) {
        std::fprintf(stderr, "config: required key %s is missing\n", pKey);
        bOk = false;
        return;
    }
    sOut = it->second;
}

void getDouble(const KeyValueMap& mValues, const char* pKey,
               double& dOut, bool& bOk)
{
    std::string sValue;
    bool bFound = true;
    getString(mValues, pKey, sValue, bFound);
    if (!bFound) {
        bOk = false;
        return;
    }

    char*        pEnd   = nullptr;
    const double dValue = std::strtod(sValue.c_str(), &pEnd);
    if (pEnd == sValue.c_str() || *pEnd != '\0') {
        std::fprintf(stderr, "config: key %s value \"%s\" is not a number\n",
                     pKey, sValue.c_str());
        bOk = false;
        return;
    }
    dOut = dValue;
}

void getUInt64(const KeyValueMap& mValues, const char* pKey,
               uint64_t& uOut, bool& bOk)
{
    std::string sValue;
    bool bFound = true;
    getString(mValues, pKey, sValue, bFound);
    if (!bFound) {
        bOk = false;
        return;
    }

    char* pEnd = nullptr;
    errno = 0;
    const unsigned long long ullValue = std::strtoull(sValue.c_str(), &pEnd, 10);
    if (pEnd == sValue.c_str() || *pEnd != '\0' || errno == ERANGE ||
        sValue[0] == '-') {
        std::fprintf(stderr,
                     "config: key %s value \"%s\" is not a non-negative integer\n",
                     pKey, sValue.c_str());
        bOk = false;
        return;
    }
    uOut = static_cast<uint64_t>(ullValue);
}

// Round a duration in seconds to a whole number of samples.
uint64_t toSamples(double dSeconds, uint64_t uSampleRateHz)
{
    return static_cast<uint64_t>(dSeconds * static_cast<double>(uSampleRateHz) + 0.5);
}

bool validate(PbrConfig& oConfig)
{
    bool bOk = true;

    const struct { const char* pName; double dValue; } aPositive[] = {
        { "SAMPLE_RATE_Hz",       static_cast<double>(oConfig.uSampleRateHz) },
        { "VITA49_FULLSCALE",     oConfig.dFullScale },
        { "BUFFER_LENGTH_s",      oConfig.dBufferSeconds },
        { "CPI_LENGTH_s",         oConfig.dCpiSeconds },
        { "CPI_HOP_s",            oConfig.dHopSeconds },
        { "N_RANGE_BINS",         static_cast<double>(oConfig.uRangeBins) },
        { "N_CLUTTER_TAPS",       static_cast<double>(oConfig.uClutterTaps) },
        { "DOPPLER_HALF_SPAN_Hz", oConfig.dDopplerHalfSpanHz },
        { "DOPPLER_DECIMATION",   static_cast<double>(oConfig.uDopplerDecimation) },
        { "ECA_BATCH_LENGTH_s",   oConfig.dEcaBatchSeconds },
        { "ECA_UPDATE_INTERVAL_s", oConfig.dEcaUpdateSeconds },
    };
    for (size_t u = 0; u < sizeof(aPositive) / sizeof(aPositive[0]); ++u) {
        if (aPositive[u].dValue <= 0.0) {
            std::fprintf(stderr, "config: %s must be greater than zero\n",
                         aPositive[u].pName);
            bOk = false;
        }
    }

    if (oConfig.uUdpPort == 0) {
        std::fprintf(stderr, "config: UDP_PORT must be between 1 and 65535\n");
        bOk = false;
    }
    if (oConfig.sReferenceName.empty() || oConfig.sSurveillanceName.empty()) {
        std::fprintf(stderr, "config: receiver names must not be empty\n");
        bOk = false;
    }
    if (oConfig.sReferenceName == oConfig.sSurveillanceName) {
        std::fprintf(stderr, "config: reference and surveillance receiver names "
                             "are identical (%s)\n", oConfig.sReferenceName.c_str());
        bOk = false;
    }
    if (oConfig.dCfarPfa <= 0.0 || oConfig.dCfarPfa >= 1.0) {
        std::fprintf(stderr, "config: CFAR_PFA must be strictly between 0 and 1\n");
        bOk = false;
    }
    if (oConfig.dMinCpiCoverage <= 0.0 || oConfig.dMinCpiCoverage > 1.0) {
        std::fprintf(stderr, "config: MIN_CPI_COVERAGE must be in (0, 1]\n");
        bOk = false;
    }

    // The ECA formulation needs at least as many delay taps as range bins.
    if (oConfig.uClutterTaps < oConfig.uRangeBins) {
        std::fprintf(stderr,
                     "config: N_CLUTTER_TAPS (%llu) must be at least N_RANGE_BINS (%llu)\n",
                     static_cast<unsigned long long>(oConfig.uClutterTaps),
                     static_cast<unsigned long long>(oConfig.uRangeBins));
        bOk = false;
    }

    if (oConfig.sDsiAlgorithm.empty() || oConfig.sDsiBackend.empty()) {
        std::fprintf(stderr, "config: DSI_ALGORITHM and DSI_BACKEND must not be empty\n");
        bOk = false;
    }
    // Loading is a fraction of the Gram diagonal, so it is dimensionless and
    // a value of 1 or more would swamp the signal entirely.
    if (oConfig.dEcaDiagLoading < 0.0 || oConfig.dEcaDiagLoading >= 1.0) {
        std::fprintf(stderr, "config: ECA_DIAGONAL_LOADING must be in [0, 1)\n");
        bOk = false;
    }

    if (!bOk)
        return false;

    // ---- derived values ---------------------------------------------------
    oConfig.uCpiSamples     = toSamples(oConfig.dCpiSeconds,    oConfig.uSampleRateHz);
    oConfig.uHopSamples     = toSamples(oConfig.dHopSeconds,    oConfig.uSampleRateHz);
    oConfig.uBufferCapacity = toSamples(oConfig.dBufferSeconds, oConfig.uSampleRateHz);

    if (oConfig.uCpiSamples == 0 || oConfig.uHopSamples == 0) {
        std::fprintf(stderr, "config: CPI_LENGTH_s and CPI_HOP_s must each be at "
                             "least one sample at %llu Hz\n",
                     static_cast<unsigned long long>(oConfig.uSampleRateHz));
        return false;
    }

    // A decimation factor that does not divide the CPI evenly leaves a
    // remainder and a subtly wrong Doppler axis.
    if (oConfig.uCpiSamples % oConfig.uDopplerDecimation != 0) {
        std::fprintf(stderr,
                     "config: DOPPLER_DECIMATION (%llu) does not divide the %llu "
                     "samples per CPI exactly\n",
                     static_cast<unsigned long long>(oConfig.uDopplerDecimation),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    oConfig.uDopplerBins = oConfig.uCpiSamples / oConfig.uDopplerDecimation;

    // The buffer has to hold a whole CPI, or extraction can never succeed.
    if (oConfig.uBufferCapacity < oConfig.uCpiSamples) {
        std::fprintf(stderr,
                     "config: BUFFER_LENGTH_s holds %llu samples, less than the "
                     "%llu samples in one CPI\n",
                     static_cast<unsigned long long>(oConfig.uBufferCapacity),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    // ECA windows. Colone et al. (TAES 52(3), 2016) divide the CPI into a
    // whole number of batches "without remainder": a partial trailing window
    // would be solved from fewer samples than the rest and quietly behave
    // differently from its neighbours.
    oConfig.uEcaEstimateSamples = toSamples(oConfig.dEcaBatchSeconds,  oConfig.uSampleRateHz);
    oConfig.uEcaUpdateSamples   = toSamples(oConfig.dEcaUpdateSeconds, oConfig.uSampleRateHz);

    if (oConfig.uEcaEstimateSamples == 0 || oConfig.uEcaUpdateSamples == 0) {
        std::fprintf(stderr, "config: ECA_BATCH_LENGTH_s and ECA_UPDATE_INTERVAL_s "
                             "must each be at least one sample at %llu Hz\n",
                     static_cast<unsigned long long>(oConfig.uSampleRateHz));
        return false;
    }
    if (oConfig.uEcaUpdateSamples > oConfig.uEcaEstimateSamples) {
        std::fprintf(stderr,
                     "config: ECA_UPDATE_INTERVAL_s (%llu samples) is longer than "
                     "ECA_BATCH_LENGTH_s (%llu samples); weights cannot be applied "
                     "to more signal than they were estimated from\n",
                     static_cast<unsigned long long>(oConfig.uEcaUpdateSamples),
                     static_cast<unsigned long long>(oConfig.uEcaEstimateSamples));
        return false;
    }
    if (oConfig.uEcaEstimateSamples > oConfig.uCpiSamples) {
        std::fprintf(stderr,
                     "config: ECA_BATCH_LENGTH_s (%llu samples) exceeds the %llu "
                     "samples in one CPI\n",
                     static_cast<unsigned long long>(oConfig.uEcaEstimateSamples),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    if (oConfig.uCpiSamples % oConfig.uEcaUpdateSamples != 0) {
        std::fprintf(stderr,
                     "config: ECA_UPDATE_INTERVAL_s (%llu samples) does not divide "
                     "the %llu samples per CPI exactly\n",
                     static_cast<unsigned long long>(oConfig.uEcaUpdateSamples),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    // Fewer samples than unknowns leaves the least-squares problem
    // underdetermined and the Gram matrix singular.
    if (oConfig.uEcaEstimateSamples <= oConfig.uClutterTaps) {
        std::fprintf(stderr,
                     "config: ECA_BATCH_LENGTH_s gives %llu samples, which is not "
                     "more than the %llu clutter taps being solved for\n",
                     static_cast<unsigned long long>(oConfig.uEcaEstimateSamples),
                     static_cast<unsigned long long>(oConfig.uClutterTaps));
        return false;
    }

    if (oConfig.uBufferCapacity < 2 * oConfig.uCpiSamples) {
        std::fprintf(stderr,
                     "config: warning -- BUFFER_LENGTH_s (%.3f s) is less than twice "
                     "CPI_LENGTH_s (%.3f s), leaving little slack if processing "
                     "falls behind\n",
                     oConfig.dBufferSeconds, oConfig.dCpiSeconds);
    }

    return true;
}

} // namespace

bool loadPbrConfig(const char* pPath, PbrConfig& oConfig)
{
    if (pPath == nullptr) {
        std::fprintf(stderr, "config: null path\n");
        return false;
    }

    KeyValueMap mValues;
    if (!readKeyValues(pPath, mValues))
        return false;

    oConfig = PbrConfig();
    bool bOk = true;

    getUInt64(mValues, "SAMPLE_RATE_Hz",             oConfig.uSampleRateHz,      bOk);
    getString(mValues, "REFERENCE_RECEIVER_NAME",    oConfig.sReferenceName,     bOk);
    getString(mValues, "SURVEILLANCE_RECEIVER_NAME", oConfig.sSurveillanceName,  bOk);

    uint64_t uPort = 0;
    getUInt64(mValues, "UDP_PORT", uPort, bOk);
    if (uPort > 65535) {
        std::fprintf(stderr, "config: UDP_PORT %llu is out of range\n",
                     static_cast<unsigned long long>(uPort));
        bOk = false;
    }
    oConfig.uUdpPort = static_cast<uint16_t>(uPort);

    getDouble(mValues, "VITA49_FULLSCALE",     oConfig.dFullScale,         bOk);
    getDouble(mValues, "BUFFER_LENGTH_s",      oConfig.dBufferSeconds,     bOk);

    getDouble(mValues, "CPI_LENGTH_s",         oConfig.dCpiSeconds,        bOk);
    getDouble(mValues, "CPI_HOP_s",            oConfig.dHopSeconds,        bOk);
    getUInt64(mValues, "N_RANGE_BINS",         oConfig.uRangeBins,         bOk);
    getUInt64(mValues, "N_CLUTTER_TAPS",       oConfig.uClutterTaps,       bOk);
    getDouble(mValues, "DOPPLER_HALF_SPAN_Hz", oConfig.dDopplerHalfSpanHz, bOk);
    getUInt64(mValues, "DOPPLER_DECIMATION",   oConfig.uDopplerDecimation, bOk);
    getDouble(mValues, "MIN_CPI_COVERAGE",     oConfig.dMinCpiCoverage,    bOk);

    getString(mValues, "DSI_ALGORITHM",        oConfig.sDsiAlgorithm,      bOk);
    getString(mValues, "DSI_BACKEND",          oConfig.sDsiBackend,        bOk);
    getDouble(mValues, "ECA_BATCH_LENGTH_s",   oConfig.dEcaBatchSeconds,   bOk);
    getDouble(mValues, "ECA_UPDATE_INTERVAL_s", oConfig.dEcaUpdateSeconds, bOk);
    getUInt64(mValues, "ECA_DOPPLER_TAPS",     oConfig.uEcaDopplerTaps,    bOk);
    getDouble(mValues, "ECA_DIAGONAL_LOADING", oConfig.dEcaDiagLoading,    bOk);
    getUInt64(mValues, "CGLS_ITERATIONS",      oConfig.uCglsIterations,    bOk);
    getDouble(mValues, "CGLS_TOLERANCE",       oConfig.dCglsTolerance,     bOk);

    getDouble(mValues, "CFAR_PFA",             oConfig.dCfarPfa,           bOk);
    getUInt64(mValues, "CFAR_GUARD_RANGE",     oConfig.uCfarGuardRange,    bOk);
    getUInt64(mValues, "CFAR_GUARD_DOPPLER",   oConfig.uCfarGuardDoppler,  bOk);
    getUInt64(mValues, "CFAR_TRAIN_RANGE",     oConfig.uCfarTrainRange,    bOk);
    getUInt64(mValues, "CFAR_TRAIN_DOPPLER",   oConfig.uCfarTrainDoppler,  bOk);

    if (!bOk) {
        std::fprintf(stderr, "config: %s could not be loaded\n", pPath);
        return false;
    }

    return validate(oConfig);
}

void printPbrConfig(const PbrConfig& oConfig)
{
    std::printf("configuration\n");
    std::printf("  sample rate        %llu Hz\n",
                static_cast<unsigned long long>(oConfig.uSampleRateHz));
    std::printf("  reference          %s\n", oConfig.sReferenceName.c_str());
    std::printf("  surveillance       %s\n", oConfig.sSurveillanceName.c_str());
    std::printf("  udp port           %u\n", static_cast<unsigned>(oConfig.uUdpPort));
    std::printf("  fullscale          %g\n", oConfig.dFullScale);
    std::printf("  buffer             %.3f s  (%llu samples per channel)\n",
                oConfig.dBufferSeconds,
                static_cast<unsigned long long>(oConfig.uBufferCapacity));
    std::printf("  CPI                %.3f s  (%llu samples)\n",
                oConfig.dCpiSeconds,
                static_cast<unsigned long long>(oConfig.uCpiSamples));
    std::printf("  hop                %.3f s  (%llu samples)\n",
                oConfig.dHopSeconds,
                static_cast<unsigned long long>(oConfig.uHopSamples));
    std::printf("  range bins         %llu\n",
                static_cast<unsigned long long>(oConfig.uRangeBins));
    std::printf("  clutter taps       %llu\n",
                static_cast<unsigned long long>(oConfig.uClutterTaps));
    std::printf("  doppler            %llu bins, decimation %llu, +/-%.1f Hz shown\n",
                static_cast<unsigned long long>(oConfig.uDopplerBins),
                static_cast<unsigned long long>(oConfig.uDopplerDecimation),
                oConfig.dDopplerHalfSpanHz);
    std::printf("  min CPI coverage   %.1f%%\n", 100.0 * oConfig.dMinCpiCoverage);
    std::printf("  DSI                %s on %s\n",
                oConfig.sDsiAlgorithm.c_str(), oConfig.sDsiBackend.c_str());
    std::printf("  ECA windows        estimate %.4f s (%llu samples), "
                "update %.4f s (%llu samples)\n",
                oConfig.dEcaBatchSeconds,
                static_cast<unsigned long long>(oConfig.uEcaEstimateSamples),
                oConfig.dEcaUpdateSeconds,
                static_cast<unsigned long long>(oConfig.uEcaUpdateSamples));
    std::printf("  ECA loading        %g (relative), doppler taps %llu\n",
                oConfig.dEcaDiagLoading,
                static_cast<unsigned long long>(oConfig.uEcaDopplerTaps));
    std::printf("  CFAR               Pfa %g, guard %llux%llu, train %llux%llu\n",
                oConfig.dCfarPfa,
                static_cast<unsigned long long>(oConfig.uCfarGuardRange),
                static_cast<unsigned long long>(oConfig.uCfarGuardDoppler),
                static_cast<unsigned long long>(oConfig.uCfarTrainRange),
                static_cast<unsigned long long>(oConfig.uCfarTrainDoppler));
}

} // namespace pbr
