// dsi_verify -- compare a canceller's output against a MATLAB golden file.
//
// The acceptance rule from CLAUDE.md: no C++/CUDA stage is accepted until it
// matches the MATLAB reference within relative error < 1e-4 (FP32). This tool
// applies that rule to DSI cancellation:
//
//   1. pbr_run --dump-cpi freezes one CPI to data/<scenario>/cpi_0000.bin
//   2. matlab/eca_golden.m reads those exact bytes, cancels in double
//      precision, writes data/<scenario>/golden/golden_ecam.bin and
//      golden_ecab.bin; matlab/fblms_golden.m does the same for FBLMS and
//      writes golden_fblms.bin
//   3. this tool runs the same algorithm on the same CPI and reports the
//      relative L2 error against the golden output, PASS/FAIL by exit code
//
// Both sides start from byte-identical samples, so any disagreement is in the
// arithmetic, not the data.
//
// The golden file carries the parameters it was generated with (taps, window,
// loading), and this tool REFUSES to compare if they disagree with the loaded
// config -- a stale golden file fails loudly instead of "passing" against the
// wrong settings.
//
// The error metric is relative L2 over the whole CPI:
//
//     sqrt( sum |cpp[n] - gold[n]|^2 / sum |gold[n]|^2 )
//
// rather than per-sample relative error, because the output is a residual --
// individual samples pass arbitrarily close to zero, where a per-sample ratio
// explodes on noise that means nothing.
//
// Usage:
//   dsi_verify --cpi data/S0_clean/cpi_0000.bin \
//              --golden data/S0_clean/golden/golden_ecam.bin \
//              --algorithm ECA-m [--backend CPU] [--tolerance 1e-4]

#include "pbr/Config.hpp"
#include "pbr/CpiFile.hpp"
#include "pbr/DSICanceller.hpp"
#include "pbr/types.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pbr;

namespace {

struct Options {
    std::string sCpiPath;
    std::string sGoldenPath;
    std::string sConfigPath = kDefaultConfigPath;
    std::string sAlgorithm;
    std::string sBackend;
    std::string sPrecision;
    std::string sAgainst;          // other backend to diff against
    std::string sDumpPath;         // write the cleaned CPI here
    double      dTolerance  = 1e-4;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s --cpi FILE --golden FILE [options]\n", pProgram);
    std::printf("  --cpi FILE        CPI written by pbr_run --dump-cpi (required)\n");
    std::printf("  --golden FILE     golden output from matlab/eca_golden.m or\n"
                "                    matlab/fblms_golden.m (required)\n");
    std::printf("  --config FILE     configuration (default %s)\n", kDefaultConfigPath);
    std::printf("  --algorithm NAME  override DSI_ALGORITHM\n");
    std::printf("  --backend NAME    override DSI_BACKEND\n");
    std::printf("  --precision NAME  override DSI_MATH_PRECISION: FP64 or FP32\n");
    std::printf("  --against NAME    instead of a golden file, run the same\n");
    std::printf("                    algorithm on this other backend and diff\n");
    std::printf("                    the two. Set --precision FP32 to remove\n");
    std::printf("                    precision as a variable, leaving only bugs.\n");
    std::printf("  --dump-output FILE  write (reference, cleaned) as a CPI file,\n");
    std::printf("                    which is what matlab/ard_golden.m reads\n");
    std::printf("  --tolerance X     relative L2 acceptance threshold (default 1e-4)\n");
}

bool parseArgs(int argc, char* argv[], Options& oOptions)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bHasValue   = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h") {
            return false;
        } else if (sArg == "--cpi" && bHasValue) {
            oOptions.sCpiPath = argv[++i];
        } else if (sArg == "--golden" && bHasValue) {
            oOptions.sGoldenPath = argv[++i];
        } else if (sArg == "--config" && bHasValue) {
            oOptions.sConfigPath = argv[++i];
        } else if (sArg == "--algorithm" && bHasValue) {
            oOptions.sAlgorithm = argv[++i];
        } else if (sArg == "--backend" && bHasValue) {
            oOptions.sBackend = argv[++i];
        } else if (sArg == "--precision" && bHasValue) {
            oOptions.sPrecision = argv[++i];
        } else if (sArg == "--against" && bHasValue) {
            oOptions.sAgainst = argv[++i];
        } else if (sArg == "--dump-output" && bHasValue) {
            oOptions.sDumpPath = argv[++i];
        } else if (sArg == "--tolerance" && bHasValue) {
            oOptions.dTolerance = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }

    if (oOptions.sCpiPath.empty()) {
        std::fprintf(stderr, "--cpi is required\n");
        return false;
    }
    if (oOptions.sGoldenPath.empty() == oOptions.sAgainst.empty()) {
        std::fprintf(stderr, "give exactly one of --golden FILE or --against BACKEND\n");
        return false;
    }
    if (oOptions.dTolerance <= 0.0) {
        std::fprintf(stderr, "--tolerance must be positive\n");
        return false;
    }
    return true;
}

