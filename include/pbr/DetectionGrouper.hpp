#ifndef PBR_DETECTION_GROUPER_HPP
#define PBR_DETECTION_GROUPER_HPP

#include <cstdint>
#include <vector>

#include "pbr/ArdProcessor.hpp"

namespace pbr {

// One target as the CFAR saw it: a group of touching detections merged into
// one point.
struct Plot {
    double   dRangeKm        = 0.0;   // power-weighted centroid of the group
    double   dDopplerHz      = 0.0;   // power-weighted centroid of the group
    uint64_t uPeakRangeBin   = 0;     // the group's strongest cell
    uint64_t uPeakDopplerBin = 0;
    double   dPeakPowerDb    = 0.0;   // that cell's Detection::dPowerDb
    double   dPeakSnrDb      = 0.0;   // that cell's Detection::dSnrDb
    uint64_t uCells          = 0;     // detections merged into this plot
};

// Groups CFAR detections into plots (Tong 2014, UCT PhD: 8-connected
// regions of detected cells, one centroid per region), with each cell
// weighted by its linear power instead of equally, the revision Tong
// suggests. A target spreads over several cells -- the FM range mainlobe
// alone is about seven bins -- and this turns that blob into one point
// that is not tied to the bin grid.
//
// Cells touch if they are adjacent in range, in Doppler or diagonally. The
// Doppler axis does not wrap. There is no tuning parameter.
//
// Runs on the host over the detection list, after either backend's CFAR,
// so both backends give the same plots. matlab/group_detections.m is the
// same rule for the MATLAB chain.
class DetectionGrouper {
public:
    // Allocates an index grid the size of the map and the working arrays,
    // once. May be called again to resize.
    bool init(uint64_t uRangeBins, uint64_t uDopplerBins);

    // Group uCount detections (at most kMaxArdDetections) into pPlots. Plots
    // come out in the order of each group's first detection in pDet, so the
    // same list always gives the same plots. Groups beyond uMaxPlots are
    // counted in uDroppedOut. Returns the number of plots written; 0 with
    // uDroppedOut 0 and an error logged if the input is invalid.
    uint64_t group(const Detection* pDet, uint64_t uCount,
                   Plot* pPlots, uint64_t uMaxPlots, uint64_t& uDroppedOut);

private:
    uint64_t              muRangeBins   = 0;
    uint64_t              muDopplerBins = 0;
    std::vector<int32_t>  mviCellToDet;   // map cell -> detection index, or -1
    std::vector<uint8_t>  mvbVisited;     // per detection
    std::vector<uint32_t> mvuStack;       // detections waiting to be expanded
};

// A plot in the Detection layout, for the ARD frame: the bins are its peak
// cell, the km and Hz its centroid, the power and SNR its peak cell's.
Detection plotAsDetection(const Plot& oPlot);

} // namespace pbr

#endif // PBR_DETECTION_GROUPER_HPP
