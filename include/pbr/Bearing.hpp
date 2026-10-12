#ifndef PBR_BEARING_HPP
#define PBR_BEARING_HPP

#include <cstdint>

#include "pbr/Config.hpp"
#include "pbr/types.hpp"

namespace pbr {

// Angle of arrival from the two surveillance channels, which form a two-
// element interferometer (docs/third-channel-aoa-design.md).
//
// A plane wave from an angle theta off the array's broadside reaches
// element 2 earlier than element 1 by d sin(theta) / c, so its phase at
// element 2 leads by
//
//     dPhi = 2 pi (d / lambda) sin(theta)      theta positive towards element 2
//
// The array looks 90 degrees clockwise of the baseline (element 1 to
// element 2), so the compass bearing is  baseline + 90 - theta.  With
// d = lambda / 2 the arcsine is unambiguous over the +/-90 degrees in front;
// a two-element array cannot tell front from back, and the antennas' own
// pattern settles that.
//
// Everything here is host arithmetic on a few cells per CPI.

struct ArrayGeometry {
    double dCarrierFreqHz  = 0.0;
    double dSpacingM       = 0.0;   // element 2 from element 1
    double dBaselineAzDeg  = 0.0;   // compass bearing, element 1 to element 2
    double dPhaseCalDeg    = 0.0;   // added to every measured phase difference
};

ArrayGeometry arrayGeometryFromConfig(const PbrConfig& oConfig);

struct Bearing {
    bool   bValid       = false;   // false if the phase implies |sin theta| > 1
    double dPhaseDeg    = 0.0;     // channel 2 minus channel 1, calibration added, in (-180, 180]
    double dCoherence   = 0.0;     // |sum M2 conj(M1)| / sqrt(sum |M1|^2 sum |M2|^2), in [0, 1]
    double dBearingDeg  = 0.0;     // compass, in [0, 360)
};

// The phase difference between the two maps over the 3 x 3 cells around
// (uRangeBin, uDopplerBin), clipped to the map, and the bearing it implies.
// Both maps are in display layout, map[doppler * uRangeBins + range].
Bearing bearingAtCell(const ComplexF* pcMap1, const ComplexF* pcMap2,
                      uint64_t uRangeBins, uint64_t uDopplerBins,
                      uint64_t uRangeBin, uint64_t uDopplerBin,
                      const ArrayGeometry& oArray);

// Compass bearing for a phase difference in degrees (calibration already
// applied). False if the phase implies |sin theta| > 1, which a spacing over
// half a wavelength or a noisy cell can produce.
bool bearingFromPhase(double dPhaseDeg, const ArrayGeometry& oArray, double& dBearingDegOut);

// The phase difference of the direct path: arg sum sur2[n] conj(sur1[n])
// over the raw CPI, in degrees, before calibration. The direct signal
// dominates that sum, and the transmitter's bearing is known, so this is
// the phase calibration check.
double directPathPhaseDeg(const ComplexF* pcSur1, const ComplexF* pcSur2, uint64_t uCount);

} // namespace pbr

#endif // PBR_BEARING_HPP