// Golden file layout, written by matlab/eca_golden.m:
//   offset  size  contents
//        0     8  magic "PBRGLD01"
//        8     8  sample count
//       16     8  clutter taps
//       24     8  update window samples
//       32     8  1 if the loading is absolute (ECA-m), 0 if relative
//       40     8  loading value, float64
//       48   16N  cleaned surveillance, interleaved float32 I,Q
struct GoldenHeader {
    uint64_t uSampleCount     = 0;
    uint64_t uClutterTaps     = 0;
    uint64_t uUpdateSamples   = 0;
    uint64_t uAbsoluteLoading = 0;
    double   dLoading         = 0.0;
};

bool readGolden(const char* pPath, GoldenHeader& oHeader,
                std::vector<ComplexF>& vcOut)
{
    std::FILE* pFile = std::fopen(pPath, "rb");
    if (pFile == nullptr) {
        std::fprintf(stderr, "golden: cannot open %s: %s\n"
                             "golden: make it from the repo root with\n"
                             "  matlab -batch \"addpath('matlab'); "
                             "eca_golden('config/fers.conf', 'data/<scenario>')\"\n",
                     pPath, std::strerror(errno));
        return false;
    }

    char achMagic[8];
    bool bOk = std::fread(achMagic, 1, sizeof(achMagic), pFile) == sizeof(achMagic) &&
               std::memcmp(achMagic, "PBRGLD01", sizeof(achMagic)) == 0 &&
               std::fread(&oHeader.uSampleCount,     8, 1, pFile) == 1 &&
               std::fread(&oHeader.uClutterTaps,     8, 1, pFile) == 1 &&
               std::fread(&oHeader.uUpdateSamples,   8, 1, pFile) == 1 &&
               std::fread(&oHeader.uAbsoluteLoading, 8, 1, pFile) == 1 &&
               std::fread(&oHeader.dLoading,         8, 1, pFile) == 1 &&
               oHeader.uSampleCount > 0 && oHeader.uSampleCount <= (1ULL << 32);

    if (bOk) {
        vcOut.assign(static_cast<size_t>(oHeader.uSampleCount), ComplexF());
        bOk = std::fread(vcOut.data(), sizeof(ComplexF), vcOut.size(), pFile) ==
              vcOut.size();
    }

    std::fclose(pFile);
    if (!bOk)
        std::fprintf(stderr, "golden: %s is not a valid golden file "
                             "(bad magic or truncated)\n", pPath);
    return bOk;
}

// The input CPI, which must hold exactly one CPI of the config's length.
bool loadCpi(const std::string& sPath, uint64_t uCpiSamples, CpiFileHeader& oHeader,
             std::vector<ComplexF>& vcRef, std::vector<ComplexF>& vcSur)
{
    if (!readCpiFileHeader(sPath.c_str(), oHeader))
        return false;
    if (oHeader.uSampleCount != uCpiSamples) {
        std::fprintf(stderr, "dsi_verify: CPI file holds %llu samples but the "
                             "config says %llu\n",
                     (unsigned long long)oHeader.uSampleCount,
                     (unsigned long long)uCpiSamples);
        return false;
    }
    vcRef.assign(static_cast<size_t>(oHeader.uSampleCount), ComplexF());
    vcSur.assign(static_cast<size_t>(oHeader.uSampleCount), ComplexF());
    return readCpiFile(sPath.c_str(), oHeader, vcRef.data(), vcSur.data(),
                       oHeader.uSampleCount);
}

