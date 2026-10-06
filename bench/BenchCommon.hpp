#ifndef PBR_BENCH_COMMON_HPP
#define PBR_BENCH_COMMON_HPP

// Shared by the timing harnesses in bench/: summary statistics over a set of
// timed runs, and appending one row to a results CSV. Header-only and
// bench-only.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace pbrbench {

// Summary of a set of timed runs, in milliseconds. Both a minimum and a mean
// are reported: under DVFS the mean wanders while the minimum tracks the true
// compute time, and a mean alone has been misleading on this hardware before.
struct TimingSummary {
    double dMinMs    = 0.0;
    double dMedianMs = 0.0;
    double dMeanMs   = 0.0;
    double dStdMs    = 0.0;   // sample standard deviation
    double dMaxMs    = 0.0;
};

inline TimingSummary summarise(std::vector<double> vdMs)   // by value: sorted here
{
    TimingSummary oSummary;
    if (vdMs.empty())
        return oSummary;

    std::sort(vdMs.begin(), vdMs.end());
    const size_t uCount = vdMs.size();
    const size_t uMid   = uCount / 2;

    double dSum = 0.0;
    for (size_t u = 0; u < uCount; ++u)
        dSum += vdMs[u];
    oSummary.dMeanMs = dSum / static_cast<double>(uCount);

    if (uCount > 1) {
        double dSq = 0.0;
        for (size_t u = 0; u < uCount; ++u)
            dSq += (vdMs[u] - oSummary.dMeanMs) * (vdMs[u] - oSummary.dMeanMs);
        oSummary.dStdMs = std::sqrt(dSq / static_cast<double>(uCount - 1));
    }

    oSummary.dMinMs    = vdMs.front();
    oSummary.dMaxMs    = vdMs.back();
    oSummary.dMedianMs = (uCount % 2 == 0) ? 0.5 * (vdMs[uMid - 1] + vdMs[uMid])
                                           : vdMs[uMid];
    return oSummary;
}

// Open pPath for appending one row, writing pHeader first if the file is new,
// so a sweep over several runs accumulates into one table. Null, having
// logged why, if the file cannot be opened; the caller writes the row and
// closes it.
inline std::FILE* openCsvForAppend(const char* pPath, const char* pHeader)
{
    std::FILE* pExisting = std::fopen(pPath, "r");
    const bool bNew = (pExisting == nullptr);
    if (pExisting != nullptr)
        std::fclose(pExisting);

    std::FILE* pCsv = std::fopen(pPath, "a");
    if (pCsv == nullptr) {
        std::fprintf(stderr, "cannot open %s for writing\n", pPath);
        return nullptr;
    }
    if (bNew)
        std::fprintf(pCsv, "%s\n", pHeader);
    return pCsv;
}

} // namespace pbrbench

#endif // PBR_BENCH_COMMON_HPP
