#ifndef PBR_TYPES_HPP
#define PBR_TYPES_HPP

#include <complex>

namespace pbr {

// Host-side complex sample type.
//
// The device side uses cufftComplex, which has the same layout, so the two
// are bridged with a reinterpret_cast at the copy boundary only -- never
// anywhere else.
using ComplexF = std::complex<float>;   // host

// constexpr, so CUDA device code may read them too.
constexpr double kPi           = 3.14159265358979323846;
constexpr double kSpeedOfLight = 299792458.0;   // m/s

} // namespace pbr

#endif // PBR_TYPES_HPP
