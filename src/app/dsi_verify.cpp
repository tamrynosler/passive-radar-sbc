// dsi_verify -- compare a canceller's output against a MATLAB golden file.
//
// The acceptance rule from CLAUDE.md: no C++/CUDA stage is accepted until it
// matches the MATLAB reference within relative error < 1e-4 (FP32). This tool
// applies that rule to DSI cancellation:
//
//   1. ingest_replay --dump-cpi freezes one CPI to data/cpi_0000.bin
//   2. matlab/eca_golden.m reads those exact bytes, cancels in double
//      precision, writes data/golden_ecam.bin and data/golden_ecab.bin
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
//   dsi_verify --cpi data/cpi_0000.bin --golden data/golden_ecam.bin \
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
#include <string>
#include <vector>

using namespace pbr;

namespace {

const char* const kDefaultConfigPath = "config/prb.conf";

struct Options {
    std::string sCpiPath;
    std::string sGoldenPath;
    std::string sConfigPath = kDefaultConfigPath;
    std::string sAlgorithm;
    std::string sBackend;
    std::string sPrecision;
    std::string sAgainst;          // other backend to diff against
    double      dTolerance  = 1e-4;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s --cpi FILE --golden FILE [options]\n", pProgram);
    std::printf("  --cpi FILE        CPI dumped by ingest_replay --dump-cpi (required)\n");
    std::printf("  --golden FILE     golden output from matlab/eca_golden.m (required)\n");
    std::printf("  --config FILE     configuration (default %s)\n", kDefaultConfigPath);
    std::printf("  --algorithm NAME  override DSI_ALGORITHM\n");
    std::printf("  --backend NAME    override DSI_BACKEND\n");
    std::printf("  --precision NAME  override DSI_MATH_PRECISION: FP64 or FP32\n");
    std::printf("  --against NAME    instead of a golden file, run the same\n");
    std::printf("                    algorithm on this other backend and diff\n");
    std::printf("                    the two. Set --precision FP32 to remove\n");
    std::printf("                    precision as a variable, leaving only bugs.\n");
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
                             "golden: run  matlab -batch \"run('matlab/eca_golden.m')\"  "
                             "from the repo root first\n",
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

    // ---- load both files ----------------------------------------------------
    CpiFileHeader oCpiHeader;
    if (!readCpiFileHeader(oOptions.sCpiPath.c_str(), oCpiHeader))
        return 1;
    if (oCpiHeader.uSampleCount != oParams.uCpiSamples) {
        std::fprintf(stderr, "dsi_verify: CPI file holds %llu samples but the "
                             "config says %llu\n",
                     (unsigned long long)oCpiHeader.uSampleCount,
                     (unsigned long long)oParams.uCpiSamples);
        return 1;
    }

    std::vector<ComplexF> vcRef(static_cast<size_t>(oCpiHeader.uSampleCount));
    std::vector<ComplexF> vcSur(static_cast<size_t>(oCpiHeader.uSampleCount));
    if (!readCpiFile(oOptions.sCpiPath.c_str(), oCpiHeader,
                     vcRef.data(), vcSur.data(), oCpiHeader.uSampleCount))
        return 1;

    // The reference output: either a MATLAB golden file, or the same
    // algorithm run on the other backend.
    //
    // Backend-vs-backend is the stronger test for a GPU port. A golden file
    // can only be compared at a tolerance loose enough to absorb the
    // single-precision difference (~5e-3), and that is exactly the range
    // where an off-by-one or a race would hide. Running the CPU in FP32 mode
    // against the GPU removes precision as a variable, so what is left is
    // structure.
    GoldenHeader          oGolden;
    std::vector<ComplexF> vcGold;
    std::string           sReferenceName = oOptions.sGoldenPath;

    if (!oOptions.sAgainst.empty()) {
        PbrConfig oOther = oCfg;
        oOther.sDsiBackend = oOptions.sAgainst;

        DSIParams oOtherParams;
        if (!dsiParamsFromConfig(oOther, oOtherParams))
            return 1;

        std::unique_ptr<DSICanceller> pOther =
            makeDSICanceller(oOtherParams.eAlgorithm, oOtherParams.eBackend);
        if (!pOther || !pOther->init(oOtherParams))
            return 1;

        vcGold.assign(static_cast<size_t>(oCpiHeader.uSampleCount), ComplexF());
        DSIStats oOtherStats;
        if (!pOther->cancel(vcRef.data(), vcSur.data(), vcGold.data(), oOtherStats))
            return 1;

        sReferenceName = pOther->name();
        oGolden.uSampleCount     = oOtherParams.uCpiSamples;
        oGolden.uClutterTaps     = oOtherParams.uClutterTaps;
        oGolden.uUpdateSamples   = oOtherParams.uUpdateSamples;
        oGolden.uAbsoluteLoading = oOtherParams.bAbsoluteLoading ? 1 : 0;
        oGolden.dLoading         = oOtherParams.dDiagonalLoading;
    } else if (!readGolden(oOptions.sGoldenPath.c_str(), oGolden, vcGold)) {
        return 1;
    }

