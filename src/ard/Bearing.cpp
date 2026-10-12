#include "pbr/Bearing.hpp"

#include "pbr/check.hpp"

#include <cmath>
#include <cstdio>

namespace pbr {

namespace {

double wrapDegrees(double dDeg)      // into (-180, 180]
{
    dDeg = std::fmod(dDeg, 360.0);
    if (dDeg > 180.0)  dDeg -= 360.0;
    if (dDeg <= -180.0) dDeg += 360.0;
    return dDeg;
}

double compassDegrees(double dDeg)   // into [0, 360)
{
    dDeg = std::fmod(dDeg, 360.0);
    if (dDeg < 0.0) dDeg += 360.0;
    return dDeg;
}

} // namespace

ArrayGeometry arrayGeometryFromConfig(const PbrConfig& oConfig)
{
    ArrayGeometry oArray;
    oArray.dCarrierFreqHz = oConfig.dCarrierFreqHz;
    oArray.dSpacingM      = oConfig.dArraySpacingM;
    oArray.dBaselineAzDeg = oConfig.dArrayBaselineAzDeg;
    oArray.dPhaseCalDeg   = oConfig.dPhaseCalDeg;
    return oArray;
}

bool bearingFromPhase(double dPhaseDeg, const ArrayGeometry& oArray, double& dBearingDegOut)
{
    dBearingDegOut = 0.0;
    CHECK(oArray.dCarrierFreqHz > 0.0 && oArray.dSpacingM > 0.0,
          "bearingFromPhase: carrier and spacing must be positive");
    const double dLambda   = kSpeedOfLight / oArray.dCarrierFreqHz;
    const double dSinTheta = (dPhaseDeg * kPi / 180.0) * dLambda / (2.0 * kPi * oArray.dSpacingM);
    if (dSinTheta > 1.0 || dSinTheta < -1.0)
        return false;
    const double dThetaDeg = std::asin(dSinTheta) * 180.0 / kPi;
    dBearingDegOut = compassDegrees(oArray.dBaselineAzDeg + 90.0 - dThetaDeg);
    return true;
}

Bearing bearingAtCell(const ComplexF* pcMap1, const ComplexF* pcMap2,
                      uint64_t uRangeBins, uint64_t uDopplerBins,
                      uint64_t uRangeBin, uint64_t uDopplerBin,
                      const ArrayGeometry& oArray)
{
    Bearing oOut;   // bValid false until the phase has been read and converted
    if (pcMap1 == nullptr || pcMap2 == nullptr ||
        uRangeBin >= uRangeBins || uDopplerBin >= uDopplerBins) {
        std::fprintf(stderr, "bearingAtCell: null map or a cell off the map\n");
        return oOut;
    }

    // Sum over the 3 x 3 neighbourhood, in double: the maps are FP32 and the
    // cells around a peak differ by orders of magnitude.
    double dCrossRe = 0.0, dCrossIm = 0.0, dPower1 = 0.0, dPower2 = 0.0;
    for (int iOffD = -1; iOffD <= 1; ++iOffD) {
        for (int iOffR = -1; iOffR <= 1; ++iOffR) {
            const int64_t iD = static_cast<int64_t>(uDopplerBin) + iOffD;
            const int64_t iR = static_cast<int64_t>(uRangeBin) + iOffR;
            if (iD < 0 || iD >= static_cast<int64_t>(uDopplerBins) ||
                iR < 0 || iR >= static_cast<int64_t>(uRangeBins))
                continue;
            const size_t   uCell = static_cast<size_t>(iD) * uRangeBins + static_cast<size_t>(iR);
            const ComplexF c1 = pcMap1[uCell];
            const ComplexF c2 = pcMap2[uCell];
            // c2 * conj(c1)
            dCrossRe += double(c2.real()) * c1.real() + double(c2.imag()) * c1.imag();
            dCrossIm += double(c2.imag()) * c1.real() - double(c2.real()) * c1.imag();
            dPower1  += double(c1.real()) * c1.real() + double(c1.imag()) * c1.imag();
            dPower2  += double(c2.real()) * c2.real() + double(c2.imag()) * c2.imag();
        }
    }
    if (dPower1 <= 0.0 || dPower2 <= 0.0)
        return oOut;   // an empty map: no phase to read

    oOut.dCoherence = std::sqrt((dCrossRe * dCrossRe + dCrossIm * dCrossIm) / (dPower1 * dPower2));
    oOut.dPhaseDeg  = wrapDegrees(std::atan2(dCrossIm, dCrossRe) * 180.0 / kPi + oArray.dPhaseCalDeg);
    oOut.bValid     = bearingFromPhase(oOut.dPhaseDeg, oArray, oOut.dBearingDeg);
    return oOut;
}

double directPathPhaseDeg(const ComplexF* pcSur1, const ComplexF* pcSur2, uint64_t uCount)
{
    double dRe = 0.0, dIm = 0.0;
    if (pcSur1 == nullptr || pcSur2 == nullptr) {
        std::fprintf(stderr, "directPathPhaseDeg: null channel\n");
        return 0.0;
    }
    for (uint64_t n = 0; n < uCount; ++n) {
        const ComplexF c1 = pcSur1[n];
        const ComplexF c2 = pcSur2[n];
        dRe += double(c2.real()) * c1.real() + double(c2.imag()) * c1.imag();
        dIm += double(c2.imag()) * c1.real() - double(c2.real()) * c1.imag();
    }
    return std::atan2(dIm, dRe) * 180.0 / kPi;
}

} // namespace pbr
