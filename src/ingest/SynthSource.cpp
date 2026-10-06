#include "pbr/SampleSource.hpp"

#include "pbr/check.hpp"

#include <cmath>
#include <cstdio>
#include <thread>

namespace pbr {

namespace {

// Signal levels in ADC codes. The reference sits well inside int16 so the
// hashed noise never clips; the surveillance terms are relative to it.
const double kRefRmsCodes     = 8000.0;
const double kDirectPathGain  = 0.1;      // -20 dB
const double kTargetGain      = 0.01;     // -40 dB
const double kSurNoiseRms     = 20.0;

const double kTwoPi = 6.283185307179586;

const uint64_t kRefSeedI = 0x9E3779B97F4A7C15ULL;
const uint64_t kRefSeedQ = 0xD1B54A32D192ED03ULL;
const uint64_t kSurSeedI = 0x8CB92BA72F3D8DD7ULL;
const uint64_t kSurSeedQ = 0x2545F4914F6CDD1DULL;

// splitmix64 finaliser: a good 64-bit hash of (seed, index).
uint64_t hash64(uint64_t uSeed, uint64_t uIndex)
{
    uint64_t uZ = uIndex + uSeed;
    uZ = (uZ ^ (uZ >> 30)) * 0xBF58476D1CE4E5B9ULL;
    uZ = (uZ ^ (uZ >> 27)) * 0x94D049BB133111EBULL;
    return uZ ^ (uZ >> 31);
}

// Roughly Gaussian, unit variance: the sum of four independent uniforms on
// [-0.5, 0.5) taken from the four 16-bit lanes of one hash, scaled by
// 1 / sqrt(4 / 12). Bounded at +-3.46, so it can never clip int16 at the
// levels above.
double noiseAt(uint64_t uSeed, uint64_t uIndex)
{
    const uint64_t uHash = hash64(uSeed, uIndex);
    double dSum = 0.0;
    for (unsigned u = 0; u < 4; ++u) {
        const double dLane = static_cast<double>((uHash >> (16 * u)) & 0xFFFFu);
        dSum += dLane / 65536.0 - 0.5;
    }
    return dSum * 1.7320508075688772;   // sqrt(3)
}

int16_t toCode(double dValue)
{
    if (dValue >  32767.0) return  32767;
    if (dValue < -32767.0) return -32767;
    return static_cast<int16_t>(std::lrint(dValue));
}

} // namespace

SynthSource::SynthSource(uint64_t uSampleRateHz, uint64_t uTargetDelaySamples,
                         int64_t iTargetDopplerHz)
    : muSampleRateHz(uSampleRateHz),
      muTargetDelay(uTargetDelaySamples),
      muDopplerSteps(0),
      mbStarted(false),
      muIndex(0),
      muStartIndex(0)
{
    if (uSampleRateHz > 0) {
        // Reduce the Doppler modulo fs so the phase step below is a
        // non-negative integer in [0, fs).
        const int64_t iFs  = static_cast<int64_t>(uSampleRateHz);
        int64_t       iMod = iTargetDopplerHz % iFs;
        if (iMod < 0)
            iMod += iFs;
        muDopplerSteps = static_cast<uint64_t>(iMod);
    }
}

bool SynthSource::start()
{
    CHECK(muSampleRateHz > 0, "SynthSource: sample rate must be non-zero");
    CHECK(!mbStarted, "SynthSource: start() called twice");

    // Anchor the index at the host clock so the packets' UTC seconds field
    // reads as the current time, like a USRP whose clock was set from the
    // host. Alignment never depends on this; it only makes logs readable.
    const uint64_t uUnixSeconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    muStartIndex = uUnixSeconds * muSampleRateHz;
    muIndex      = muStartIndex;
    moStart      = std::chrono::steady_clock::now();
    mbStarted    = true;

    std::printf("SynthSource: %llu Hz, target at delay %llu samples, Doppler %llu Hz (mod fs)\n",
                static_cast<unsigned long long>(muSampleRateHz),
                static_cast<unsigned long long>(muTargetDelay),
                static_cast<unsigned long long>(muDopplerSteps));
    return true;
}

bool SynthSource::receive(int16_t* pRef, int16_t* pSur, uint64_t uWant,
                          uint64_t& uGotOut, uint64_t& uFirstIndexOut,
                          bool& bOverflowOut)
{
    uGotOut       = 0;
    uFirstIndexOut = 0;
    bOverflowOut  = false;

    CHECK(mbStarted, "SynthSource: receive() before start()");
    CHECK(pRef != nullptr && pSur != nullptr, "SynthSource: null buffers");

    // Pace to the real rate: this chunk's last sample "arrives" at
    // start + (index + uWant - startIndex) / fs. Absolute schedule, so
    // per-call delays do not accumulate into a drifting rate.
    const double dDueSeconds =
        static_cast<double>(muIndex + uWant - muStartIndex) /
        static_cast<double>(muSampleRateHz);
    std::this_thread::sleep_until(
        moStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(dDueSeconds)));

    const double dTwoPiOverFs = kTwoPi / static_cast<double>(muSampleRateHz);

    for (uint64_t u = 0; u < uWant; ++u) {
        const uint64_t uN = muIndex + u;

        const double dRefI = kRefRmsCodes * noiseAt(kRefSeedI, uN);
        const double dRefQ = kRefRmsCodes * noiseAt(kRefSeedQ, uN);

        // Target: the reference as it was muTargetDelay samples ago, rotated
        // by the Doppler phase. ((uN mod fs) * fd) mod fs equals (uN * fd) mod
        // fs, so the phase stays exact and continuous without overflow.
        const uint64_t uPhaseSteps = ((uN % muSampleRateHz) * muDopplerSteps) % muSampleRateHz;
        const double   dPhase      = dTwoPiOverFs * static_cast<double>(uPhaseSteps);
        const double   dCos        = std::cos(dPhase);
        const double   dSin        = std::sin(dPhase);

        const double dDelI = kRefRmsCodes * noiseAt(kRefSeedI, uN - muTargetDelay);
        const double dDelQ = kRefRmsCodes * noiseAt(kRefSeedQ, uN - muTargetDelay);
        const double dTgtI = kTargetGain * (dDelI * dCos - dDelQ * dSin);
        const double dTgtQ = kTargetGain * (dDelI * dSin + dDelQ * dCos);

        const double dSurI = kDirectPathGain * dRefI + dTgtI + kSurNoiseRms * noiseAt(kSurSeedI, uN);
        const double dSurQ = kDirectPathGain * dRefQ + dTgtQ + kSurNoiseRms * noiseAt(kSurSeedQ, uN);

        pRef[2 * u]     = toCode(dRefI);
        pRef[2 * u + 1] = toCode(dRefQ);
        pSur[2 * u]     = toCode(dSurI);
        pSur[2 * u + 1] = toCode(dSurQ);
    }

    uGotOut        = uWant;
    uFirstIndexOut = muIndex;
    muIndex       += uWant;
    return true;
}

void SynthSource::stop()
{
    mbStarted = false;
}

} // namespace pbr