// The header a golden file should carry for this run's algorithm. FBLMS has
// no ECA windows, so its taps and block length fill the ECA slots and its
// step fills the loading. The forgetting factor is not in the header; the
// generator reads it from the same config.
GoldenHeader expectedHeader(const DSIParams& oParams)
{
    GoldenHeader oHeader;
    oHeader.uSampleCount = oParams.uCpiSamples;
    if (oParams.eAlgorithm == DSIAlgorithm::Fblms) {
        oHeader.uClutterTaps     = oParams.uFblmsTaps;
        oHeader.uUpdateSamples   = oParams.uFblmsTaps;
        oHeader.uAbsoluteLoading = 0;
        oHeader.dLoading         = oParams.dFblmsStep;
    } else {
        oHeader.uClutterTaps     = oParams.uClutterTaps;
        oHeader.uUpdateSamples   = oParams.uUpdateSamples;
        oHeader.uAbsoluteLoading = oParams.bAbsoluteLoading ? 1 : 0;
        oHeader.dLoading         = oParams.dDiagonalLoading;
    }
    return oHeader;
}

// The output to compare against: the same algorithm on the --against
// backend, described by a header made from its parameters.
//
// Backend-vs-backend is the stronger test for a GPU port. A golden file can
// only be compared at a tolerance loose enough to absorb the single-precision
// difference (~5e-3), and that is exactly the range where an off-by-one or a
// race would hide. Running the CPU in FP32 mode against the GPU removes
// precision as a variable, so what is left is structure.
bool runOtherBackend(const PbrConfig& oCfg, const std::string& sBackend,
                     const std::vector<ComplexF>& vcRef, const std::vector<ComplexF>& vcSur,
                     GoldenHeader& oHeader, std::vector<ComplexF>& vcOut, std::string& sNameOut)
{
    PbrConfig oOther = oCfg;
    oOther.sDsiBackend = sBackend;

    DSIParams oParams;
    if (!dsiParamsFromConfig(oOther, oParams))
        return false;
    std::unique_ptr<DSICanceller> pOther = makeDSICanceller(oParams.eAlgorithm, oParams.eBackend);
    if (!pOther || !pOther->init(oParams))
        return false;

    vcOut.assign(vcRef.size(), ComplexF());
    DSIStats oStats;
    if (!pOther->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats))
        return false;

    sNameOut                 = pOther->name();
    oHeader = expectedHeader(oParams);
    return true;
}

// The golden output must have been made with THESE parameters. Comparing
// against one made with different settings would "fail" a correct
// implementation or, worse, pass a wrong one.
bool goldenMatches(const GoldenHeader& oGolden, const DSIParams& oParams,
                   const std::string& sGoldenName)
{
    const GoldenHeader oWant = expectedHeader(oParams);
    const bool bMatch =
        oGolden.uSampleCount == oWant.uSampleCount &&
        oGolden.uClutterTaps == oWant.uClutterTaps &&
        oGolden.uUpdateSamples == oWant.uUpdateSamples &&
        oGolden.uAbsoluteLoading == oWant.uAbsoluteLoading &&
        std::fabs(oGolden.dLoading - oWant.dLoading) <=
            1e-12 * std::fabs(oWant.dLoading);
    if (bMatch)
        return true;

    std::fprintf(stderr,
        "dsi_verify: %s was generated with different parameters than the "
        "current run:\n"
        "              golden                current (%s)\n"
        "  samples     %-20llu  %llu\n"
        "  taps        %-20llu  %llu\n"
        "  window      %-20llu  %llu\n"
        "  loading/step %g (%s)   %g (%s)\n"
        "Re-run matlab/eca_golden.m (matlab/fblms_golden.m for FBLMS), or pick the matching --algorithm.\n",
        sGoldenName.c_str(), dsiAlgorithmName(oParams.eAlgorithm),
        (unsigned long long)oGolden.uSampleCount,
        (unsigned long long)oWant.uSampleCount,
        (unsigned long long)oGolden.uClutterTaps,
        (unsigned long long)oWant.uClutterTaps,
        (unsigned long long)oGolden.uUpdateSamples,
        (unsigned long long)oWant.uUpdateSamples,
        oGolden.dLoading, oGolden.uAbsoluteLoading ? "absolute" : "relative",
        oWant.dLoading, oWant.uAbsoluteLoading ? "absolute" : "relative");
    return false;
}

double powerOf(const std::vector<ComplexF>& vc)
{
    double dPower = 0.0;
    for (size_t u = 0; u < vc.size(); ++u)
        dPower += double(vc[u].real()) * vc[u].real() + double(vc[u].imag()) * vc[u].imag();
    return dPower;
}

} // namespace

