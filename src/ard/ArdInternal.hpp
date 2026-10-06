#ifndef PBR_ARD_INTERNAL_HPP
#define PBR_ARD_INTERNAL_HPP

// Shared by the ARD implementations in src/ard/. Not part of the public
// interface: nothing outside src/ard/ includes this.

#include <memory>

#include "pbr/ArdProcessor.hpp"

namespace pbr {

// One factory per backend; makeArdProcessor() chooses.
std::unique_ptr<ArdProcessor> makeArdProcessorCpu();
#if defined(PBR_WITH_CUDA)
std::unique_ptr<ArdProcessor> makeArdProcessorGpu();
#endif

// The parameter rules both backends' init() enforce, so they refuse the same
// inputs. On success fAlphaOverTrainOut is the CFAR scale factor: the
// threshold for a cell is fAlphaOverTrainOut times the sum of its training
// cells. Computed once in double and narrowed, as both backends use it.
// False, having logged the broken rule, otherwise.
bool checkArdParams(const ArdParams& oParams, float& fAlphaOverTrainOut);

} // namespace pbr

#endif // PBR_ARD_INTERNAL_HPP
