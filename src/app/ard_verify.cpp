// ard_verify -- check the range-Doppler stage against a MATLAB golden file,
// or against the other backend.
//
// Three checks, because a single end-to-end comparison cannot say WHICH half
// is wrong:
//
//   --stage caf    compare the complex map only. Isolates map formation.
//   --stage cfar   read a golden MAP as input and run only the detector, so
//                  any disagreement is in the detector rather than in the map
//                  that fed it.
//   --stage both   cleaned CPI in, detections out. The chain as it will run.
//
// Inputs come from:
//   ./bin/dsi_verify --cpi data/cpi_0000.bin --golden data/golden_ecab.bin \
//                    --algorithm ECA-B --dump-output data/cleaned_0000.bin
//   matlab -batch "addpath('matlab'); ard_golden"
//
// Map agreement is relative L2 over all cells. Detection agreement is exact
// on the set of (range bin, Doppler bin) pairs: a detector that finds a
// different cell is wrong, not slightly off.

#include "pbr/ArdFile.hpp"
#include "pbr/ArdProcessor.hpp"
#include "pbr/Config.hpp"
#include "pbr/DSICanceller.hpp"
#include "pbr/CpiFile.hpp"
#include "pbr/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace pbr;

namespace {

const char* const kDefaultConfigPath = "config/prb.conf";
const uint64_t    kMaxDetections     = 8192;

struct Options {
    std::string sCleanedPath;      // (reference, cleaned) CPI
    std::string sRawCpiPath;       // raw CPI, for --chained
    bool        bChained    = false;
    std::string sGoldenPath;       // MATLAB ARD file
    std::string sConfigPath = kDefaultConfigPath;
    std::string sBackend;
    std::string sStage      = "both";
    double      dTolerance  = 1e-4;
    double      dMarginDb   = 0.5;
};

bool bToleranceGiven = false;

void printUsage(const char* pProgram)
{
    std::printf("usage: %s --cleaned FILE --golden FILE [options]\n", pProgram);
    std::printf("  --cleaned FILE   (reference, cleaned) CPI from\n");
    std::printf("                   dsi_verify --dump-output\n");
    std::printf("  --golden FILE    ARD file from matlab/ard_golden.m\n");
    std::printf("  --stage NAME     caf | cfar | both   (default both)\n");
    std::printf("  --backend NAME   CPU or GPU (default: from the config)\n");
    std::printf("  --config FILE    configuration (default %s)\n", kDefaultConfigPath);
    std::printf("  --cpi FILE       raw CPI, used with --chained\n");
    std::printf("  --chained        run the canceller and the ARD stage\n");
    std::printf("                   DEVICE-COUPLED, which is the exact path\n");
    std::printf("                   pbr_run uses, and compare end to end\n");
    std::printf("  --tolerance X    relative L2 acceptance for the map (default 1e-4)\n");
    std::printf("  --margin X       a detection difference is only a failure if\n");
    std::printf("                   the cell cleared its threshold by more than\n");
    std::printf("                   this many dB (default 0.5)\n");
}

bool parseArgs(int argc, char* argv[], Options& oOptions)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bHasValue   = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h") {
            return false;
        } else if (sArg == "--cleaned" && bHasValue) {
            oOptions.sCleanedPath = argv[++i];
        } else if (sArg == "--cpi" && bHasValue) {
            oOptions.sRawCpiPath = argv[++i];
        } else if (sArg == "--chained") {
            oOptions.bChained = true;
        } else if (sArg == "--golden" && bHasValue) {
            oOptions.sGoldenPath = argv[++i];
        } else if (sArg == "--stage" && bHasValue) {
            oOptions.sStage = argv[++i];
        } else if (sArg == "--backend" && bHasValue) {
            oOptions.sBackend = argv[++i];
        } else if (sArg == "--config" && bHasValue) {
            oOptions.sConfigPath = argv[++i];
        } else if (sArg == "--tolerance" && bHasValue) {
            oOptions.dTolerance = std::atof(argv[++i]);
            bToleranceGiven = true;
        } else if (sArg == "--margin" && bHasValue) {
            oOptions.dMarginDb = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }

    if (oOptions.sGoldenPath.empty()) {
        std::fprintf(stderr, "--golden is required\n");
        return false;
    }
    if (oOptions.sStage != "caf" && oOptions.sStage != "cfar" &&
        oOptions.sStage != "both") {
        std::fprintf(stderr, "--stage must be caf, cfar or both\n");
        return false;
    }
    if (oOptions.bChained) {
        if (oOptions.sRawCpiPath.empty()) {
            std::fprintf(stderr, "--chained needs --cpi with the RAW CPI\n");
            return false;
        }
    } else if (oOptions.sStage != "cfar" && oOptions.sCleanedPath.empty()) {
        std::fprintf(stderr, "--cleaned is required unless --stage cfar\n");
        return false;
    }
    return true;
}

