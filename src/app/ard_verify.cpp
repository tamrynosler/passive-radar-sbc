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
    std::string sGoldenPath;       // MATLAB ARD file
    std::string sConfigPath = kDefaultConfigPath;
    std::string sBackend;
    std::string sStage      = "both";
    double      dTolerance  = 1e-4;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s --cleaned FILE --golden FILE [options]\n", pProgram);
    std::printf("  --cleaned FILE   (reference, cleaned) CPI from\n");
    std::printf("                   dsi_verify --dump-output\n");
    std::printf("  --golden FILE    ARD file from matlab/ard_golden.m\n");
    std::printf("  --stage NAME     caf | cfar | both   (default both)\n");
    std::printf("  --backend NAME   CPU or GPU (default: from the config)\n");
    std::printf("  --config FILE    configuration (default %s)\n", kDefaultConfigPath);
    std::printf("  --tolerance X    relative L2 acceptance for the map (default 1e-4)\n");
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
    if (oOptions.sStage != "cfar" && oOptions.sCleanedPath.empty()) {
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

    if (oOptions.sStage == "cfar") {
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
                pArd->name(), oOptions.sStage.c_str(), oOptions.sGoldenPath.c_str());

    bool bPass = true;

    if (oOptions.sStage != "cfar") {
        const double dRel = relativeL2(vcMap, vcGoldenMap);
        std::printf("  map relative L2   %.3e   (tolerance %g)\n",
                    dRel, oOptions.dTolerance);
        std::printf("  peak              %.4f dB\n", oStats.dPeakDb);
        if (!(dRel < oOptions.dTolerance))
            bPass = false;
    }

    if (oOptions.sStage != "caf") {
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

        for (size_t u = 0; u < vMissed.size() && u < 5; ++u)
            std::printf("    missed: range bin %llu, doppler bin %llu\n",
                        (unsigned long long)vMissed[u].first,
                        (unsigned long long)vMissed[u].second);
        for (size_t u = 0; u < vExtra.size() && u < 5; ++u)
            std::printf("    extra:  range bin %llu, doppler bin %llu\n",
                        (unsigned long long)vExtra[u].first,
                        (unsigned long long)vExtra[u].second);

        if (!vMissed.empty() || !vExtra.empty())
            bPass = false;
    }

    std::printf("%s\n", bPass ? "PASS -- matches the MATLAB reference."
                              : "FAIL -- see above.");
    return bPass ? 0 : 1;
}
