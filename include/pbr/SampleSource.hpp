#ifndef PBR_SAMPLE_SOURCE_HPP
#define PBR_SAMPLE_SOURCE_HPP

#include <chrono>
#include <cstdint>

namespace pbr {

// Where uhd_to_vita49 gets its two channels of sc16 samples from. Two
// implementations: UsrpSource (UHD, lives in src/app/uhd_to_vita49.cpp so
// that file is the only one to include UHD) and SynthSource (below, no
// hardware). The driver loop is written against this interface so the whole
// transport can be tested without a USRP.
//
// Samples are host-order int16 I, int16 Q pairs, one contiguous run per
// channel: exactly what UHD's sc16 recv() delivers, and exactly what
// writeVita49Data() consumes after a byte swap. No repacking anywhere.
class SampleSource {
public:
    virtual ~SampleSource() {}

    // Begin streaming. Returns false, having logged why, on failure.
    virtual bool start() = 0;

    // Fill pRef and pSur with up to uWant samples each (2 * uWant int16
    // values per buffer). uGotOut is how many arrived, and may be 0 on a
    // timeout; uFirstIndexOut is the source's own absolute index of the
    // first sample, at the actual sample rate, so the caller can detect a
    // gap by comparing it with its running counter. bOverflowOut reports a
    // source-side overflow on this call.
    //
    // Returns false only on a fatal error: the caller stops.
    virtual bool receive(int16_t* pRef, int16_t* pSur, uint64_t uWant,
                         uint64_t& uGotOut, uint64_t& uFirstIndexOut,
                         bool& bOverflowOut) = 0;

    virtual void stop() = 0;

    // The ACTUAL sample rate, as read back from the device.
    virtual double sampleRateHz() const = 0;

    // True if streaming from an external reference that is locked. Sets the
    // calibrated-time and reference-lock indicators.
    virtual bool referenceLocked() const = 0;
};

// A two-channel test signal at the real rate, no hardware.
//
// The reference carries band-limited noise standing in for an FM broadcast.
// The surveillance channel carries the same noise at the same delay (direct
// path, -20 dB), plus a copy delayed by uTargetDelaySamples and shifted by
// iTargetDopplerHz (a target, -40 dB), plus independent noise. Fed into
// pbr_run, cancellation removes the direct path and CFAR should report one
// detection at (uTargetDelaySamples, iTargetDopplerHz): an end-to-end check
// of the transport with a known answer.
//
// Every sample is a pure function of its absolute index (hashed noise), so a
// delayed copy is just the same function evaluated earlier. No delay line,
// no state beyond the running index.
//
// The design note proposed a tone; noise is used instead because a tone has
// no range resolution, so the planted target would appear at every range
// bin rather than at one.
class SynthSource : public SampleSource {
public:
    // iTargetDopplerHz must be a whole number of hertz, so the target phase
    // stays continuous across one-second boundaries in integer arithmetic.
    SynthSource(uint64_t uSampleRateHz, uint64_t uTargetDelaySamples,
                int64_t iTargetDopplerHz);

    bool start() override;
    bool receive(int16_t* pRef, int16_t* pSur, uint64_t uWant,
                 uint64_t& uGotOut, uint64_t& uFirstIndexOut,
                 bool& bOverflowOut) override;
    void stop() override;

    double sampleRateHz() const override { return static_cast<double>(muSampleRateHz); }
    bool referenceLocked() const override { return false; }

private:
    uint64_t muSampleRateHz;
    uint64_t muTargetDelay;
    uint64_t muDopplerSteps;    // Doppler in Hz, reduced modulo fs and made positive

    bool     mbStarted;
    uint64_t muIndex;           // absolute index of the next sample to produce
    uint64_t muStartIndex;
    std::chrono::steady_clock::time_point moStart;
};

} // namespace pbr

#endif // PBR_SAMPLE_SOURCE_HPP