double relativeL2(const std::vector<ComplexF>& vcA, const std::vector<ComplexF>& vcB)
{
    double dDiff = 0.0, dRef = 0.0;
    for (size_t u = 0; u < vcA.size(); ++u) {
        const double dRe = double(vcA[u].real()) - vcB[u].real();
        const double dIm = double(vcA[u].imag()) - vcB[u].imag();
        dDiff += dRe * dRe + dIm * dIm;
        dRef  += double(vcB[u].real()) * vcB[u].real() +
                 double(vcB[u].imag()) * vcB[u].imag();
    }
    return (dRef > 0.0) ? std::sqrt(dDiff / dRef) : 0.0;
}

typedef std::set<std::pair<uint64_t, uint64_t> > CellSet;

// How far above its own threshold a detection sat, or -1 if it is not in the
// list at all. A cell that cleared the threshold by hundredths of a dB is a
// coin toss: the smallest difference in the map flips it either way, so a
// disagreement there says nothing about correctness. A cell that cleared it
// by several dB and then went missing is a real fault.
double marginOf(const Detection* pDet, uint64_t uCount,
                uint64_t uRangeBin, uint64_t uDopplerBin)
{
    for (uint64_t u = 0; u < uCount; ++u) {
        if (pDet[u].uRangeBin == uRangeBin && pDet[u].uDopplerBin == uDopplerBin)
            return pDet[u].dSnrDb;
    }
    return -1.0;
}