int main(int argc, char* argv[])
{
    Options oOptions;
    if (!parseArgs(argc, argv, oOptions)) {
        printUsage(argv[0]);
        return 1;
    }

    PbrConfig oCfg;
    if (!loadPbrConfig(oOptions.sConfigPath.c_str(), oCfg))
        return 1;
    if (!oOptions.sAlgorithm.empty())
        oCfg.sDsiAlgorithm = oOptions.sAlgorithm;
    if (!oOptions.sBackend.empty())
        oCfg.sDsiBackend = oOptions.sBackend;
    if (!oOptions.sPrecision.empty())
        oCfg.sDsiMathPrecision = oOptions.sPrecision;

    DSIParams oParams;
    if (!dsiParamsFromConfig(oCfg, oParams))
        return 1;

    // ---- the input and the output to compare against ------------------------
    CpiFileHeader         oCpiHeader;
    std::vector<ComplexF> vcRef, vcSur;
    if (!loadCpi(oOptions.sCpiPath, oParams.uCpiSamples, oCpiHeader, vcRef, vcSur))
        return 1;

    GoldenHeader          oGolden;
    std::vector<ComplexF> vcGold;
    std::string           sReferenceName = oOptions.sGoldenPath;
    if (!oOptions.sAgainst.empty()) {
        if (!runOtherBackend(oCfg, oOptions.sAgainst, vcRef, vcSur, oGolden, vcGold,
                             sReferenceName))
            return 1;
    } else if (!readGolden(oOptions.sGoldenPath.c_str(), oGolden, vcGold)) {
        return 1;
    }
    if (!goldenMatches(oGolden, oParams, sReferenceName))
        return 1;

    // ---- run the canceller --------------------------------------------------
    std::unique_ptr<DSICanceller> pCanceller =
        makeDSICanceller(oParams.eAlgorithm, oParams.eBackend);
    if (!pCanceller || !pCanceller->init(oParams))
        return 1;

    std::vector<ComplexF> vcOut(static_cast<size_t>(oCpiHeader.uSampleCount));
    DSIStats oStats;
    if (!pCanceller->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats))
        return 1;

    // ---- compare ------------------------------------------------------------
    // Golden suppression, from the same input power, for the side-by-side.
    const double dSurPower  = powerOf(vcSur);
    const double dGoldPower = powerOf(vcGold);
    const double dGoldSuppressionDb =
        (dGoldPower > 0.0) ? 10.0 * std::log10(dSurPower / dGoldPower) : 0.0;

    double dDiffSq = 0.0;
    double dMaxSq  = 0.0;
    for (size_t u = 0; u < vcGold.size(); ++u) {
        const double dRe = double(vcOut[u].real()) - vcGold[u].real();
        const double dIm = double(vcOut[u].imag()) - vcGold[u].imag();
        const double dSq = dRe * dRe + dIm * dIm;
        dDiffSq += dSq;
        if (dSq > dMaxSq)
            dMaxSq = dSq;
    }
    const double dRelL2    = std::sqrt(dDiffSq / dGoldPower);
    const double dRms      = std::sqrt(dGoldPower / double(vcGold.size()));
    const double dMaxVsRms = std::sqrt(dMaxSq) / dRms;
    const bool   bPass     = dRelL2 < oOptions.dTolerance;

    std::printf("dsi_verify: %s against %s\n",
                pCanceller->name(), sReferenceName.c_str());
    std::printf("  suppression   %.4f dB   reference %.4f dB\n",
                oStats.dSuppressionDb, dGoldSuppressionDb);
    std::printf("  relative L2   %.3e   (tolerance %g)\n", dRelL2, oOptions.dTolerance);
    std::printf("  worst sample  %.3e   (vs the golden RMS)\n", dMaxVsRms);

    // The cleaned CPI is what the range-Doppler stage consumes. Written in
    // the SAME format as the input CPI -- both hold two aligned channels, so
    // (reference, cleaned) fits with no new format and MATLAB reads it with
    // the loader it already has.
    if (!oOptions.sDumpPath.empty()) {
        if (!writeCpiFile(oOptions.sDumpPath.c_str(), oCpiHeader,
                          vcRef.data(), vcOut.data()))
            return 1;
        std::printf("wrote (reference, cleaned) to %s\n",
                    oOptions.sDumpPath.c_str());
    }

    std::printf("%s\n", bPass
        ? "PASS -- the two agree within tolerance."
        : "FAIL -- the outputs disagree beyond tolerance.");
    return bPass ? 0 : 1;
}
