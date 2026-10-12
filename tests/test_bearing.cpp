// Unit tests for the two-element bearing arithmetic. Plain main() + asserts.
#undef NDEBUG
#include <cassert>

#include "pbr/Bearing.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pbr;

namespace {

bool near(double dA, double dB, double dTol) { return std::fabs(dA - dB) <= dTol; }

ArrayGeometry halfWavelength()
{
    ArrayGeometry oArray;
    oArray.dCarrierFreqHz = 89e6;
    oArray.dSpacingM      = 0.5 * kSpeedOfLight / 89e6;
    oArray.dBaselineAzDeg = 35.0;
    oArray.dPhaseCalDeg   = 0.0;
    return oArray;
}

// At half a wavelength, phase = 180 sin(theta); the array looks at 125.
void testBearingFromPhase()
{
    const ArrayGeometry oArray = halfWavelength();
    double dBearing = 0.0;
    assert(bearingFromPhase(0.0, oArray, dBearing) && near(dBearing, 125.0, 1e-9));
    assert(bearingFromPhase(180.0, oArray, dBearing) && near(dBearing, 35.0, 1e-9));    // along the baseline, towards element 2
    assert(bearingFromPhase(-180.0, oArray, dBearing) && near(dBearing, 215.0, 1e-9));
    assert(bearingFromPhase(90.0, oArray, dBearing) && near(dBearing, 95.0, 1e-9));     // theta = 30
    assert(!bearingFromPhase(181.0, oArray, dBearing));                                 // |sin| > 1

    ArrayGeometry oNorth = oArray;
    oNorth.dBaselineAzDeg = 300.0;   // looks at 30; theta = -30 wraps past 0
    assert(bearingFromPhase(-90.0, oNorth, dBearing) && near(dBearing, 60.0, 1e-9));
}

// Map 2 is map 1 rotated by a fixed phase: the phase comes back, the
// coherence is 1, and the bearing follows.
void testBearingAtCell()
{
    const ArrayGeometry oArray = halfWavelength();
    const uint64_t uR = 7, uD = 5;
    std::vector<ComplexF> vcMap1(uR * uD), vcMap2(uR * uD);
    const double dPhaseDeg = 60.0;   // theta = asin(1/3)
    const ComplexF cRot(std::cos(dPhaseDeg * kPi / 180.0), std::sin(dPhaseDeg * kPi / 180.0));
    for (size_t u = 0; u < vcMap1.size(); ++u) {
        vcMap1[u] = ComplexF(float(0.1 * (u % 4) + 0.3), float(0.2 * (u % 3) - 0.1));
        vcMap2[u] = vcMap1[u] * cRot;
    }

    Bearing oB = bearingAtCell(vcMap1.data(), vcMap2.data(), uR, uD, 3, 2, oArray);
    assert(oB.bValid);
    assert(near(oB.dPhaseDeg, dPhaseDeg, 1e-4));
    assert(near(oB.dCoherence, 1.0, 1e-6));
    const double dTheta = std::asin(60.0 / 180.0) * 180.0 / kPi;
    assert(near(oB.dBearingDeg, 125.0 - dTheta, 1e-4));

    // A corner cell: the window is clipped, the answer unchanged.
    oB = bearingAtCell(vcMap1.data(), vcMap2.data(), uR, uD, 0, 0, oArray);
    assert(oB.bValid && near(oB.dPhaseDeg, dPhaseDeg, 1e-4));

    // Calibration shifts the phase before the arcsine.
    ArrayGeometry oCal = oArray;
    oCal.dPhaseCalDeg = -60.0;
    oB = bearingAtCell(vcMap1.data(), vcMap2.data(), uR, uD, 3, 2, oCal);
    assert(oB.bValid && near(oB.dPhaseDeg, 0.0, 1e-4) && near(oB.dBearingDeg, 125.0, 1e-4));

    // Uncorrelated maps: low coherence.
    for (size_t u = 0; u < vcMap2.size(); ++u)
        vcMap2[u] = ComplexF(float((u * 7919) % 13) - 6.0f, float((u * 104729) % 11) - 5.0f);
    oB = bearingAtCell(vcMap1.data(), vcMap2.data(), uR, uD, 3, 2, oArray);
    assert(oB.dCoherence < 0.9);

    // An empty map reads as no bearing rather than NaN.
    std::vector<ComplexF> vcZero(uR * uD, ComplexF(0.0f, 0.0f));
    oB = bearingAtCell(vcZero.data(), vcMap1.data(), uR, uD, 3, 2, oArray);
    assert(!oB.bValid && oB.dCoherence == 0.0);
}

// A rotated copy plus independent noise still reads the rotation.
void testDirectPathPhase()
{
    const uint64_t uN = 20000;
    std::vector<ComplexF> vc1(uN), vc2(uN);
    uint32_t uLcg = 99u;
    auto next = [&uLcg]() {
        uLcg = uLcg * 1664525u + 1013904223u;
        return float(uLcg >> 8) / 16777216.0f - 0.5f;
    };
    const double dPhaseDeg = -176.0;
    const ComplexF cRot(std::cos(dPhaseDeg * kPi / 180.0), std::sin(dPhaseDeg * kPi / 180.0));
    for (uint64_t n = 0; n < uN; ++n) {
        vc1[n] = ComplexF(next(), next());
        vc2[n] = vc1[n] * cRot + ComplexF(0.05f * next(), 0.05f * next());
    }
    assert(near(directPathPhaseDeg(vc1.data(), vc2.data(), uN), dPhaseDeg, 0.5));
}

} // namespace

int main()
{
    testBearingFromPhase();
    testBearingAtCell();
    testDirectPathPhase();
    std::printf("test_bearing: all tests passed\n");
    return 0;
}
