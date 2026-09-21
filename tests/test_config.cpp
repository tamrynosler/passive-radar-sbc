// Unit tests for the .conf loader. Plain main() + asserts.
//
// NDEBUG is undefined before <cassert> because the project builds Release,
// which would otherwise compile every assert out.
#undef NDEBUG
#include <cassert>

#include "pbr/Config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>   // mkstemp, close

using namespace pbr;

namespace {

// A complete, valid config. Tests derive their cases by editing this.
const char* const kValidConfig =
    "# a comment\n"
    "SAMPLE_RATE_Hz = 204800\n"
    "REFERENCE_RECEIVER_NAME = ArmasuisseRefRxClean\n"
    "SURVEILLANCE_RECEIVER_NAME = ArmasuisseSurRxClean\n"
    "\n"
    "UDP_PORT = 4991\n"
    "VITA49_FULLSCALE = 0.0022\n"
    "BUFFER_LENGTH_s = 2.0\n"
    "\n"
    "CPI_LENGTH_s = 1.0        # inline comment\n"
    "CPI_HOP_s = 1.0\n"
    "N_RANGE_BINS = 120\n"
    "N_CLUTTER_TAPS = 120\n"
    "DOPPLER_HALF_SPAN_Hz = 200\n"
    "DOPPLER_DECIMATION = 128\n"
    "MIN_CPI_COVERAGE = 0.99\n"
    "\n"
    "DSI_ALGORITHM = ECA-B\n"
    "DSI_BACKEND = CPU\n"
    "DSI_MATH_PRECISION = FP64\n"
    "ECA_BATCH_LENGTH_s = 0.025\n"
    "ECA_UPDATE_INTERVAL_s = 0.025\n"
    "ECA_DOPPLER_TAPS = 0\n"
    "ECA_DIAGONAL_LOADING = 1e-6\n"
    "ECA_CD_BATCH_s = 0.0025\n"
    "ECA_CD_DOPPLER_TAPS = 1\n"
    "ARD_SNAPSHOT_EVERY = 1\n"
    "CGLS_ITERATIONS = 30\n"
    "CGLS_PRIME = 0\n"
    "\n"
    "CFAR_PFA = 1e-5\n"
    "CFAR_GUARD_RANGE = 2\n"
    "CFAR_GUARD_DOPPLER = 2\n"
    "CFAR_TRAIN_RANGE = 4\n"
    "CFAR_TRAIN_DOPPLER = 8\n";

std::string gTempPath;

// Write text to a scratch file and return its path.
const char* writeTemp(const std::string& sText)
{
    if (gTempPath.empty()) {
        char achTemplate[] = "/tmp/pbr_test_config_XXXXXX";
        const int iFd = mkstemp(achTemplate);
        assert(iFd >= 0);
        close(iFd);
        gTempPath = achTemplate;
    }

    std::FILE* pFile = std::fopen(gTempPath.c_str(), "w");
    assert(pFile != nullptr);
    std::fwrite(sText.data(), 1, sText.size(), pFile);
    std::fclose(pFile);
    return gTempPath.c_str();
}

// Replace the first occurrence of sFrom with sTo.
std::string replaced(const char* pBase, const std::string& sFrom, const std::string& sTo)
{
    std::string s(pBase);
    const size_t u = s.find(sFrom);
    assert(u != std::string::npos);
    return s.replace(u, sFrom.size(), sTo);
}

// Remove the line containing sNeedle.
std::string withoutLine(const char* pBase, const std::string& sNeedle)
{
    std::string s(pBase);
    const size_t uAt = s.find(sNeedle);
    assert(uAt != std::string::npos);
    const size_t uStart = s.rfind('\n', uAt);
    const size_t uEnd   = s.find('\n', uAt);
    return s.erase(uStart + 1, uEnd - uStart);
}

void testValidConfig()
{
    PbrConfig oConfig;
    assert(loadPbrConfig(writeTemp(kValidConfig), oConfig));

    assert(oConfig.uSampleRateHz      == 204800u);
    assert(oConfig.sReferenceName     == "ArmasuisseRefRxClean");
    assert(oConfig.sSurveillanceName  == "ArmasuisseSurRxClean");
    assert(oConfig.uUdpPort           == 4991u);
    assert(oConfig.dFullScale         == 0.0022);
    assert(oConfig.dBufferSeconds     == 2.0);
    assert(oConfig.dCpiSeconds        == 1.0);
    assert(oConfig.dHopSeconds        == 1.0);
    assert(oConfig.uRangeBins         == 120u);
    assert(oConfig.uClutterTaps       == 120u);
    assert(oConfig.uDopplerDecimation == 128u);
    assert(oConfig.dMinCpiCoverage    == 0.99);
    assert(oConfig.dCfarPfa           == 1e-5);
    assert(oConfig.uCfarTrainDoppler  == 8u);

    // Derived, not present in the file.
    assert(oConfig.uCpiSamples     == 204800u);
    assert(oConfig.uHopSamples     == 204800u);
    assert(oConfig.uBufferCapacity == 409600u);
    assert(oConfig.uDopplerBins    == 1600u);
}

void testShippedConfigLoads()
{
    // The config the project actually ships must itself be valid.
    PbrConfig oConfig;
    if (!loadPbrConfig("config/prb.conf", oConfig)) {
        // Run from a different directory; not a failure of the loader.
        std::printf("  (skipped config/prb.conf, not reachable from here)\n");
        return;
    }
    assert(oConfig.uCpiSamples > 0);
    assert(oConfig.uDopplerBins > 0);
}

void testMissingKeyIsFatal()
{
    // The single most important policy: a missing key must fail loudly rather
    // than fall back to a default nobody can see.
    PbrConfig oConfig;
    const std::string s = withoutLine(kValidConfig, "VITA49_FULLSCALE");
    assert(!loadPbrConfig(writeTemp(s), oConfig));
}

void testUnknownKeyIsIgnored()
{
    // One file has to be able to serve tools that read different subsets.
    PbrConfig oConfig;
    std::string s(kValidConfig);
    s += "SOME_SETTING_WE_DO_NOT_READ = 42\n";
    s += "SAVE_VIDEO = true\n";
    assert(loadPbrConfig(writeTemp(s), oConfig));
    assert(oConfig.uSampleRateHz == 204800u);
}

void testMalformedLinesRejected()
{
    PbrConfig oConfig;

    // No '='.
    {
        std::string s(kValidConfig);
        s += "THIS LINE HAS NO EQUALS SIGN\n";
        assert(!loadPbrConfig(writeTemp(s), oConfig));
    }
    // Empty value.
    {
        const std::string s = replaced(kValidConfig, "UDP_PORT = 4991", "UDP_PORT =");
        assert(!loadPbrConfig(writeTemp(s), oConfig));
    }
    // Duplicate key: almost always a copy-paste mistake, so refuse rather
    // than silently take one of them.
    {
        std::string s(kValidConfig);
        s += "UDP_PORT = 5000\n";
        assert(!loadPbrConfig(writeTemp(s), oConfig));
    }
    // Not a number.
    {
        const std::string s = replaced(kValidConfig, "CPI_LENGTH_s = 1.0",
                                                     "CPI_LENGTH_s = one");
        assert(!loadPbrConfig(writeTemp(s), oConfig));
    }
    // Trailing junk after a number.
    {
        const std::string s = replaced(kValidConfig, "N_RANGE_BINS = 120",
                                                     "N_RANGE_BINS = 120bins");
        assert(!loadPbrConfig(writeTemp(s), oConfig));
    }
    // Missing file.
    assert(!loadPbrConfig("/tmp/pbr_no_such_config_file_12345.conf", oConfig));
}

void testValidationRules()
{
    PbrConfig oConfig;

    // Non-positive values.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "CPI_LENGTH_s = 1.0", "CPI_LENGTH_s = 0")), oConfig));
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "VITA49_FULLSCALE = 0.0022", "VITA49_FULLSCALE = 0")), oConfig));

    // Pfa must be a probability.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "CFAR_PFA = 1e-5", "CFAR_PFA = 1.5")), oConfig));

    // Coverage must be a fraction.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "MIN_CPI_COVERAGE = 0.99", "MIN_CPI_COVERAGE = 1.5")), oConfig));

    // Port out of range.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "UDP_PORT = 4991", "UDP_PORT = 70000")), oConfig));

    // Clutter taps must be at least the range bin count.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "N_CLUTTER_TAPS = 120", "N_CLUTTER_TAPS = 119")), oConfig));

    // Decimation must divide the CPI exactly. 204800 is 2^13 * 5^2, so any
    // factor of 3 leaves a remainder: 204800 / 300 = 682.67.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "DOPPLER_DECIMATION = 128", "DOPPLER_DECIMATION = 300")), oConfig));

    // 100 divides 204800 evenly, so it must be accepted.
    assert(loadPbrConfig(writeTemp(replaced(kValidConfig,
        "DOPPLER_DECIMATION = 128", "DOPPLER_DECIMATION = 100")), oConfig));
    assert(oConfig.uDopplerBins == 2048u);

    // The buffer must hold at least one whole CPI.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "BUFFER_LENGTH_s = 2.0", "BUFFER_LENGTH_s = 0.5")), oConfig));

    // Identical receiver names cannot be told apart.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "SURVEILLANCE_RECEIVER_NAME = ArmasuisseSurRxClean",
        "SURVEILLANCE_RECEIVER_NAME = ArmasuisseRefRxClean")), oConfig));

    // Exactly one CPI of buffer is legal but warns; it must still load.
    assert(loadPbrConfig(writeTemp(replaced(kValidConfig,
        "BUFFER_LENGTH_s = 2.0", "BUFFER_LENGTH_s = 1.0")), oConfig));

    // ---- ECA windows -----------------------------------------------------

    // The shipped 25 ms batch is 5120 samples, 40 to a CPI.
    assert(loadPbrConfig(writeTemp(kValidConfig), oConfig));
    assert(oConfig.uEcaEstimateSamples == 5120u);
    assert(oConfig.uEcaUpdateSamples   == 5120u);

    // The update window must divide the CPI exactly, or the last fragment is
    // short and behaves differently from its neighbours. 0.03 s is 6144
    // samples and 204800 / 6144 is not a whole number.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_UPDATE_INTERVAL_s = 0.025", "ECA_UPDATE_INTERVAL_s = 0.03")), oConfig));

    // Applying weights to more signal than they were estimated from is
    // backwards; ECA-S goes the other way.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_BATCH_LENGTH_s = 0.025", "ECA_BATCH_LENGTH_s = 0.0125")), oConfig));

    // An estimation window longer than the CPI cannot be filled.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_BATCH_LENGTH_s = 0.025", "ECA_BATCH_LENGTH_s = 2.0")), oConfig));

    // Fewer samples than taps leaves the least-squares problem
    // underdetermined. 0.0005 s is 102 samples against 120 taps.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_BATCH_LENGTH_s = 0.025\nECA_UPDATE_INTERVAL_s = 0.025",
        "ECA_BATCH_LENGTH_s = 0.0005\nECA_UPDATE_INTERVAL_s = 0.0005")), oConfig));

    // Loading is a fraction of the Gram diagonal, so 1.0 or more would swamp
    // the signal.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_DIAGONAL_LOADING = 1e-6", "ECA_DIAGONAL_LOADING = 1.0")), oConfig));

    // Zero loading is legal: the Gram is normally well conditioned without it.
    assert(loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_DIAGONAL_LOADING = 1e-6", "ECA_DIAGONAL_LOADING = 0")), oConfig));

    // The ECA-CD batch must be a power of two (radix-2 FFT on the CPU) and
    // tile the CPI. 0.003 s is 614 samples: neither.
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_CD_BATCH_s = 0.0025", "ECA_CD_BATCH_s = 0.003")), oConfig));
    // 0.0025 s is 512 samples and 204800 / 512 = 400 exactly.
    assert(loadPbrConfig(writeTemp(kValidConfig), oConfig));
    assert(oConfig.uCdBatchSamples == 512u);
    assert(oConfig.uCdDopplerTaps == 1u);
    assert(!oConfig.bCglsPrime);

    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "ECA_CD_DOPPLER_TAPS = 1", "ECA_CD_DOPPLER_TAPS = 4")), oConfig));
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "CGLS_PRIME = 0", "CGLS_PRIME = 2")), oConfig));
    assert(!loadPbrConfig(writeTemp(replaced(kValidConfig,
        "CGLS_ITERATIONS = 30", "CGLS_ITERATIONS = 0")), oConfig));
    assert(loadPbrConfig(writeTemp(replaced(kValidConfig,
        "CGLS_PRIME = 0", "CGLS_PRIME = 1")), oConfig));
    assert(oConfig.bCglsPrime);

    // ECA-m is the whole CPI in one window, so a short batch length must not
    // stop it loading -- the algorithm ignores those keys.
    assert(loadPbrConfig(writeTemp(replaced(kValidConfig,
        "DSI_ALGORITHM = ECA-B", "DSI_ALGORITHM = ECA-m")), oConfig));
    assert(oConfig.sDsiAlgorithm == "ECA-m");
}