CellSet toCellSet(const Detection* pDet, uint64_t uCount)
{
    CellSet s;
    for (uint64_t u = 0; u < uCount; ++u)
        s.insert(std::make_pair(pDet[u].uRangeBin, pDet[u].uDopplerBin));
    return s;
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
    if (!oOptions.sBackend.empty())
        oCfg.sDsiBackend = oOptions.sBackend;

    ArdParams oParams;
    if (!ardParamsFromConfig(oCfg, oParams))
        return 1;

    // ---- the golden file, and the parameters it was made with -------------
    ArdFileHeader oGoldenHeader;
    if (!readArdFileHeader(oOptions.sGoldenPath.c_str(), oGoldenHeader))
        return 1;

    if (oGoldenHeader.uRangeBins != oParams.uRangeBins ||
        oGoldenHeader.uDopplerBins != oParams.uDopplerBins ||
        oGoldenHeader.uDopplerDecimation != oParams.uDopplerDecimation) {
        std::fprintf(stderr,
            "ard_verify: %s was generated with different parameters:\n"
            "                    golden     current\n"
            "  range bins        %-10llu %llu\n"
            "  doppler bins      %-10llu %llu\n"
            "  decimation        %-10llu %llu\n"
            "Re-run matlab/ard_golden.m.\n",
            oOptions.sGoldenPath.c_str(),
            (unsigned long long)oGoldenHeader.uRangeBins,
            (unsigned long long)oParams.uRangeBins,
            (unsigned long long)oGoldenHeader.uDopplerBins,
            (unsigned long long)oParams.uDopplerBins,
            (unsigned long long)oGoldenHeader.uDopplerDecimation,
            (unsigned long long)oParams.uDopplerDecimation);
        return 1;
    }

    const uint64_t uCells = oParams.uRangeBins * oParams.uDopplerBins;
    std::vector<ComplexF> vcGoldenMap(static_cast<size_t>(uCells));
    std::vector<Detection> vGoldenDet(static_cast<size_t>(kMaxDetections));
    if (!readArdFile(oOptions.sGoldenPath.c_str(), oGoldenHeader,
                     vcGoldenMap.data(), uCells,
                     vGoldenDet.data(), kMaxDetections))
        return 1;

    // ---- run our side ------------------------------------------------------
    std::unique_ptr<ArdProcessor> pArd = makeArdProcessor(oParams.eBackend);
    if (!pArd || !pArd->init(oParams))
        return 1;

    std::vector<ComplexF>  vcMap(static_cast<size_t>(uCells));
    std::vector<Detection> vDet(static_cast<size_t>(kMaxDetections));
    ArdStats oStats;

    std::string sPathLabel = pArd->name();

    if (oOptions.bChained) {
        // Validating the chained path takes TWO comparisons, because there are
        // two different questions and one number cannot answer both.
        //
        //   1. Is the device coupling correct? Run the same GPU code twice --
        //      once with the cleaned samples round-tripping through the host,
        //      once reading the canceller's device buffer directly. The
        //      arithmetic is identical either way, so the two must agree
        //      EXACTLY. Anything else means processDevice() is reading the
        //      wrong memory. This is the real test of the coupling.
        //
        //   2. Does the whole chain still match MATLAB? It does, but more
        //      loosely than the ARD stage alone, because it inherits the
        //      canceller's single-precision error (~6.5e-3). That is expected
        //      propagation rather than a new fault, so --chained defaults to
        //      the canceller's own accepted tolerance rather than the ARD's.
        DSIParams oDsiParams;
        if (!dsiParamsFromConfig(oCfg, oDsiParams))
            return 1;

        CpiFileHeader oCpiHeader;
        if (!readCpiFileHeader(oOptions.sRawCpiPath.c_str(), oCpiHeader))
            return 1;
        if (oCpiHeader.uSampleCount != oDsiParams.uCpiSamples) {
            std::fprintf(stderr, "ard_verify: %s holds %llu samples, config "
                                 "says %llu\n", oOptions.sRawCpiPath.c_str(),
                         (unsigned long long)oCpiHeader.uSampleCount,
                         (unsigned long long)oDsiParams.uCpiSamples);
            return 1;
        }

        std::vector<ComplexF> vcRef(static_cast<size_t>(oCpiHeader.uSampleCount));
        std::vector<ComplexF> vcSur(static_cast<size_t>(oCpiHeader.uSampleCount));
        if (!readCpiFile(oOptions.sRawCpiPath.c_str(), oCpiHeader,
                         vcRef.data(), vcSur.data(), oCpiHeader.uSampleCount))
            return 1;

        std::unique_ptr<DSICanceller> pDsi =
            makeDSICanceller(oDsiParams.eAlgorithm, oDsiParams.eBackend);
        if (!pDsi || !pDsi->init(oDsiParams))
            return 1;

        DSIStats oDsiStats;

        // --- path A: through the host -------------------------------------
        std::vector<ComplexF> vcCleaned(static_cast<size_t>(oCpiHeader.uSampleCount));
        std::vector<ComplexF> vcMapHost(static_cast<size_t>(uCells));
        std::vector<Detection> vDetHost(static_cast<size_t>(kMaxDetections));
        ArdStats oStatsHost;

        pDsi->setHostOutputEnabled(true);
        if (!pDsi->cancel(vcRef.data(), vcSur.data(), vcCleaned.data(), oDsiStats))
            return 1;
        if (!pArd->process(vcRef.data(), vcCleaned.data(), vcMapHost.data(),
                           vDetHost.data(), kMaxDetections, oStatsHost))
            return 1;

        // --- path B: device-coupled ---------------------------------------
        if (pDsi->deviceOutput() != nullptr) {
            pDsi->setHostOutputEnabled(false);
            if (!pDsi->cancel(vcRef.data(), vcSur.data(), nullptr, oDsiStats))
                return 1;
            if (!pArd->processDevice(pDsi->deviceReference(), pDsi->deviceOutput(),
                                     vcMap.data(), vDet.data(), kMaxDetections,
                                     oStats))
                return 1;

            // The coupling check. Same code, same inputs, different plumbing.
            uint64_t uCellsDiffer = 0;
            for (uint64_t u = 0; u < uCells; ++u) {
                if (vcMap[u].real() != vcMapHost[u].real() ||
                    vcMap[u].imag() != vcMapHost[u].imag())
                    ++uCellsDiffer;
            }
            std::printf("  coupling check    %llu of %llu map cells differ "
                        "from the host round-trip\n",
                        (unsigned long long)uCellsDiffer,
                        (unsigned long long)uCells);
            std::printf("  detections        %llu device-coupled, %llu via host\n",
                        (unsigned long long)oStats.uDetections,
                        (unsigned long long)oStatsHost.uDetections);
            if (uCellsDiffer != 0 ||
                oStats.uDetections != oStatsHost.uDetections) {
                std::fprintf(stderr,
                    "ard_verify: the device-coupled path disagrees with the "
                    "host round-trip. These run identical arithmetic, so any "
                    "difference means processDevice() is reading the wrong "
                    "memory.\n");
                return 1;
            }
            sPathLabel = std::string(pDsi->name()) + " -> " + pArd->name() +
                         ", device-coupled";
        } else {
            // No device to couple through. Report honestly rather than
            // implying the coupling was exercised.
            vcMap = vcMapHost;
            vDet  = vDetHost;
            oStats = oStatsHost;
            sPathLabel = std::string(pDsi->name()) + " -> " + pArd->name() +
                         ", via host (no device to couple)";
        }

        std::printf("  cancellation      %.4f dB suppression\n",
                    oDsiStats.dSuppressionDb);

        // The chain inherits the canceller's error, so it is held to the
        // canceller's tolerance unless the caller says otherwise.
        if (!bToleranceGiven)
            oOptions.dTolerance = 1e-2;
    } else if (oOptions.sStage == "cfar") {
        // Feed the detector MATLAB's own map, so nothing about map formation
        // can influence the result.
        if (!pArd->detect(vcGoldenMap.data(), vDet.data(), kMaxDetections, oStats))
            return 1;
    } else {
        CpiFileHeader oCpiHeader;
        if (!readCpiFileHeader(oOptions.sCleanedPath.c_str(), oCpiHeader))
            return 1;
        if (oCpiHeader.uSampleCount != oParams.uCpiSamples) {
            std::fprintf(stderr, "ard_verify: %s holds %llu samples but the "
                                 "config says a CPI is %llu\n",
                         oOptions.sCleanedPath.c_str(),
                         (unsigned long long)oCpiHeader.uSampleCount,
                         (unsigned long long)oParams.uCpiSamples);
            return 1;
        }

        std::vector<ComplexF> vcRef(static_cast<size_t>(oCpiHeader.uSampleCount));
        std::vector<ComplexF> vcCleaned(static_cast<size_t>(oCpiHeader.uSampleCount));
        if (!readCpiFile(oOptions.sCleanedPath.c_str(), oCpiHeader,
                         vcRef.data(), vcCleaned.data(), oCpiHeader.uSampleCount))
            return 1;

        if (!pArd->process(vcRef.data(), vcCleaned.data(), vcMap.data(),
                           vDet.data(), kMaxDetections, oStats))
            return 1;
    }

    // ---- compare -----------------------------------------------------------
    std::printf("ard_verify: %s, stage %s, against %s\n",
                sPathLabel.c_str(),
                oOptions.bChained ? "chained" : oOptions.sStage.c_str(),
                oOptions.sGoldenPath.c_str());

    bool bPass = true;

    if (oOptions.bChained || oOptions.sStage != "cfar") {
        const double dRel = relativeL2(vcMap, vcGoldenMap);
        std::printf("  map relative L2   %.3e   (tolerance %g)\n",
                    dRel, oOptions.dTolerance);
        std::printf("  peak              %.4f dB\n", oStats.dPeakDb);
        if (!(dRel < oOptions.dTolerance))
            bPass = false;
    }

    if (oOptions.bChained || oOptions.sStage != "caf") {
        const CellSet oMine   = toCellSet(vDet.data(), oStats.uDetections);
        const CellSet oGolden = toCellSet(vGoldenDet.data(), oGoldenHeader.uDetections);

        std::vector<std::pair<uint64_t, uint64_t> > vMissed, vExtra;
        std::set_difference(oGolden.begin(), oGolden.end(),
                            oMine.begin(), oMine.end(), std::back_inserter(vMissed));
        std::set_difference(oMine.begin(), oMine.end(),
                            oGolden.begin(), oGolden.end(), std::back_inserter(vExtra));

        std::printf("  detections        %llu   (MATLAB %llu)\n",
                    (unsigned long long)oStats.uDetections,
                    (unsigned long long)oGoldenHeader.uDetections);
        std::printf("  missed / extra    %llu / %llu\n",
                    (unsigned long long)vMissed.size(),
                    (unsigned long long)vExtra.size());
        std::printf("  in the CFAR border %llu   (zero-padded window: these\n"
                    "                     over-detect by construction)\n",
                    (unsigned long long)oStats.uDetectionsInBorder);
        if (oStats.uDetectionsDropped > 0) {
            std::printf("  DROPPED           %llu beyond capacity -- the list is "
                        "incomplete\n",
                        (unsigned long long)oStats.uDetectionsDropped);
            bPass = false;
        }

        // Judge each difference by how far the cell was from its threshold.
        double dWorstMargin = 0.0;
        for (size_t u = 0; u < vMissed.size(); ++u) {
            const double dMargin = marginOf(vGoldenDet.data(),
                                            oGoldenHeader.uDetections,
                                            vMissed[u].first, vMissed[u].second);
            if (dMargin > dWorstMargin)
                dWorstMargin = dMargin;
            if (u < 5)
                std::printf("    missed: range bin %llu, doppler bin %llu "
                            "(MATLAB cleared threshold by %.4f dB)\n",
                            (unsigned long long)vMissed[u].first,
                            (unsigned long long)vMissed[u].second, dMargin);
        }
        for (size_t u = 0; u < vExtra.size(); ++u) {
            const double dMargin = marginOf(vDet.data(), oStats.uDetections,
                                            vExtra[u].first, vExtra[u].second);
            if (dMargin > dWorstMargin)
                dWorstMargin = dMargin;
            if (u < 5)
                std::printf("    extra:  range bin %llu, doppler bin %llu "
                            "(cleared threshold by %.4f dB)\n",
                            (unsigned long long)vExtra[u].first,
                            (unsigned long long)vExtra[u].second, dMargin);
        }

        if (!vMissed.empty() || !vExtra.empty()) {
            std::printf("  worst margin      %.4f dB   (tolerated up to %.2f dB)\n",
                        dWorstMargin, oOptions.dMarginDb);
            if (dWorstMargin > oOptions.dMarginDb) {
                std::printf("  -> a cell well clear of its threshold disagrees; "
                            "that is a real fault, not rounding\n");
                bPass = false;
            } else {
                std::printf("  -> every disagreement is a cell sitting on its "
                            "threshold, which the smallest\n"
                            "     difference in the map flips either way\n");
            }
        }
    }

    std::printf("%s\n", bPass ? "PASS -- matches the MATLAB reference."
                              : "FAIL -- see above.");
    return bPass ? 0 : 1;
}