    // ---- the golden file must have been generated with THESE parameters ----
    // Comparing against a golden file made with different settings would
    // "fail" a correct implementation or, worse, pass a wrong one.
    bool bMatch = true;
    if (oGolden.uSampleCount != oParams.uCpiSamples)     bMatch = false;
    if (oGolden.uClutterTaps != oParams.uClutterTaps)    bMatch = false;
    if (oGolden.uUpdateSamples != oParams.uUpdateSamples) bMatch = false;
    if ((oGolden.uAbsoluteLoading != 0) != oParams.bAbsoluteLoading) bMatch = false;
    if (std::fabs(oGolden.dLoading - oParams.dDiagonalLoading) >
        1e-12 * std::fabs(oParams.dDiagonalLoading))     bMatch = false;

    if (!bMatch) {
        std::fprintf(stderr,
            "dsi_verify: %s was generated with different parameters than the "
            "current run:\n"
            "              golden                current (%s)\n"
            "  samples     %-20llu  %llu\n"
            "  taps        %-20llu  %llu\n"
            "  window      %-20llu  %llu\n"
            "  loading     %g (%s)   %g (%s)\n"
            "Re-run matlab/eca_golden.m, or pick the matching --algorithm.\n",
            sReferenceName.c_str(), dsiAlgorithmName(oParams.eAlgorithm),
            (unsigned long long)oGolden.uSampleCount,
            (unsigned long long)oParams.uCpiSamples,
            (unsigned long long)oGolden.uClutterTaps,
            (unsigned long long)oParams.uClutterTaps,
            (unsigned long long)oGolden.uUpdateSamples,
            (unsigned long long)oParams.uUpdateSamples,
            oGolden.dLoading, oGolden.uAbsoluteLoading ? "absolute" : "relative",
            oParams.dDiagonalLoading, oParams.bAbsoluteLoading ? "absolute" : "relative");
        return 1;
    }

    // ---- run the canceller --------------------------------------------------
    std::unique_ptr<DSICanceller> pCanceller =
        makeDSICanceller(oParams.eAlgorithm, oParams.eBackend);
    if (!pCanceller || !pCanceller->init(oParams))
        return 1;

    std::vector<ComplexF> vcOut(static_cast<size_t>(oCpiHeader.uSampleCount));
    DSIStats oStats;
    if (!pCanceller->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats))
        return 1;

    // Golden suppression, from the same input power, for the side-by-side.
    double dSurPower  = 0.0;
    double dGoldPower = 0.0;
    for (size_t u = 0; u < vcGold.size(); ++u) {
        dSurPower  += double(vcSur[u].real())  * vcSur[u].real() +
                      double(vcSur[u].imag())  * vcSur[u].imag();
        dGoldPower += double(vcGold[u].real()) * vcGold[u].real() +
                      double(vcGold[u].imag()) * vcGold[u].imag();
    }
    const double dGoldSuppressionDb =
        (dGoldPower > 0.0) ? 10.0 * std::log10(dSurPower / dGoldPower) : 0.0;

    // ---- compare ------------------------------------------------------------
    double dDiffSq = 0.0;
    double dMaxAbs = 0.0;
    for (size_t u = 0; u < vcGold.size(); ++u) {
        const double dRe = double(vcOut[u].real()) - vcGold[u].real();
        const double dIm = double(vcOut[u].imag()) - vcGold[u].imag();
        const double dSq = dRe * dRe + dIm * dIm;
        dDiffSq += dSq;
        if (dSq > dMaxAbs)
            dMaxAbs = dSq;
    }
    const double dRelL2 = std::sqrt(dDiffSq / dGoldPower);
    const double dRms   = std::sqrt(dGoldPower / double(vcGold.size()));
    const double dMaxVsRms = std::sqrt(dMaxAbs) / dRms;

    const bool bPass = dRelL2 < oOptions.dTolerance;

    std::printf("dsi_verify: %s against %s\n",
                pCanceller->name(), sReferenceName.c_str());
    std::printf("  suppression   %.4f dB   reference %.4f dB\n",
                oStats.dSuppressionDb, dGoldSuppressionDb);
    std::printf("  relative L2   %.3e   (tolerance %g)\n", dRelL2, oOptions.dTolerance);
    std::printf("  worst sample  %.3e   (vs the golden RMS)\n", dMaxVsRms);
    std::printf("%s\n", bPass
        ? "PASS -- the two agree within tolerance."
        : "FAIL -- the outputs disagree beyond tolerance.");

    return bPass ? 0 : 1;
}