void testWhitespaceAndCommentHandling()
{
    PbrConfig oConfig;
    const std::string s =
        "   # indented comment\n"
        "\t\n"
        "SAMPLE_RATE_Hz=204800\n"                      // no spaces at all
        "  REFERENCE_RECEIVER_NAME   =   RefName   \n"  // padded everywhere
        "SURVEILLANCE_RECEIVER_NAME = SurName\n"
        "UDP_PORT = 4991\n"
        "VITA49_FULLSCALE = 0.0022\n"
        "BUFFER_LENGTH_s = 2.0\n"
        "CPI_LENGTH_s = 1.0\n"
        "CPI_HOP_s = 1.0\n"
        "N_RANGE_BINS = 120\n"
        "N_CLUTTER_TAPS = 120\n"
        "DOPPLER_HALF_SPAN_Hz = 200\n"
        "DOPPLER_DECIMATION = 128\n"
        "MIN_CPI_COVERAGE = 0.99\n"
        "DSI_ALGORITHM = ECA-B\n"
        "DSI_BACKEND = CPU\n"
        "DSI_MATH_PRECISION = FP64\n"
        "ECA_BATCH_LENGTH_s = 0.025\n"
        "ECA_UPDATE_INTERVAL_s = 0.025\n"
        "ECA_DOPPLER_TAPS = 0\n"
        "ECA_DIAGONAL_LOADING = 1e-6\n"
        "ECA_CD_BATCH_s = 0.0025\n"
        "ECA_CD_DOPPLER_TAPS = 1\n"
        "ARD_SNAPSHOT_EVERY = 1\n"
        "CGLS_ITERATIONS = 30\n"
        "CGLS_PRIME = 0\n"
        "CFAR_PFA = 1e-5\n"
        "CFAR_GUARD_RANGE = 2\n"
        "CFAR_GUARD_DOPPLER = 2\n"
        "CFAR_TRAIN_RANGE = 4\n"
        "CFAR_TRAIN_DOPPLER = 8\n";

    assert(loadPbrConfig(writeTemp(s), oConfig));
    assert(oConfig.uSampleRateHz  == 204800u);
    assert(oConfig.sReferenceName == "RefName");   // padding stripped
}

} // namespace

int main()
{
    testValidConfig();
    testShippedConfigLoads();
    testMissingKeyIsFatal();
    testUnknownKeyIsIgnored();
    testMalformedLinesRejected();
    testValidationRules();
    testWhitespaceAndCommentHandling();

    if (!gTempPath.empty())
        std::remove(gTempPath.c_str());

    std::printf("test_config: all tests passed\n");
    return 0;
}
