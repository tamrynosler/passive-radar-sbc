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

const double kPi = 3.14159265358979323846;

} // namespace pbr

#endif // PBR_TYPES_HPP
