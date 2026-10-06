#ifndef PBR_DSI_INTERNAL_HPP
#define PBR_DSI_INTERNAL_HPP

// Shared by the DSI implementations in src/dsi/. Not part of the public
// interface: nothing outside src/dsi/ includes this.

#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include "pbr/DSICanceller.hpp"
#include "pbr/types.hpp"

namespace pbr {

// One factory per implementation file; makeDSICanceller() chooses.
std::unique_ptr<DSICanceller> makeEcaCancellerCpu();
std::unique_ptr<DSICanceller> makeCglsCancellerCpu();
std::unique_ptr<DSICanceller> makeEcaCdCancellerCpu();
#if defined(PBR_WITH_CUDA)
std::unique_ptr<DSICanceller> makeEcaCancellerGpu();
std::unique_ptr<DSICanceller> makeCglsCancellerGpu();
std::unique_ptr<DSICanceller> makeEcaCdCancellerGpu();
#endif

// The parameter rules each algorithm's init() enforces, the same on both
// backends. Each returns false, having logged the broken rule.
bool ecaParamsOk(const DSIParams& oParams);
bool cglsParamsOk(const DSIParams& oParams);
bool ecaCdParamsOk(const DSIParams& oParams);

// Recompute the CGLS residual r = b - R x from scratch this often, in
// iterations. Ten is a guess that the FP64 versus FP32 comparison in
// tests/test_cgls.cpp is there to check.
const uint64_t kCglsResidualRefresh = 10;

// The M-point table exp(+j 2 pi i / M), i = 0..M-1, computed in double. ECA-CD
// steps across its batches with it; the CPU's FP32 mode and the GPU both
// round it to float, so they use the same values.
std::vector<std::complex<double>> batchTwiddles(uint64_t uBatches);

// 10 log10(input power / output power) over uSamples, summed in double.
// The CPU backends' suppression figure.
double hostSuppressionDb(const ComplexF* pcSur, const ComplexF* pcOut, uint64_t uSamples);

} // namespace pbr

#endif // PBR_DSI_INTERNAL_HPP
