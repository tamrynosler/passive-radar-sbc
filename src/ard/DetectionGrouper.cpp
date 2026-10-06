#include "pbr/DetectionGrouper.hpp"

#include "pbr/check.hpp"

#include <climits>
#include <cmath>
#include <cstdio>

namespace pbr {

bool DetectionGrouper::init(uint64_t uRangeBins, uint64_t uDopplerBins)
{
    CHECK(uRangeBins > 0 && uDopplerBins > 0, "DetectionGrouper: empty map");
    CHECK(uRangeBins * uDopplerBins <= INT_MAX, "DetectionGrouper: map too large");

    muRangeBins   = uRangeBins;
    muDopplerBins = uDopplerBins;
    mviCellToDet.assign(static_cast<size_t>(uRangeBins * uDopplerBins), -1);
    mvbVisited.assign(static_cast<size_t>(kMaxArdDetections), 0);
    mvuStack.reserve(static_cast<size_t>(kMaxArdDetections));
    return true;
}

uint64_t DetectionGrouper::group(const Detection* pDet, uint64_t uCount,
                                 Plot* pPlots, uint64_t uMaxPlots, uint64_t& uDroppedOut)
{
    uDroppedOut = 0;
    CHECK(muRangeBins > 0, "DetectionGrouper::group before init");
    CHECK(pDet != nullptr || uCount == 0, "DetectionGrouper: null detection list");
    CHECK(pPlots != nullptr || uMaxPlots == 0, "DetectionGrouper: null plot array");
    CHECK(uCount <= kMaxArdDetections, "DetectionGrouper: more detections than kMaxArdDetections");

    // Mark every detection's cell with its index. Each detection is a
    // distinct cell, so a cell marked twice means the list is corrupt.
    uint64_t uMarked = 0;
    bool     bOk     = true;
    for (; uMarked < uCount; ++uMarked) {
        const Detection& oDet = pDet[uMarked];
        if (oDet.uRangeBin >= muRangeBins || oDet.uDopplerBin >= muDopplerBins) {
            std::fprintf(stderr, "DetectionGrouper: detection %llu lies outside the map\n",
                         (unsigned long long)uMarked);
            bOk = false;
            break;
        }
        int32_t& iCell = mviCellToDet[static_cast<size_t>(
            oDet.uDopplerBin * muRangeBins + oDet.uRangeBin)];
        if (iCell >= 0) {
            std::fprintf(stderr, "DetectionGrouper: two detections in one cell\n");
            bOk = false;
            break;
        }
        iCell = static_cast<int32_t>(uMarked);
        mvbVisited[static_cast<size_t>(uMarked)] = 0;
    }

    uint64_t uPlots = 0;
    for (uint64_t uFirst = 0; bOk && uFirst < uCount; ++uFirst) {
        if (mvbVisited[static_cast<size_t>(uFirst)])
            continue;

        // Flood-fill the group that uFirst belongs to, accumulating the
        // power-weighted sums as each detection is taken off the stack.
        double   dWeight = 0.0, dWeightedKm = 0.0, dWeightedHz = 0.0;
        uint64_t uCells  = 0;
        uint64_t uPeak   = uFirst;
        mvbVisited[static_cast<size_t>(uFirst)] = 1;
        mvuStack.assign(1, static_cast<uint32_t>(uFirst));

        while (!mvuStack.empty()) {
            const uint32_t u = mvuStack.back();
            mvuStack.pop_back();
            const Detection& oDet = pDet[u];

            // dPowerDb is relative to the map peak, so this is the cell's
            // linear power over the peak's: at most 1, never zero.
            const double dW = std::pow(10.0, oDet.dPowerDb / 10.0);
            dWeight     += dW;
            dWeightedKm += dW * oDet.dRangeKm;
            dWeightedHz += dW * oDet.dDopplerHz;
            ++uCells;
            // Strictly greater, so on a tie the earlier detection stays.
            if (oDet.dPowerDb > pDet[uPeak].dPowerDb ||
                (oDet.dPowerDb == pDet[uPeak].dPowerDb && u < uPeak))
                uPeak = u;

            // The eight neighbours, those that exist on the map.
            for (int iOffD = -1; iOffD <= 1; ++iOffD) {
                for (int iOffR = -1; iOffR <= 1; ++iOffR) {
                    if (iOffD == 0 && iOffR == 0)
                        continue;
                    const int64_t iD = static_cast<int64_t>(oDet.uDopplerBin) + iOffD;
                    const int64_t iR = static_cast<int64_t>(oDet.uRangeBin) + iOffR;
                    if (iD < 0 || iD >= static_cast<int64_t>(muDopplerBins) ||
                        iR < 0 || iR >= static_cast<int64_t>(muRangeBins))
                        continue;
                    const int32_t iNeighbour = mviCellToDet[static_cast<size_t>(
                        static_cast<uint64_t>(iD) * muRangeBins + static_cast<uint64_t>(iR))];
                    if (iNeighbour < 0 || mvbVisited[static_cast<size_t>(iNeighbour)])
                        continue;
                    mvbVisited[static_cast<size_t>(iNeighbour)] = 1;
                    mvuStack.push_back(static_cast<uint32_t>(iNeighbour));
                }
            }
        }

        if (uPlots >= uMaxPlots) {
            ++uDroppedOut;
            continue;
        }
        Plot& oPlot = pPlots[static_cast<size_t>(uPlots++)];
        oPlot.dRangeKm        = dWeightedKm / dWeight;
        oPlot.dDopplerHz      = dWeightedHz / dWeight;
        oPlot.uPeakRangeBin   = pDet[uPeak].uRangeBin;
        oPlot.uPeakDopplerBin = pDet[uPeak].uDopplerBin;
        oPlot.dPeakPowerDb    = pDet[uPeak].dPowerDb;
        oPlot.dPeakSnrDb      = pDet[uPeak].dSnrDb;
        oPlot.uCells          = uCells;
    }

    // Clear only the cells marked above, so the next call starts from an
    // all -1 grid without touching the whole map.
    for (uint64_t u = 0; u < uMarked; ++u) {
        mviCellToDet[static_cast<size_t>(
            pDet[u].uDopplerBin * muRangeBins + pDet[u].uRangeBin)] = -1;
    }
    return bOk ? uPlots : 0;
}

Detection plotAsDetection(const Plot& oPlot)
{
    Detection oDet;
    oDet.uRangeBin   = oPlot.uPeakRangeBin;
    oDet.uDopplerBin = oPlot.uPeakDopplerBin;
    oDet.dRangeKm    = oPlot.dRangeKm;
    oDet.dDopplerHz  = oPlot.dDopplerHz;
    oDet.dPowerDb    = oPlot.dPeakPowerDb;
    oDet.dSnrDb      = oPlot.dPeakSnrDb;
    return oDet;
}

} // namespace pbr
