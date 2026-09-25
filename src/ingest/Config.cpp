#include "pbr/Config.hpp"

#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace pbr {

namespace {

// Every message the loader prints is also appended here when a caller sets
// it, so a refused live change can carry its reason back to the GUI. One
// thread ever loads configs, so a file-static sink is enough.
std::string* g_pMessageSink = nullptr;

void configMessage(const char* pFormat, ...)
{
    char achLine[512];
    va_list oArgs;
    va_start(oArgs, pFormat);
    std::vsnprintf(achLine, sizeof(achLine), pFormat, oArgs);
    va_end(oArgs);
    std::fputs(achLine, stderr);
    if (g_pMessageSink != nullptr)
        g_pMessageSink->append(achLine);
}

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

// Parse KEY = VALUE text into key/value pairs. Rejects malformed lines and
// duplicate keys rather than guessing what was meant. pPath names the source
// in messages: a file path, or "live change".
bool parseKeyValues(const std::string& sText, const char* pPath, KeyValueMap& mValues)
{
    int  iLineNo = 0;
    bool bOk     = true;

    size_t uPos = 0;
    while (uPos < sText.size()) {
        size_t uEnd = sText.find('\n', uPos);
        if (uEnd == std::string::npos)
            uEnd = sText.size();
        std::string sLine = sText.substr(uPos, uEnd - uPos);
        uPos = uEnd + 1;
        ++iLineNo;

        // '#' starts a comment, to end of line.
        const size_t uHash = sLine.find('#');
        if (uHash != std::string::npos)
            sLine = sLine.substr(0, uHash);

        sLine = trim(sLine);
        if (sLine.empty())
            continue;

        const size_t uEquals = sLine.find('=');
        if (uEquals == std::string::npos) {
            configMessage("config: %s:%d has no '=': \"%s\"\n",
                         pPath, iLineNo, sLine.c_str());
            bOk = false;
            continue;
        }

        const std::string sKey   = trim(sLine.substr(0, uEquals));
        const std::string sValue = trim(sLine.substr(uEquals + 1));

        if (sKey.empty()) {
            configMessage("config: %s:%d has an empty key\n", pPath, iLineNo);
            bOk = false;
            continue;
        }
        if (sValue.empty()) {
            configMessage("config: %s:%d key %s has an empty value\n",
                         pPath, iLineNo, sKey.c_str());
            bOk = false;
            continue;
        }
        if (mValues.find(sKey) != mValues.end()) {
            configMessage("config: %s:%d key %s is set more than once\n",
                         pPath, iLineNo, sKey.c_str());
            bOk = false;
            continue;
        }

        mValues[sKey] = sValue;
    }

    return bOk;
}

// Read a whole file, then parse it.
bool readKeyValues(const char* pPath, KeyValueMap& mValues)
{
    std::FILE* pFile = std::fopen(pPath, "r");
    if (pFile == nullptr) {
        configMessage("config: cannot open %s: %s\n", pPath, std::strerror(errno));
        return false;
    }
    std::string sText;
    char achChunk[4096];
    for (;;) {
        const size_t uGot = std::fread(achChunk, 1, sizeof(achChunk), pFile);
        if (uGot == 0)
            break;
        sText.append(achChunk, uGot);
    }
    std::fclose(pFile);
    return parseKeyValues(sText, pPath, mValues);
}

// Each getter reports a missing or unparseable key and clears bOk, so a single
// load reports every problem at once rather than one per run.
void getString(const KeyValueMap& mValues, const char* pKey,
               std::string& sOut, bool& bOk)
{
    const KeyValueMap::const_iterator it = mValues.find(pKey);
    if (it == mValues.end()) {
        configMessage("config: required key %s is missing\n", pKey);
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
        configMessage("config: key %s value \"%s\" is not a number\n",
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
        configMessage(
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
        { "ECA_CD_BATCH_s",       oConfig.dCdBatchSeconds },
        { "CGLS_BATCH_s",         oConfig.dCglsBatchSeconds },
        { "CGLS_ITERATIONS",      static_cast<double>(oConfig.uCglsIterations) },
    };
    for (size_t u = 0; u < sizeof(aPositive) / sizeof(aPositive[0]); ++u) {
        if (aPositive[u].dValue <= 0.0) {
            configMessage("config: %s must be greater than zero\n",
                         aPositive[u].pName);
            bOk = false;
        }
    }

    if (oConfig.uUdpPort == 0) {
        configMessage("config: UDP_PORT must be between 1 and 65535\n");
        bOk = false;
    }
    if (oConfig.sReferenceName.empty() || oConfig.sSurveillanceName.empty()) {
        configMessage("config: receiver names must not be empty\n");
        bOk = false;
    }
    if (oConfig.sReferenceName == oConfig.sSurveillanceName) {
        configMessage("config: reference and surveillance receiver names "
                             "are identical (%s)\n", oConfig.sReferenceName.c_str());
        bOk = false;
    }
    if (oConfig.dCfarPfa <= 0.0 || oConfig.dCfarPfa >= 1.0) {
        configMessage("config: CFAR_PFA must be strictly between 0 and 1\n");
        bOk = false;
    }
    if (oConfig.dMinCpiCoverage <= 0.0 || oConfig.dMinCpiCoverage > 1.0) {
        configMessage("config: MIN_CPI_COVERAGE must be in (0, 1]\n");
        bOk = false;
    }

    // The ECA formulation needs at least as many delay taps as range bins.
    if (oConfig.uClutterTaps < oConfig.uRangeBins) {
        configMessage(
                     "config: N_CLUTTER_TAPS (%llu) must be at least N_RANGE_BINS (%llu)\n",
                     static_cast<unsigned long long>(oConfig.uClutterTaps),
                     static_cast<unsigned long long>(oConfig.uRangeBins));
        bOk = false;
    }

    if (oConfig.sDsiAlgorithm.empty() || oConfig.sDsiBackend.empty()) {
        configMessage("config: DSI_ALGORITHM and DSI_BACKEND must not be empty\n");
        bOk = false;
    }
    if (oConfig.uArdSnapshotEvery == 0) {
        configMessage("config: ARD_SNAPSHOT_EVERY must be at least 1\n");
        bOk = false;
    }
    if (oConfig.sArdAlgorithm != "Direct" && oConfig.sArdAlgorithm != "Batches") {
        configMessage(
                     "config: ARD_ALGORITHM must be Direct or Batches, not \"%s\"\n",
                     oConfig.sArdAlgorithm.c_str());
        bOk = false;
    }
    if (oConfig.sDsiMathPrecision != "FP64" && oConfig.sDsiMathPrecision != "FP32") {
        configMessage("config: DSI_MATH_PRECISION must be FP64 or FP32, not \"%s\"\n",
                     oConfig.sDsiMathPrecision.c_str());
        bOk = false;
    }
    // Loading is a fraction of the Gram diagonal, so it is dimensionless and
    // a value of 1 or more would swamp the signal entirely.
    if (oConfig.dEcaDiagLoading < 0.0 || oConfig.dEcaDiagLoading >= 1.0) {
        configMessage("config: ECA_DIAGONAL_LOADING must be in [0, 1)\n");
        bOk = false;
    }
    // The ECA-CD projection solves a (2P+1)-square system per carrier inside
    // one GPU thread, sized for P <= 3 at compile time.
    if (oConfig.uCdDopplerTaps > 3) {
        configMessage("config: ECA_CD_DOPPLER_TAPS must be at most 3\n");
        bOk = false;
    }

    if (!bOk)
        return false;

    // ---- derived values ---------------------------------------------------
    oConfig.uCpiSamples     = toSamples(oConfig.dCpiSeconds,    oConfig.uSampleRateHz);
    oConfig.uHopSamples     = toSamples(oConfig.dHopSeconds,    oConfig.uSampleRateHz);
    oConfig.uBufferCapacity = toSamples(oConfig.dBufferSeconds, oConfig.uSampleRateHz);

    if (oConfig.uCpiSamples == 0 || oConfig.uHopSamples == 0) {
        configMessage("config: CPI_LENGTH_s and CPI_HOP_s must each be at "
                             "least one sample at %llu Hz\n",
                     static_cast<unsigned long long>(oConfig.uSampleRateHz));
        return false;
    }

    // A decimation factor that does not divide the CPI evenly leaves a
    // remainder and a subtly wrong Doppler axis.
    if (oConfig.uCpiSamples % oConfig.uDopplerDecimation != 0) {
        configMessage(
                     "config: DOPPLER_DECIMATION (%llu) does not divide the %llu "
                     "samples per CPI exactly\n",
                     static_cast<unsigned long long>(oConfig.uDopplerDecimation),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    oConfig.uDopplerBins = oConfig.uCpiSamples / oConfig.uDopplerDecimation;

    // The buffer has to hold a whole CPI, or extraction can never succeed.
    if (oConfig.uBufferCapacity < oConfig.uCpiSamples) {
        configMessage(
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
        configMessage("config: ECA_BATCH_LENGTH_s and ECA_UPDATE_INTERVAL_s "
                             "must each be at least one sample at %llu Hz\n",
                     static_cast<unsigned long long>(oConfig.uSampleRateHz));
        return false;
    }
    if (oConfig.uEcaUpdateSamples > oConfig.uEcaEstimateSamples) {
        configMessage(
                     "config: ECA_UPDATE_INTERVAL_s (%llu samples) is longer than "
                     "ECA_BATCH_LENGTH_s (%llu samples); weights cannot be applied "
                     "to more signal than they were estimated from\n",
                     static_cast<unsigned long long>(oConfig.uEcaUpdateSamples),
                     static_cast<unsigned long long>(oConfig.uEcaEstimateSamples));
        return false;
    }
    if (oConfig.uEcaEstimateSamples > oConfig.uCpiSamples) {
        configMessage(
                     "config: ECA_BATCH_LENGTH_s (%llu samples) exceeds the %llu "
                     "samples in one CPI\n",
                     static_cast<unsigned long long>(oConfig.uEcaEstimateSamples),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    if (oConfig.uCpiSamples % oConfig.uEcaUpdateSamples != 0) {
        configMessage(
                     "config: ECA_UPDATE_INTERVAL_s (%llu samples) does not divide "
                     "the %llu samples per CPI exactly\n",
                     static_cast<unsigned long long>(oConfig.uEcaUpdateSamples),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    // ECA-S shares one partial Gram per update window between the estimation
    // windows that span it, which needs the estimation window to be a whole
    // number of them (docs/design_decisions.md 1d.9). ECA-B satisfies this
    // trivially, the two being equal.
    if (oConfig.uEcaEstimateSamples % oConfig.uEcaUpdateSamples != 0) {
        configMessage(
                     "config: ECA_UPDATE_INTERVAL_s (%llu samples) does not divide "
                     "ECA_BATCH_LENGTH_s (%llu samples) exactly\n",
                     static_cast<unsigned long long>(oConfig.uEcaUpdateSamples),
                     static_cast<unsigned long long>(oConfig.uEcaEstimateSamples));
        return false;
    }
    // Fewer samples than unknowns leaves the least-squares problem
    // underdetermined and the Gram matrix singular.
    if (oConfig.uEcaEstimateSamples <= oConfig.uClutterTaps) {
        configMessage(
                     "config: ECA_BATCH_LENGTH_s gives %llu samples, which is not "
                     "more than the %llu clutter taps being solved for\n",
                     static_cast<unsigned long long>(oConfig.uEcaEstimateSamples),
                     static_cast<unsigned long long>(oConfig.uClutterTaps));
        return false;
    }

    // ECA-CD batch. The CPU backend's FFT is radix-2, so the length must be a
    // power of two; it must also tile the CPI exactly for the same reason the
    // ECA update window must.
    oConfig.uCdBatchSamples = toSamples(oConfig.dCdBatchSeconds, oConfig.uSampleRateHz);
    if (oConfig.uCdBatchSamples == 0 ||
        (oConfig.uCdBatchSamples & (oConfig.uCdBatchSamples - 1)) != 0) {
        configMessage(
                     "config: ECA_CD_BATCH_s gives %llu samples, which is not a "
                     "power of two\n",
                     static_cast<unsigned long long>(oConfig.uCdBatchSamples));
        return false;
    }
    if (oConfig.uCpiSamples % oConfig.uCdBatchSamples != 0) {
        configMessage(
                     "config: ECA_CD_BATCH_s (%llu samples) does not divide the "
                     "%llu samples per CPI exactly\n",
                     static_cast<unsigned long long>(oConfig.uCdBatchSamples),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }

    // CGLS window: the same two rules as the ECA update window, on its own key
    // so the two can be tuned apart.
    oConfig.uCglsBatchSamples = toSamples(oConfig.dCglsBatchSeconds, oConfig.uSampleRateHz);
    if (oConfig.uCglsBatchSamples == 0 ||
        oConfig.uCpiSamples % oConfig.uCglsBatchSamples != 0) {
        configMessage(
                     "config: CGLS_BATCH_s (%llu samples) does not divide the %llu "
                     "samples per CPI exactly\n",
                     static_cast<unsigned long long>(oConfig.uCglsBatchSamples),
                     static_cast<unsigned long long>(oConfig.uCpiSamples));
        return false;
    }
    if (oConfig.uCglsBatchSamples <= oConfig.uClutterTaps) {
        configMessage(
                     "config: CGLS_BATCH_s gives %llu samples, which is not more "
                     "than the %llu clutter taps being solved for\n",
                     static_cast<unsigned long long>(oConfig.uCglsBatchSamples),
                     static_cast<unsigned long long>(oConfig.uClutterTaps));
        return false;
    }

    if (oConfig.uBufferCapacity < 2 * oConfig.uCpiSamples) {
        configMessage(
                     "config: warning -- BUFFER_LENGTH_s (%.3f s) is less than twice "
                     "CPI_LENGTH_s (%.3f s), leaving little slack if processing "
                     "falls behind\n",
                     oConfig.dBufferSeconds, oConfig.dCpiSeconds);
    }

    return true;
}

} // namespace

namespace {

// Every key the loader knows, for refusing a live change that names one it
// does not. Keep in step with configFromKeyValues() below.
const char* const kKnownKeys[] = {
    "SAMPLE_RATE_Hz", "REFERENCE_RECEIVER_NAME", "SURVEILLANCE_RECEIVER_NAME",
    "UDP_PORT", "CONTROL_PORT", "VITA49_FULLSCALE", "BUFFER_LENGTH_s",
    "CPI_LENGTH_s", "CPI_HOP_s", "N_RANGE_BINS", "N_CLUTTER_TAPS",
    "DOPPLER_HALF_SPAN_Hz", "DOPPLER_DECIMATION", "MIN_CPI_COVERAGE",
    "ARD_ALGORITHM", "DSI_ALGORITHM", "DSI_BACKEND", "DSI_MATH_PRECISION",
    "ECA_BATCH_LENGTH_s", "ECA_UPDATE_INTERVAL_s", "ECA_DOPPLER_TAPS",
    "ECA_DIAGONAL_LOADING", "ECA_CD_BATCH_s", "ECA_CD_DOPPLER_TAPS",
    "ARD_SNAPSHOT_EVERY", "ARD_WRITE_FILES", "ARD_STREAM", "ARD_STREAM_DEST",
    "CGLS_ITERATIONS", "CGLS_BATCH_s", "CGLS_PRIME",
    "CFAR_PFA", "CFAR_GUARD_RANGE", "CFAR_GUARD_DOPPLER",
    "CFAR_TRAIN_RANGE", "CFAR_TRAIN_DOPPLER",
};

bool isKnownKey(const std::string& sKey)
{
    for (size_t u = 0; u < sizeof(kKnownKeys) / sizeof(kKnownKeys[0]); ++u)
        if (sKey == kKnownKeys[u])
            return true;
    return false;
}

// From parsed key/values to a validated config with its derived fields.
// pLabel names the source in the failure message.
bool configFromKeyValues(const KeyValueMap& mValues, const char* pLabel, PbrConfig& oConfig)
{
    oConfig = PbrConfig();
    bool bOk = true;

    getUInt64(mValues, "SAMPLE_RATE_Hz",             oConfig.uSampleRateHz,      bOk);
    getString(mValues, "REFERENCE_RECEIVER_NAME",    oConfig.sReferenceName,     bOk);
    getString(mValues, "SURVEILLANCE_RECEIVER_NAME", oConfig.sSurveillanceName,  bOk);

    uint64_t uPort = 0;
    getUInt64(mValues, "UDP_PORT", uPort, bOk);
    if (uPort > 65535) {
        configMessage("config: UDP_PORT %llu is out of range\n",
                     static_cast<unsigned long long>(uPort));
        bOk = false;
    }
    oConfig.uUdpPort = static_cast<uint16_t>(uPort);

    // The chain's control port for live changes; 0 disables it.
    uint64_t uControl = 0;
    getUInt64(mValues, "CONTROL_PORT", uControl, bOk);
    if (uControl > 65535) {
        configMessage("config: CONTROL_PORT %llu is out of range\n",
                     static_cast<unsigned long long>(uControl));
        bOk = false;
    }
    oConfig.uControlPort = static_cast<uint16_t>(uControl);

    getDouble(mValues, "VITA49_FULLSCALE",     oConfig.dFullScale,         bOk);
    getDouble(mValues, "BUFFER_LENGTH_s",      oConfig.dBufferSeconds,     bOk);

    getDouble(mValues, "CPI_LENGTH_s",         oConfig.dCpiSeconds,        bOk);
    getDouble(mValues, "CPI_HOP_s",            oConfig.dHopSeconds,        bOk);
    getUInt64(mValues, "N_RANGE_BINS",         oConfig.uRangeBins,         bOk);
    getUInt64(mValues, "N_CLUTTER_TAPS",       oConfig.uClutterTaps,       bOk);
    getDouble(mValues, "DOPPLER_HALF_SPAN_Hz", oConfig.dDopplerHalfSpanHz, bOk);
    getUInt64(mValues, "DOPPLER_DECIMATION",   oConfig.uDopplerDecimation, bOk);
    getDouble(mValues, "MIN_CPI_COVERAGE",     oConfig.dMinCpiCoverage,    bOk);

    getString(mValues, "ARD_ALGORITHM",        oConfig.sArdAlgorithm,      bOk);

    getString(mValues, "DSI_ALGORITHM",        oConfig.sDsiAlgorithm,      bOk);
    getString(mValues, "DSI_BACKEND",          oConfig.sDsiBackend,        bOk);
    getString(mValues, "DSI_MATH_PRECISION",   oConfig.sDsiMathPrecision,  bOk);
    getDouble(mValues, "ECA_BATCH_LENGTH_s",   oConfig.dEcaBatchSeconds,   bOk);
    getDouble(mValues, "ECA_UPDATE_INTERVAL_s", oConfig.dEcaUpdateSeconds, bOk);
    getUInt64(mValues, "ECA_DOPPLER_TAPS",     oConfig.uEcaDopplerTaps,    bOk);
    getDouble(mValues, "ECA_DIAGONAL_LOADING", oConfig.dEcaDiagLoading,    bOk);
    getDouble(mValues, "ECA_CD_BATCH_s",       oConfig.dCdBatchSeconds,    bOk);
    getUInt64(mValues, "ECA_CD_DOPPLER_TAPS",  oConfig.uCdDopplerTaps,     bOk);
    getUInt64(mValues, "ARD_SNAPSHOT_EVERY",   oConfig.uArdSnapshotEvery,  bOk);

    // The two ARD outputs, 0 or 1 each, and the stream destination as
    // host:port. Read even when the stream is off, so a bad line is caught
    // before the day it is turned on.
    uint64_t uWriteFiles = 0;
    uint64_t uStream     = 0;
    getUInt64(mValues, "ARD_WRITE_FILES", uWriteFiles, bOk);
    getUInt64(mValues, "ARD_STREAM",      uStream,     bOk);
    if (uWriteFiles > 1 || uStream > 1) {
        configMessage("config: ARD_WRITE_FILES and ARD_STREAM must be 0 or 1\n");
        bOk = false;
    }
    oConfig.bArdWriteFiles = (uWriteFiles == 1);
    oConfig.bArdStream     = (uStream == 1);

    std::string sDest;
    getString(mValues, "ARD_STREAM_DEST", sDest, bOk);
    if (!sDest.empty()) {
        const size_t uColon = sDest.rfind(':');
        const long   lPort  = (uColon == std::string::npos) ? 0
                            : std::strtol(sDest.c_str() + uColon + 1, nullptr, 10);
        if (uColon == std::string::npos || uColon == 0 || lPort < 1 || lPort > 65535) {
            configMessage("config: ARD_STREAM_DEST must be HOST:PORT, got \"%s\"\n",
                         sDest.c_str());
            bOk = false;
        } else {
            oConfig.sArdStreamHost = sDest.substr(0, uColon);
            oConfig.uArdStreamPort = static_cast<uint16_t>(lPort);
        }
    }
    getUInt64(mValues, "CGLS_ITERATIONS",      oConfig.uCglsIterations,    bOk);
    getDouble(mValues, "CGLS_BATCH_s",         oConfig.dCglsBatchSeconds,  bOk);

    // 0 or 1, read as an integer so a typo like "yes" is a load error rather
    // than a silent false.
    uint64_t uPrime = 0;
    getUInt64(mValues, "CGLS_PRIME", uPrime, bOk);
    if (uPrime > 1) {
        configMessage("config: CGLS_PRIME must be 0 or 1\n");
        bOk = false;
    }
    oConfig.bCglsPrime = (uPrime == 1);

    getDouble(mValues, "CFAR_PFA",             oConfig.dCfarPfa,           bOk);
    getUInt64(mValues, "CFAR_GUARD_RANGE",     oConfig.uCfarGuardRange,    bOk);
    getUInt64(mValues, "CFAR_GUARD_DOPPLER",   oConfig.uCfarGuardDoppler,  bOk);
    getUInt64(mValues, "CFAR_TRAIN_RANGE",     oConfig.uCfarTrainRange,    bOk);
    getUInt64(mValues, "CFAR_TRAIN_DOPPLER",   oConfig.uCfarTrainDoppler,  bOk);

    if (!bOk) {
        configMessage("config: %s could not be loaded\n", pLabel);
        return false;
    }

    return validate(oConfig);
}

// A whole number of samples at a rate, back to seconds, printed so that the
// loader's rounding recovers exactly that count.
std::string secondsText(uint64_t uSamples, uint64_t uSampleRateHz)
{
    char ach[64];
    std::snprintf(ach, sizeof(ach), "%.9g",
                  static_cast<double>(uSamples) / static_cast<double>(uSampleRateHz));
    return ach;
}

uint64_t gcd64(uint64_t a, uint64_t b)
{
    while (b != 0) { const uint64_t t = a % b; a = b; b = t; }
    return a;
}

uint64_t lcm64(uint64_t a, uint64_t b)
{
    return (a / gcd64(a, b)) * b;
}

// The power of two nearest to uTarget, at least 2.
uint64_t nearestPowerOfTwo(uint64_t uTarget)
{
    uint64_t uBest = 2;
    for (uint64_t u = 2; u != 0 && u <= (1ULL << 40); u <<= 1) {
        const uint64_t dBest = (uBest > uTarget) ? uBest - uTarget : uTarget - uBest;
        const uint64_t dThis = (u > uTarget) ? u - uTarget : uTarget - u;
        if (dThis <= dBest)
            uBest = u;
        if (u > uTarget)
            break;
    }
    return uBest;
}

// The divisor of uOf nearest to uTarget, at least uMin.
uint64_t nearestDivisor(uint64_t uOf, uint64_t uTarget, uint64_t uMin)
{
    uint64_t uBest = uOf;
    for (uint64_t d = 1; d * d <= uOf; ++d) {
        if (uOf % d != 0)
            continue;
        const uint64_t aCandidates[2] = { d, uOf / d };
        for (int i = 0; i < 2; ++i) {
            const uint64_t c = aCandidates[i];
            if (c < uMin)
                continue;
            const uint64_t dBest = (uBest > uTarget) ? uBest - uTarget : uTarget - uBest;
            const uint64_t dThis = (c > uTarget) ? c - uTarget : uTarget - c;
            if (dThis < dBest)
                uBest = c;
        }
    }
    return uBest;
}

// The multiple of uStep nearest to uTarget, at least uStep.
uint64_t nearestMultiple(uint64_t uStep, uint64_t uTarget)
{
    const uint64_t uLower = (uTarget / uStep) * uStep;
    const uint64_t uUpper = uLower + uStep;
    if (uLower == 0)
        return uStep;
    return (uTarget - uLower <= uUpper - uTarget) ? uLower : uUpper;
}

} // namespace

bool loadPbrConfig(const char* pPath, PbrConfig& oConfig)
{
    if (pPath == nullptr) {
        configMessage("config: null path\n");
        return false;
    }
    KeyValueMap mValues;
    if (!readKeyValues(pPath, mValues))
        return false;
    return configFromKeyValues(mValues, pPath, oConfig);
}

bool loadPbrConfigFromText(const std::string& sText, PbrConfig& oConfig, std::string* pMessages)
{
    g_pMessageSink = pMessages;
    KeyValueMap mValues;
    bool bOk = parseKeyValues(sText, "config text", mValues) &&
               configFromKeyValues(mValues, "config text", oConfig);
    g_pMessageSink = nullptr;
    return bOk;
}

std::string pbrConfigToText(const PbrConfig& o)
{
    char ach[8192];
    std::snprintf(ach, sizeof(ach),
        "SAMPLE_RATE_Hz = %llu\n"
        "REFERENCE_RECEIVER_NAME = %s\n"
        "SURVEILLANCE_RECEIVER_NAME = %s\n"
        "UDP_PORT = %u\n"
        "CONTROL_PORT = %u\n"
        "VITA49_FULLSCALE = %.10g\n"
        "BUFFER_LENGTH_s = %.10g\n"
        "CPI_LENGTH_s = %.10g\n"
        "CPI_HOP_s = %.10g\n"
        "N_RANGE_BINS = %llu\n"
        "N_CLUTTER_TAPS = %llu\n"
        "DOPPLER_HALF_SPAN_Hz = %.10g\n"
        "DOPPLER_DECIMATION = %llu\n"
        "MIN_CPI_COVERAGE = %.10g\n"
        "ARD_ALGORITHM = %s\n"
        "DSI_ALGORITHM = %s\n"
        "DSI_BACKEND = %s\n"
        "DSI_MATH_PRECISION = %s\n"
        "ECA_BATCH_LENGTH_s = %.10g\n"
        "ECA_UPDATE_INTERVAL_s = %.10g\n"
        "ECA_DOPPLER_TAPS = %llu\n"
        "ECA_DIAGONAL_LOADING = %.10g\n"
        "ECA_CD_BATCH_s = %.10g\n"
        "ECA_CD_DOPPLER_TAPS = %llu\n"
        "ARD_SNAPSHOT_EVERY = %llu\n"
        "ARD_WRITE_FILES = %d\n"
        "ARD_STREAM = %d\n"
        "ARD_STREAM_DEST = %s:%u\n"
        "CGLS_ITERATIONS = %llu\n"
        "CGLS_BATCH_s = %.10g\n"
        "CGLS_PRIME = %d\n"
        "CFAR_PFA = %.10g\n"
        "CFAR_GUARD_RANGE = %llu\n"
        "CFAR_GUARD_DOPPLER = %llu\n"
        "CFAR_TRAIN_RANGE = %llu\n"
        "CFAR_TRAIN_DOPPLER = %llu\n",
        (unsigned long long)o.uSampleRateHz,
        o.sReferenceName.c_str(), o.sSurveillanceName.c_str(),
        (unsigned)o.uUdpPort, (unsigned)o.uControlPort,
        o.dFullScale, o.dBufferSeconds, o.dCpiSeconds, o.dHopSeconds,
        (unsigned long long)o.uRangeBins, (unsigned long long)o.uClutterTaps,
        o.dDopplerHalfSpanHz, (unsigned long long)o.uDopplerDecimation,
        o.dMinCpiCoverage, o.sArdAlgorithm.c_str(),
        o.sDsiAlgorithm.c_str(), o.sDsiBackend.c_str(), o.sDsiMathPrecision.c_str(),
        o.dEcaBatchSeconds, o.dEcaUpdateSeconds, (unsigned long long)o.uEcaDopplerTaps,
        o.dEcaDiagLoading, o.dCdBatchSeconds, (unsigned long long)o.uCdDopplerTaps,
        (unsigned long long)o.uArdSnapshotEvery,
        o.bArdWriteFiles ? 1 : 0, o.bArdStream ? 1 : 0,
        o.sArdStreamHost.c_str(), (unsigned)o.uArdStreamPort,
        (unsigned long long)o.uCglsIterations, o.dCglsBatchSeconds, o.bCglsPrime ? 1 : 0,
        o.dCfarPfa,
        (unsigned long long)o.uCfarGuardRange, (unsigned long long)o.uCfarGuardDoppler,
        (unsigned long long)o.uCfarTrainRange, (unsigned long long)o.uCfarTrainDoppler);
    return ach;
}

bool applyPbrConfigLines(const PbrConfig& oCurrent, const std::string& sLines,
                         PbrConfig& oOut, std::string& sMessagesOut)
{
    sMessagesOut.clear();
    g_pMessageSink = &sMessagesOut;

    KeyValueMap mChanges;
    bool bOk = parseKeyValues(sLines, "live change", mChanges);
    if (bOk && mChanges.empty()) {
        configMessage("live change: no KEY = VALUE lines\n");
        bOk = false;
    }
    for (KeyValueMap::const_iterator it = mChanges.begin(); bOk && it != mChanges.end(); ++it) {
        if (!isKnownKey(it->first)) {
            configMessage("live change: unknown key %s\n", it->first.c_str());
            bOk = false;
        }
    }

    if (bOk) {
        // Overlay onto the current set, then validate the whole thing into a
        // scratch config, so a refusal leaves oOut exactly as it was.
        KeyValueMap mAll;
        parseKeyValues(pbrConfigToText(oCurrent), "current config", mAll);
        for (KeyValueMap::const_iterator it = mChanges.begin(); it != mChanges.end(); ++it)
            mAll[it->first] = it->second;
        PbrConfig oCandidate;
        bOk = configFromKeyValues(mAll, "live change", oCandidate);
        if (bOk)
            oOut = oCandidate;
    }

    g_pMessageSink = nullptr;
    return bOk;
}

bool adaptPbrConfigToSampleRate(const PbrConfig& oCurrent, uint64_t uSourceRateHz,
                                PbrConfig& oOut, std::string& sReportOut)
{
    sReportOut.clear();
    if (uSourceRateHz == 0) {
        sReportOut = "source sample rate is zero";
        return false;
    }
    if (uSourceRateHz == oCurrent.uSampleRateHz) {
        oOut = oCurrent;
        return true;
    }

    char ach[256];
    std::snprintf(ach, sizeof(ach),
                  "Warning: source sample rate %llu Hz does not match the configured %llu Hz; "
                  "defaulting to the nearest settings the chain allows at %llu Hz:\n",
                  (unsigned long long)uSourceRateHz,
                  (unsigned long long)oCurrent.uSampleRateHz,
                  (unsigned long long)uSourceRateHz);
    sReportOut += ach;

    // Re-express every window in samples at the new rate, snapping each to
    // what the validator requires, in the order the constraints chain:
    // the ECA-CD batch must be a power of two; the CPI must be a multiple of
    // it and of the Doppler decimation; the ECA update and CGLS windows must
    // divide the CPI; the ECA estimation window must be a multiple of the
    // update window.
    const uint64_t uCd   = nearestPowerOfTwo(toSamples(oCurrent.dCdBatchSeconds, uSourceRateHz));
    const uint64_t uBase = lcm64(oCurrent.uDopplerDecimation, uCd);
    const uint64_t uCpi  = nearestMultiple(uBase, toSamples(oCurrent.dCpiSeconds, uSourceRateHz));
    const bool     bHopEqualsCpi = (oCurrent.uHopSamples == oCurrent.uCpiSamples);
    uint64_t       uHop  = bHopEqualsCpi ? uCpi : toSamples(oCurrent.dHopSeconds, uSourceRateHz);
    if (uHop == 0) uHop = 1;
    if (uHop > uCpi) uHop = uCpi;
    const uint64_t uUpd  = nearestDivisor(uCpi, toSamples(oCurrent.dEcaUpdateSeconds, uSourceRateHz), 1);
    uint64_t       uEst  = nearestMultiple(uUpd, toSamples(oCurrent.dEcaBatchSeconds, uSourceRateHz));
    if (uEst > uCpi) uEst = uCpi;
    const uint64_t uCgls = nearestDivisor(uCpi, toSamples(oCurrent.dCglsBatchSeconds, uSourceRateHz), 1);

    KeyValueMap mAll;
    parseKeyValues(pbrConfigToText(oCurrent), "current config", mAll);
    mAll["SAMPLE_RATE_Hz"]        = std::to_string(uSourceRateHz);
    mAll["CPI_LENGTH_s"]          = secondsText(uCpi,  uSourceRateHz);
    mAll["CPI_HOP_s"]             = secondsText(uHop,  uSourceRateHz);
    mAll["ECA_CD_BATCH_s"]        = secondsText(uCd,   uSourceRateHz);
    mAll["ECA_UPDATE_INTERVAL_s"] = secondsText(uUpd,  uSourceRateHz);
    mAll["ECA_BATCH_LENGTH_s"]    = secondsText(uEst,  uSourceRateHz);
    mAll["CGLS_BATCH_s"]          = secondsText(uCgls, uSourceRateHz);

    const struct { const char* pKey; uint64_t uOld; uint64_t uNew; } aWindows[] = {
        { "CPI_LENGTH_s",          oCurrent.uCpiSamples,         uCpi  },
        { "CPI_HOP_s",             oCurrent.uHopSamples,         uHop  },
        { "ECA_CD_BATCH_s",        oCurrent.uCdBatchSamples,     uCd   },
        { "ECA_UPDATE_INTERVAL_s", oCurrent.uEcaUpdateSamples,   uUpd  },
        { "ECA_BATCH_LENGTH_s",    oCurrent.uEcaEstimateSamples, uEst  },
        { "CGLS_BATCH_s",          oCurrent.uCglsBatchSamples,   uCgls },
    };
    for (size_t u = 0; u < sizeof(aWindows) / sizeof(aWindows[0]); ++u) {
        std::snprintf(ach, sizeof(ach), "  %-22s %s s (%llu samples), was %llu samples at %llu Hz\n",
                      aWindows[u].pKey, mAll[aWindows[u].pKey].c_str(),
                      (unsigned long long)aWindows[u].uNew,
                      (unsigned long long)aWindows[u].uOld,
                      (unsigned long long)oCurrent.uSampleRateHz);
        sReportOut += ach;
    }

    std::string sMessages;
    g_pMessageSink = &sMessages;
    PbrConfig oCandidate;
    const bool bOk = configFromKeyValues(mAll, "adapted config", oCandidate);
    g_pMessageSink = nullptr;
    if (!bOk) {
        sReportOut += "  the adapted settings do not validate:\n" + sMessages;
        return false;
    }
    oOut = oCandidate;
    return true;
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
    // The two ARD algorithms produce the same map; the backend logs the
    // transform length it derived, which is the only thing Batches adds.
    std::printf("  ARD                %s\n", oConfig.sArdAlgorithm.c_str());
    std::printf("  ARD output         files %s, stream %s (%s:%u), every %llu CPI(s)\n",
                oConfig.bArdWriteFiles ? "on" : "off",
                oConfig.bArdStream ? "on" : "off",
                oConfig.sArdStreamHost.c_str(),
                static_cast<unsigned>(oConfig.uArdStreamPort),
                static_cast<unsigned long long>(oConfig.uArdSnapshotEvery));
    std::printf("  control port       %u%s\n", static_cast<unsigned>(oConfig.uControlPort),
                oConfig.uControlPort == 0 ? " (live changes off)" : "");
    // The GPU ignores DSI_MATH_PRECISION -- Maxwell runs FP64 at 1/32 the
    // FP32 rate, so single precision is forced. Printing the configured value
    // unqualified next to "on GPU" would read as a claim about what will
    // actually happen.
    if (oConfig.sDsiBackend == "GPU") {
        std::printf("  DSI                %s on GPU (FP32; DSI_MATH_PRECISION=%s "
                    "applies to the CPU backend only)\n",
                    oConfig.sDsiAlgorithm.c_str(),
                    oConfig.sDsiMathPrecision.c_str());
    } else {
        std::printf("  DSI                %s on %s (%s math)\n",
                    oConfig.sDsiAlgorithm.c_str(), oConfig.sDsiBackend.c_str(),
                    oConfig.sDsiMathPrecision.c_str());
    }
    std::printf("  ECA windows        estimate %.4f s (%llu samples), "
                "update %.4f s (%llu samples)\n",
                oConfig.dEcaBatchSeconds,
                static_cast<unsigned long long>(oConfig.uEcaEstimateSamples),
                oConfig.dEcaUpdateSeconds,
                static_cast<unsigned long long>(oConfig.uEcaUpdateSamples));
    std::printf("  ECA loading        %g (relative), doppler taps %llu\n",
                oConfig.dEcaDiagLoading,
                static_cast<unsigned long long>(oConfig.uEcaDopplerTaps));
    std::printf("  ECA-CD             batch %.4f s (%llu samples), doppler taps %llu\n",
                oConfig.dCdBatchSeconds,
                static_cast<unsigned long long>(oConfig.uCdBatchSamples),
                static_cast<unsigned long long>(oConfig.uCdDopplerTaps));
    std::printf("  CGLS               batch %.4f s (%llu samples), %llu iterations, %s\n",
                oConfig.dCglsBatchSeconds,
                static_cast<unsigned long long>(oConfig.uCglsBatchSamples),
                static_cast<unsigned long long>(oConfig.uCglsIterations),
                oConfig.bCglsPrime ? "primed from the previous CPI" : "cold start");
    std::printf("  CFAR               Pfa %g, guard %llux%llu, train %llux%llu\n",
                oConfig.dCfarPfa,
                static_cast<unsigned long long>(oConfig.uCfarGuardRange),
                static_cast<unsigned long long>(oConfig.uCfarGuardDoppler),
                static_cast<unsigned long long>(oConfig.uCfarTrainRange),
                static_cast<unsigned long long>(oConfig.uCfarTrainDoppler));
}

} // namespace pbr
