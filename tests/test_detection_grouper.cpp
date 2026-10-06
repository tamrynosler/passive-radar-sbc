// Unit tests for DetectionGrouper: CFAR detections into power-weighted plots.
// Plain main() + asserts, no framework. Every expected value is worked out by
// hand in the comment beside it.
//
// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out.
#undef NDEBUG
#include <cassert>

#include "pbr/DetectionGrouper.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pbr;

namespace {

const uint64_t kRanges   = 10;
const uint64_t kDopplers = 20;

// A detection whose km and Hz are simply its bin numbers, so a centroid can
// be read off in bins.
Detection det(uint64_t uRange, uint64_t uDoppler, double dPowerDb, double dSnrDb = 3.0)
{
    Detection oDet;
    oDet.uRangeBin   = uRange;
    oDet.uDopplerBin = uDoppler;
    oDet.dRangeKm    = static_cast<double>(uRange);
    oDet.dDopplerHz  = static_cast<double>(uDoppler);
    oDet.dPowerDb    = dPowerDb;
    oDet.dSnrDb      = dSnrDb;
    return oDet;
}

bool near(double dA, double dB)
{
    return std::fabs(dA - dB) < 1e-12;
}

uint64_t run(DetectionGrouper& oGrouper, const std::vector<Detection>& vDet,
             std::vector<Plot>& vPlots, uint64_t uMaxPlots, uint64_t& uDropped)
{
    vPlots.assign(static_cast<size_t>(uMaxPlots), Plot());
    return oGrouper.group(vDet.data(), vDet.size(), vPlots.data(), uMaxPlots, uDropped);
}

// Two separate blobs and a lone cell: three plots, in the order of each
// group's first detection, with the peak and cell count of each.
void testSeparateGroups(DetectionGrouper& oGrouper)
{
    std::vector<Detection> vDet;
    vDet.push_back(det(2, 3, -10.0));   // group A
    vDet.push_back(det(3, 3, 0.0));     // group A, its peak
    vDet.push_back(det(7, 3, -20.0));   // lone cell, 4 range bins from A
    vDet.push_back(det(2, 4, -10.0));   // group A
    vDet.push_back(det(5, 12, -6.0, 9.0)); // group B
    vDet.push_back(det(5, 13, -6.0, 4.0)); // group B, equal power: the first stays peak

    std::vector<Plot> vPlots;
    uint64_t uDropped = 0;
    assert(run(oGrouper, vDet, vPlots, 10, uDropped) == 3);
    assert(uDropped == 0);

    assert(vPlots[0].uCells == 3);
    assert(vPlots[0].uPeakRangeBin == 3 && vPlots[0].uPeakDopplerBin == 3);
    assert(vPlots[0].dPeakPowerDb == 0.0);

    assert(vPlots[1].uCells == 1);
    assert(near(vPlots[1].dRangeKm, 7.0) && near(vPlots[1].dDopplerHz, 3.0));

    assert(vPlots[2].uCells == 2);
    assert(vPlots[2].uPeakDopplerBin == 12 && vPlots[2].dPeakSnrDb == 9.0);
    // Equal weights: halfway between Doppler 12 and 13.
    assert(near(vPlots[2].dRangeKm, 5.0) && near(vPlots[2].dDopplerHz, 12.5));
}

// The weighting. Two cells 10 dB apart have linear weights 1 and 0.1, so
// the centroid sits 0.1 / 1.1 = 1/11 of the way from the stronger cell.
void testPowerWeighting(DetectionGrouper& oGrouper)
{
    std::vector<Detection> vDet;
    vDet.push_back(det(4, 8, 0.0));
    vDet.push_back(det(5, 8, -10.0));

    std::vector<Plot> vPlots;
    uint64_t uDropped = 0;
    assert(run(oGrouper, vDet, vPlots, 10, uDropped) == 1);
    assert(near(vPlots[0].dRangeKm, 4.0 + 1.0 / 11.0));
    assert(near(vPlots[0].dDopplerHz, 8.0));
}

// Cells that touch only at a corner are one group (8-connectivity). A chain
// of diagonal steps joins three cells that share no edge at all.
void testDiagonalJoins(DetectionGrouper& oGrouper)
{
    std::vector<Detection> vDet;
    vDet.push_back(det(1, 1, -3.0));
    vDet.push_back(det(2, 2, -3.0));
    vDet.push_back(det(3, 3, -3.0));
    vDet.push_back(det(5, 3, -3.0));   // two range bins from (3,3): separate

    std::vector<Plot> vPlots;
    uint64_t uDropped = 0;
    assert(run(oGrouper, vDet, vPlots, 10, uDropped) == 2);
    assert(vPlots[0].uCells == 3);
    assert(near(vPlots[0].dRangeKm, 2.0) && near(vPlots[0].dDopplerHz, 2.0));
    assert(vPlots[1].uCells == 1);
}

// Groups on the map's corners and edges: the neighbour search must stay on
// the map, and the Doppler axis does not wrap, so the top and bottom rows
// are separate groups.
void testEdges(DetectionGrouper& oGrouper)
{
    std::vector<Detection> vDet;
    vDet.push_back(det(0, 0, -1.0));
    vDet.push_back(det(1, 0, -1.0));
    vDet.push_back(det(0, kDopplers - 1, -1.0));
    vDet.push_back(det(kRanges - 1, kDopplers - 1, -1.0));

    std::vector<Plot> vPlots;
    uint64_t uDropped = 0;
    assert(run(oGrouper, vDet, vPlots, 10, uDropped) == 3);
    assert(vPlots[0].uCells == 2 && near(vPlots[0].dRangeKm, 0.5));
    assert(vPlots[1].uCells == 1 && vPlots[2].uCells == 1);
}

// More groups than room: the first uMaxPlots come out, the rest are counted.
// Then the grouper must have cleaned up: the same cells grouped again, as a
// single blob, give one plot, not stale results from the previous call.
void testCapAndReuse(DetectionGrouper& oGrouper)
{
    std::vector<Detection> vDet;
    for (uint64_t u = 0; u < 5; ++u)
        vDet.push_back(det(2 * u, 2 * u, -1.0));   // diagonal, but two apart: 5 groups

    std::vector<Plot> vPlots;
    uint64_t uDropped = 0;
    assert(run(oGrouper, vDet, vPlots, 3, uDropped) == 3);
    assert(uDropped == 2);
    assert(vPlots[2].uPeakRangeBin == 4);

    std::vector<Detection> vBlob;
    vBlob.push_back(det(0, 0, -1.0));
    vBlob.push_back(det(1, 1, -1.0));
    assert(run(oGrouper, vBlob, vPlots, 3, uDropped) == 1);
    assert(uDropped == 0 && vPlots[0].uCells == 2);

    std::vector<Detection> vNone;
    assert(run(oGrouper, vNone, vPlots, 3, uDropped) == 0 && uDropped == 0);
}

// A list the CFAR could never produce is refused rather than grouped.
void testInvalidInput(DetectionGrouper& oGrouper)
{
    std::vector<Plot> vPlots;
    uint64_t uDropped = 0;

    std::vector<Detection> vOff;
    vOff.push_back(det(kRanges, 0, -1.0));
    assert(run(oGrouper, vOff, vPlots, 3, uDropped) == 0);

    std::vector<Detection> vTwice;
    vTwice.push_back(det(1, 1, -1.0));
    vTwice.push_back(det(1, 1, -2.0));
    assert(run(oGrouper, vTwice, vPlots, 3, uDropped) == 0);

    // And it still works afterwards.
    std::vector<Detection> vGood;
    vGood.push_back(det(1, 1, -1.0));
    assert(run(oGrouper, vGood, vPlots, 3, uDropped) == 1);
}

// The frame record of a plot: peak cell's bins, power and SNR; centroid's
// km and Hz.
void testPlotAsDetection()
{
    Plot oPlot;
    oPlot.dRangeKm = 12.25;  oPlot.dDopplerHz = -3.5;
    oPlot.uPeakRangeBin = 8; oPlot.uPeakDopplerBin = 797;
    oPlot.dPeakPowerDb = -4.0; oPlot.dPeakSnrDb = 17.0; oPlot.uCells = 9;

    const Detection oDet = plotAsDetection(oPlot);
    assert(oDet.uRangeBin == 8 && oDet.uDopplerBin == 797);
    assert(oDet.dRangeKm == 12.25 && oDet.dDopplerHz == -3.5);
    assert(oDet.dPowerDb == -4.0 && oDet.dSnrDb == 17.0);
}

} // namespace

int main()
{
    DetectionGrouper oGrouper;
    assert(oGrouper.init(kRanges, kDopplers));

    testSeparateGroups(oGrouper);
    testPowerWeighting(oGrouper);
    testDiagonalJoins(oGrouper);
    testEdges(oGrouper);
    testCapAndReuse(oGrouper);
    testInvalidInput(oGrouper);
    testPlotAsDetection();

    std::printf("test_detection_grouper: all tests passed\n");
    return 0;
}
