// uhd_to_vita49 -- two channels from a USRP N210 over UHD, streamed as
// VITA 49.2 UDP in the FERS profile that pbr_run --live already accepts.
//
//   USRP2 / N210 with BasicRX: RX-A = reference (ch 0), RX-B = surveillance (ch 1)
//       -> UHD sc16 recv()  ->  Vita49Writer  ->  UdpSender  ->  pbr_run --live
//
// The lab's board (probed 2026-09-25) is a USRP2 r4, serial 627, at its
// factory address 192.168.10.2, FW 12.4 / FPGA 10.1 on its SD card, ltc2284
// 14-bit ADC, BasicRX on slot A. UHD treats the USRP2 and the N2x0 the same
// way and everything here applies to both; the device name in the context
// packets is read from the board rather than assumed.
//
// Usage:
//   ./uhd_to_vita49 --config config/prb.conf                    # the USRP
//   ./uhd_to_vita49 --synth --dest 127.0.0.1:4991 --seconds 30   # no hardware
//   ./uhd_to_vita49 --args addr=192.168.10.2 --fc 89e6 --ext-ref
//
// Design: docs/UHD_VITA49_Driver_design.md. Replaces the legacy UHDToPXGF;
// the receiver cannot tell this stream from FERS, so nothing downstream of
// the UDP socket changes.
//
// Hardware facts carried over from the legacy help text:
//   * Channel 0 (reference) is RX-A and channel 1 (surveillance) is RX-B on
//     a BasicRX daughterboard, subdev "A:A A:B". BasicRX has no tuner, gain
//     stage or IF filter: the tune programs the DDC only, and --gain0/--gain1
//     exist for a future daughterboard.
//   * External reference levels: USRP2 5 to 15 dBm, N2xx 0 to 15 dBm on the
//     10 MHz input; PPS 5 Vpp (USRP2), 3.3 to 5 Vpp (N2xx).
//
// Single thread and no ring buffer, on purpose. At 200 kS/s a 342-sample
// recv() returns every 1.7 ms; building and sending two datagrams costs tens
// of microseconds. UHD and the N210 buffer on their side. If overflows appear
// on the bench, a producer thread is the escalation, proposed separately.
//
// Timestamps never come from the wall clock. One integer sample counter,
// anchored from the USRP's own tick count on the first recv(), is converted
// exactly by makeTime(); a disagreement between the counter and UHD's tick on
// any later recv() is a gap, which re-anchors the counter and flags sample
// loss so the ingest zero-fills instead of shifting.
//
// Exception boundary: UHD throws. Every UHD call lives inside UsrpSource,
// each method wraps its calls in one try/catch and converts to a return code
// and an stderr line. Nothing else in this program throws or catches.

#include "pbr/Config.hpp"
#include "pbr/SampleSource.hpp"
#include "pbr/UdpSender.hpp"
#include "pbr/Vita49Writer.hpp"
#include "pbr/check.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#ifdef PBR_HAVE_UHD
#include <uhd/exception.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/version.hpp>
#endif

using namespace pbr;

namespace {

// Multicast group, organisation-local scope: every client on the switch that
// joins it receives the stream; TTL 1 keeps it off any router. Loopback
// (--dest 127.0.0.1:4991) is the one-receiver alternative for bring-up.
const char* const kDefaultDestHost = "239.192.0.1";
const uint16_t    kDefaultDestPort = 4991;

const uint64_t kMaxSampleRateHz  = 10000000;   // makeTime() overflow bound
const size_t   kDatagramCapacity = 2048;

struct Options {
    std::string sConfigPath;                    // empty: flags and defaults only
    std::string sArgs;                          // UHD device args, "" = first found
    double      dFs       = 200000.0;
    double      dFc       = 89e6;
    double      dGain0    = 0.0;
    double      dGain1    = 0.0;
    bool        bExtRef   = false;
    std::string sDestHost = kDefaultDestHost;
    uint16_t    uDestPort = kDefaultDestPort;
    std::string sIface;                         // outgoing interface for multicast
    uint64_t    uSamples  = 342;                // per data packet, FERS's default
    std::string sRefName  = "ArmasuisseRefRxClean";
    std::string sSurName  = "ArmasuisseSurRxClean";
    double      dSeconds  = 0.0;                // 0 = until Ctrl-C
    bool        bSynth    = false;
    bool        bSynthSilent = false;           // all-zero samples: tests the chain's blank-frame path
    uint64_t    uSynthDelay   = 20;             // planted target, samples
    int64_t     iSynthDoppler = 37;             // planted target, Hz

    // Which of the config-backed values were given explicitly, so a flag
    // beats the config file whatever the order on the command line.
    bool bFsGiven = false, bRefNameGiven = false, bSurNameGiven = false, bDestGiven = false;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s [options]\n", pProgram);
    std::printf("  --config FILE     read SAMPLE_RATE_Hz, receiver names and UDP_PORT from a\n");
    std::printf("                    chain config, so the two sides cannot disagree\n");
    std::printf("  --args ARGS       UHD device args, e.g. addr=192.168.10.2 (default: first found)\n");
    std::printf("  --fs HZ           sample rate, an integer divisor of 100 MHz (default 200000)\n");
    std::printf("  --fc HZ           centre frequency (default 89e6)\n");
    std::printf("  --gain0 DB        channel 0 (reference) gain; no effect on BasicRX\n");
    std::printf("  --gain1 DB        channel 1 (surveillance) gain; no effect on BasicRX\n");
    std::printf("  --ext-ref         external 10 MHz + PPS reference\n");
    std::printf("  --dest HOST:PORT  destination (default %s:%u, a multicast group)\n",
                kDefaultDestHost, static_cast<unsigned>(kDefaultDestPort));
    std::printf("  --iface IPV4      outgoing interface address for multicast\n");
    std::printf("  --samples N       samples per data packet, at most 512 (default 342)\n");
    std::printf("  --ref-name NAME   receiver.name for stream 1 (default FERS's)\n");
    std::printf("  --sur-name NAME   receiver.name for stream 2 (default FERS's)\n");
    std::printf("  --seconds N       stop after N seconds (default: until Ctrl-C)\n");
    std::printf("  --synth           synthetic two-channel signal instead of the USRP\n");
    std::printf("  --synth-silent    with --synth: send exact zeros (a dead front end)\n");
    std::printf("  --synth-delay N   planted target delay in samples (default 20)\n");
    std::printf("  --synth-doppler HZ planted target Doppler, whole Hz (default 37)\n");
}

bool parseDest(const std::string& sDest, std::string& sHostOut, uint16_t& uPortOut)
{
    const size_t uColon = sDest.rfind(':');
    CHECK(uColon != std::string::npos && uColon > 0 && uColon + 1 < sDest.size(),
          "--dest must be HOST:PORT");
    sHostOut = sDest.substr(0, uColon);
    const long lPort = std::strtol(sDest.c_str() + uColon + 1, nullptr, 10);
    CHECK(lPort >= 1 && lPort <= 65535, "--dest port must be 1..65535");
    uPortOut = static_cast<uint16_t>(lPort);
    return true;
}

bool parseArgs(int argc, char* argv[], Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bV = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h")         return false;
        else if (sArg == "--config" && bV)            o.sConfigPath = argv[++i];
        else if (sArg == "--args" && bV)              o.sArgs = argv[++i];
        else if (sArg == "--fs" && bV)              { o.dFs = std::atof(argv[++i]); o.bFsGiven = true; }
        else if (sArg == "--fc" && bV)                o.dFc = std::atof(argv[++i]);
        else if (sArg == "--gain0" && bV)             o.dGain0 = std::atof(argv[++i]);
        else if (sArg == "--gain1" && bV)             o.dGain1 = std::atof(argv[++i]);
        else if (sArg == "--ext-ref")                 o.bExtRef = true;
        else if (sArg == "--dest" && bV) {
            if (!parseDest(argv[++i], o.sDestHost, o.uDestPort))
                return false;
            o.bDestGiven = true;
        }
        else if (sArg == "--iface" && bV)             o.sIface = argv[++i];
        else if (sArg == "--samples" && bV)           o.uSamples = std::strtoull(argv[++i], nullptr, 10);
        else if (sArg == "--ref-name" && bV)        { o.sRefName = argv[++i]; o.bRefNameGiven = true; }
        else if (sArg == "--sur-name" && bV)        { o.sSurName = argv[++i]; o.bSurNameGiven = true; }
        else if (sArg == "--seconds" && bV)           o.dSeconds = std::atof(argv[++i]);
        else if (sArg == "--synth")                   o.bSynth = true;
        else if (sArg == "--synth-silent")            o.bSynthSilent = true;
        else if (sArg == "--synth-delay" && bV)       o.uSynthDelay = std::strtoull(argv[++i], nullptr, 10);
        else if (sArg == "--synth-doppler" && bV)     o.iSynthDoppler = std::strtoll(argv[++i], nullptr, 10);
        else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }
    return true;
}

// Whole seconds of host time, for the session id.
long hostNowSecondsCoarse()
{
    return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// Ctrl-C sets a flag; the loop notices, closes the streams and reports.
volatile std::sig_atomic_t g_iStop = 0;

void onSignal(int)
{
    g_iStop = 1;
}

// ---------------------------------------------------------------------------
// UsrpSource: the only code that talks to UHD. UHD 3.10 API.
// Sequence, in this order because it matters: make -> subdev -> time -> rate
// -> freq -> gain -> stream -> timed start (design note 3.2, 6.4).
// ---------------------------------------------------------------------------
#ifdef PBR_HAVE_UHD

const double kStartDelaySeconds  = 2.0;   // timed-start margin (design note 11)
const double kRecvTimeoutSeconds = 1.0;

double hostNowSeconds()
{
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class UsrpSource : public SampleSource {
public:
    bool open(const std::string& sArgs, double dFs, double dFc,
              double dGain0, double dGain1, bool bExtRef);

    bool start() override;
    bool receive(int16_t* pRef, int16_t* pSur, uint64_t uWant,
                 uint64_t& uGotOut, uint64_t& uFirstIndexOut,
                 bool& bOverflowOut) override;
    void stop() override;

    double sampleRateHz() const override { return mdActualRateHz; }
    bool referenceLocked() const override { return mbExtRef && mbRefLocked; }

    // UHD's name for the motherboard, e.g. "USRP2 r4" or "N210r4".
    const std::string& mboardName() const { return msMboardName; }

    uint64_t timeouts()    const { return muTimeouts; }
    uint64_t overflows()   const { return muOverflows; }
    uint64_t otherErrors() const { return muOtherErrors; }

private:
    uhd::usrp::multi_usrp::sptr mpUsrp;
    uhd::rx_streamer::sptr      mpRx;
    std::vector<void*>          mvBuffs = std::vector<void*>(2, nullptr);
    std::string                 msMboardName;

    double   mdActualRateHz = 0.0;
    bool     mbExtRef       = false;
    bool     mbRefLocked    = false;
    size_t   muMaxSamps     = 0;
    bool     mbFirstRecv    = true;

    uint64_t muTimeouts            = 0;
    uint64_t muConsecutiveTimeouts = 0;
    uint64_t muRestarts            = 0;
    uint64_t muOverflows           = 0;
    uint64_t muOtherErrors         = 0;
    std::chrono::steady_clock::time_point moLastTimeoutLog;
    bool     mbHaveTimeoutLog = false;
};

// Timeouts in a row before the stream command is re-issued. With a 1 s
// receive timeout this is ten seconds of silence: a pulled cable, a power
// cycle. The driver never gives up on its own (author's rule: no timeouts
// that end a program); it keeps re-issuing and keeps saying so.
const uint64_t kTimeoutsBeforeRestart = 10;

bool UsrpSource::open(const std::string& sArgs, double dFs, double dFc,
                      double dGain0, double dGain1, bool bExtRef)
{
    mbExtRef = bExtRef;
    try {
        std::printf("UHD %s\n", uhd::get_version_string().c_str());
        std::printf("Creating the USRP device with args \"%s\" ...\n", sArgs.c_str());
        mpUsrp = uhd::usrp::multi_usrp::make(uhd::device_addr_t(sArgs));

        // Two ADC channels through separate DDCs: RX-A and RX-B of the BasicRX.
        mpUsrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t("A:A A:B"));
        std::printf("%s\n", mpUsrp->get_pp_string().c_str());
        msMboardName = mpUsrp->get_mboard_name();
        CHECK(mpUsrp->get_rx_num_channels() == 2,
              "expected two RX channels from subdev A:A A:B");

        // Time base. The host clock seeds the USRP once, here; every packet
        // time afterwards comes from the USRP's tick counter.
        if (bExtRef) {
            std::printf("External 10 MHz and PPS reference\n");
            mpUsrp->set_clock_source("external");
            mpUsrp->set_time_source("external");
            // Applied at the PPS edge after the next one UHD sees, so the
            // seconds field is the host's UTC to about a second.
            const double dNow = hostNowSeconds();
            mpUsrp->set_time_unknown_pps(uhd::time_spec_t(std::floor(dNow) + 2.0));
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            mbRefLocked = mpUsrp->get_mboard_sensor("ref_locked").to_bool();
            CHECK(mbRefLocked, "the external 10 MHz reference is not locked");
        } else {
            std::printf("Internal reference; USRP time set from the host clock\n");
            mpUsrp->set_time_now(uhd::time_spec_t(hostNowSeconds()));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::printf("USRP time %.6f, host time %.6f\n",
                    mpUsrp->get_time_now().get_real_secs(), hostNowSeconds());

        // Rate before frequency: the DDC tune depends on the decimation.
        mpUsrp->set_rx_rate(dFs);
        mdActualRateHz = mpUsrp->get_rx_rate();
        std::printf("Sample rate: requested %.3f Hz, USRP delivers %.3f Hz\n", dFs, mdActualRateHz);
        CHECK(std::fabs(mdActualRateHz - dFs) < 1e-6 * dFs,
              "the USRP cannot produce the requested rate; use an integer divisor of 100 MHz");

        for (size_t uCh = 0; uCh < 2; ++uCh) {
            // BasicRX: no RF tuner, so this programs the DDC with dFc directly.
            // 89 MHz sampled at 100 MSPS aliases to -11 MHz and the DDC treats
            // them as the same frequency, so the spectrum should come out the
            // right way round. Verify on the bench (design note 9, step 8).
            mpUsrp->set_rx_freq(uhd::tune_request_t(dFc), uCh);

            const double dGain = (uCh == 0) ? dGain0 : dGain1;
            if (!mpUsrp->get_rx_gain_names(uCh).empty())
                mpUsrp->set_rx_gain(dGain, uCh);
            else if (dGain != 0.0)
                std::printf("channel %zu: no gain stage on this daughterboard; --gain%zu ignored\n",
                            uCh, uCh);

            std::printf("channel %zu: %s, freq %.1f Hz, gain %.2f dB, antenna %s\n",
                        uCh, (uCh == 0) ? "reference   " : "surveillance",
                        mpUsrp->get_rx_freq(uCh), mpUsrp->get_rx_gain(uCh),
                        mpUsrp->get_rx_antenna(uCh).c_str());
        }

        // sc16 on the wire and in memory: int16 I, int16 Q, no conversion.
        uhd::stream_args_t oArgs("sc16", "sc16");
        oArgs.channels.push_back(0);
        oArgs.channels.push_back(1);
        mpRx = mpUsrp->get_rx_stream(oArgs);
        CHECK(mpRx->get_num_channels() == 2, "the RX streamer did not give two channels");

        muMaxSamps = mpRx->get_max_num_samps();
        std::printf("RX streamer: %zu samples per packet per channel at most\n", muMaxSamps);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UsrpSource: UHD failed during open: %s\n", e.what());
        return false;
    }
    return true;
}

bool UsrpSource::start()
{
    CHECK(mpRx, "UsrpSource::start before open");
    try {
        // Timed start so both channels begin on the same tick.
        uhd::stream_cmd_t oCmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        oCmd.stream_now = false;
        oCmd.time_spec  = mpUsrp->get_time_now() + kStartDelaySeconds;
        mpRx->issue_stream_cmd(oCmd);
        std::printf("Streaming starts in %.1f s, at USRP time %.6f\n",
                    kStartDelaySeconds, oCmd.time_spec.get_real_secs());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UsrpSource: UHD failed during start: %s\n", e.what());
        return false;
    }
    return true;
}

bool UsrpSource::receive(int16_t* pRef, int16_t* pSur, uint64_t uWant,
                         uint64_t& uGotOut, uint64_t& uFirstIndexOut,
                         bool& bOverflowOut)
{
    uGotOut        = 0;
    uFirstIndexOut = 0;
    bOverflowOut   = false;

    try {
        mvBuffs[0] = pRef;
        mvBuffs[1] = pSur;

        // One metadata for both channels: UHD aligns them, and the timestamp
        // is that of the first sample of both.
        // The first recv() also waits out the timed start, so it gets the
        // start margin on top of the normal timeout; otherwise every run
        // opens with one spurious "timeout" line.
        const double dTimeout = mbFirstRecv ? kStartDelaySeconds + kRecvTimeoutSeconds
                                            : kRecvTimeoutSeconds;
        mbFirstRecv = false;

        uhd::rx_metadata_t oMd;
        const size_t uGot = mpRx->recv(mvBuffs, static_cast<size_t>(uWant), oMd,
                                       dTimeout, false);

        switch (oMd.error_code) {
        case uhd::rx_metadata_t::ERROR_CODE_NONE:
            break;

        case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT: {
            // No data, no packets: the ingest sees a gap. Log once a second.
            ++muTimeouts;
            ++muConsecutiveTimeouts;
            const std::chrono::steady_clock::time_point oNow = std::chrono::steady_clock::now();
            if (!mbHaveTimeoutLog ||
                std::chrono::duration<double>(oNow - moLastTimeoutLog).count() >= 1.0) {
                std::fprintf(stderr, "UsrpSource: recv timeout (%llu so far); is the USRP "
                                     "still connected?\n",
                             static_cast<unsigned long long>(muTimeouts));
                moLastTimeoutLog = oNow;
                mbHaveTimeoutLog = true;
            }
            // A quiet USRP gets its stream command again, as often as it
            // takes. When the board comes back it starts on a new tick and
            // the caller's index check re-anchors.
            if (muConsecutiveTimeouts % kTimeoutsBeforeRestart == 0) {
                ++muRestarts;
                std::fprintf(stderr, "UsrpSource: %llu s without samples; re-issuing the "
                                     "stream command (restart %llu)\n",
                             static_cast<unsigned long long>(muConsecutiveTimeouts),
                             static_cast<unsigned long long>(muRestarts));
                mpRx->issue_stream_cmd(uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS));
                uhd::stream_cmd_t oCmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
                oCmd.stream_now = false;
                oCmd.time_spec  = mpUsrp->get_time_now() + kStartDelaySeconds;
                mpRx->issue_stream_cmd(oCmd);
                mbFirstRecv = true;   // give the timed start its margin again
            }
            return true;
        }

        case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
            // The next good recv() carries a later tick than our counter
            // expects; the caller re-anchors there and flags sample loss.
            ++muOverflows;
            bOverflowOut = true;
            std::fprintf(stderr, "UsrpSource: overflow %llu%s\n",
                         static_cast<unsigned long long>(muOverflows),
                         oMd.out_of_sequence ? " (out of sequence)" : "");
            break;

        case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
            // The timed start was missed: setup took longer than the margin.
            std::fprintf(stderr, "UsrpSource: late command -- the timed start was missed\n");
            return false;

        default:
            ++muOtherErrors;
            std::fprintf(stderr, "UsrpSource: %s\n", oMd.strerror().c_str());
            break;
        }

        if (uGot > 0) {
            muConsecutiveTimeouts = 0;
            if (!oMd.has_time_spec) {
                std::fprintf(stderr, "UsrpSource: %zu samples without a timestamp; dropped\n", uGot);
                return true;
            }
            // UHD's own rounding of full + fractional seconds to a tick count
            // at the actual rate, applied once per packet. Both channels share it.
            uFirstIndexOut = static_cast<uint64_t>(oMd.time_spec.to_ticks(mdActualRateHz));
            uGotOut        = uGot;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UsrpSource: UHD failed during recv: %s\n", e.what());
        return false;
    }
    return true;
}

void UsrpSource::stop()
{
    if (!mpRx)
        return;
    try {
        mpRx->issue_stream_cmd(uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UsrpSource: UHD failed during stop: %s\n", e.what());
    }
    std::printf("UsrpSource: %llu timeouts, %llu stream restarts, %llu overflows, "
                "%llu other errors\n",
                static_cast<unsigned long long>(muTimeouts),
                static_cast<unsigned long long>(muRestarts),
                static_cast<unsigned long long>(muOverflows),
                static_cast<unsigned long long>(muOtherErrors));
}

#endif // PBR_HAVE_UHD

// ---------------------------------------------------------------------------
// Per-stream packet state. The mod-16 counter advances on every packet of the
// stream, context included, or the ingest reports a gap per heartbeat.
// ---------------------------------------------------------------------------
struct StreamState {
    Vita49StreamInfo oInfo;
    uint8_t  uPacketCount       = 0;
    uint64_t uLastContextIndex  = 0;
    bool     bContextLossPending = false;   // set on re-anchor, cleared by the next context
    uint64_t uDataPackets       = 0;
    uint64_t uContextPackets    = 0;
};

// A writer failure is a programming error (bad name, buffer too small) and is
// fatal. A send failure is counted by the sender and is not: the receiver may
// start later.
bool sendContext(StreamState& oStream, UdpSender& oSender, uint8_t* pBuf, size_t uCap,
                 uint64_t uSampleIndex, uint64_t uFs, bool bLocked, uint32_t uFlags)
{
    if (oStream.bContextLossPending)
        uFlags |= kVita49FlagSampleLoss;

    size_t uLength = 0;
    if (!writeVita49Context(pBuf, uCap, oStream.oInfo, oStream.uPacketCount,
                            makeTime(uSampleIndex, uFs), bLocked, uFlags, uLength))
        return false;

    oStream.uPacketCount        = static_cast<uint8_t>((oStream.uPacketCount + 1u) & 0x0Fu);
    oStream.uLastContextIndex   = uSampleIndex;
    oStream.bContextLossPending = false;
    ++oStream.uContextPackets;
    oSender.send(pBuf, uLength);
    return true;
}

bool sendData(StreamState& oStream, UdpSender& oSender, uint8_t* pBuf, size_t uCap,
              uint64_t uSampleIndex, uint64_t uFs, const int16_t* pSamples, uint64_t uCount,
              bool bLocked, bool bSampleLoss)
{
    size_t uLength = 0;
    if (!writeVita49Data(pBuf, uCap, oStream.oInfo.uStreamId, oStream.uPacketCount,
                         makeTime(uSampleIndex, uFs), pSamples, uCount,
                         bLocked, bSampleLoss, uLength))
        return false;

    oStream.uPacketCount = static_cast<uint8_t>((oStream.uPacketCount + 1u) & 0x0Fu);
    ++oStream.uDataPackets;
    oSender.send(pBuf, uLength);
    return true;
}

// One context packet on each stream, with the same flags.
bool sendContexts(StreamState (&aStreams)[2], UdpSender& oSender, std::vector<uint8_t>& vDatagram,
                  uint64_t uSampleIndex, uint64_t uFs, bool bLocked, uint32_t uFlags)
{
    for (unsigned u = 0; u < 2; ++u) {
        if (!sendContext(aStreams[u], oSender, vDatagram.data(), vDatagram.size(),
                         uSampleIndex, uFs, bLocked, uFlags))
            return false;
    }
    return true;
}

// One recv()'s samples as one data packet per stream, reference first, each
// preceded by its heartbeat context once per second of samples.
bool sendBlock(StreamState (&aStreams)[2], UdpSender& oSender, std::vector<uint8_t>& vDatagram,
               uint64_t uSampleIndex, uint64_t uFs, const int16_t* const apSamples[2],
               uint64_t uCount, bool bLocked, bool bSampleLoss)
{
    for (unsigned u = 0; u < 2; ++u) {
        StreamState& oStream = aStreams[u];
        if (uSampleIndex - oStream.uLastContextIndex >= uFs &&
            !sendContext(oStream, oSender, vDatagram.data(), vDatagram.size(),
                         uSampleIndex, uFs, bLocked, 0))
            return false;
        if (!sendData(oStream, oSender, vDatagram.data(), vDatagram.size(),
                      uSampleIndex, uFs, apSamples[u], uCount, bLocked, bSampleLoss))
            return false;
    }
    return true;
}

// The chain's config supplies what both sides must agree on. Flags given
// explicitly still win, so a rate can be probed without editing the file.
bool applyConfigFile(Options& o)
{
    if (o.sConfigPath.empty())
        return true;
    PbrConfig oCfg;
    if (!loadPbrConfig(o.sConfigPath.c_str(), oCfg))
        return false;
    if (!o.bFsGiven)      o.dFs      = static_cast<double>(oCfg.uSampleRateHz);
    if (!o.bRefNameGiven) o.sRefName = oCfg.sReferenceName;
    if (!o.bSurNameGiven) o.sSurName = oCfg.sSurveillanceName;
    if (!o.bDestGiven)    o.uDestPort = oCfg.uUdpPort;
    if (oCfg.dFullScale != 1.0)
        std::fprintf(stderr,
            "WARNING: %s has VITA49_FULLSCALE = %g; this driver sends raw ADC codes\n"
            "         and declares full scale 1.0, so the chain will scale amplitudes\n"
            "         by %g. Set it to 1.0 for USRP runs.\n",
            o.sConfigPath.c_str(), oCfg.dFullScale, oCfg.dFullScale);
    return true;
}

// Sanity on what the packets will say. uFsOut is the rate as a whole number.
bool checkOptions(const Options& o, uint64_t& uFsOut)
{
    uFsOut = static_cast<uint64_t>(std::llround(o.dFs));
    if (o.dFs <= 0.0 || std::fabs(o.dFs - static_cast<double>(uFsOut)) > 1e-9 ||
        uFsOut > kMaxSampleRateHz) {
        std::fprintf(stderr, "sample rate must be a whole number of hertz, at most %llu\n",
                     static_cast<unsigned long long>(kMaxSampleRateHz));
        return false;
    }
    if (o.uSamples == 0 || o.uSamples > 512) {
        std::fprintf(stderr, "--samples must be 1..512\n");
        return false;
    }
    if (o.sRefName.empty() || o.sSurName.empty() || o.sRefName == o.sSurName) {
        std::fprintf(stderr, "receiver names must be non-empty and different\n");
        return false;
    }
    return true;
}

// The synthetic source or the USRP. Returns the exit code to stop with, or 0.
int openSource(const Options& o, uint64_t uFs, std::unique_ptr<SampleSource>& pSource,
               std::string& sDeviceOut, std::string& sDriverVersionOut)
{
    sDriverVersionOut = "none";
    if (o.bSynth) {
        pSource.reset(new SynthSource(uFs, o.uSynthDelay, o.iSynthDoppler));
        sDeviceOut = "synth";
        return 0;
    }
#ifdef PBR_HAVE_UHD
    UsrpSource* pUsrp = new UsrpSource();
    pSource.reset(pUsrp);
    if (!pUsrp->open(o.sArgs, o.dFs, o.dFc, o.dGain0, o.dGain1, o.bExtRef))
        return 2;
    sDeviceOut        = "USRP " + pUsrp->mboardName() + " BasicRX";
    sDriverVersionOut = uhd::get_version_string();
    return 0;
#else
    std::fprintf(stderr,
        "this build has no UHD, so there is no USRP path. Build on the Jetson with\n"
        "libuhd-dev installed, or run with --synth.\n");
    return 1;
#endif
}

// A session id that changes on every start, so the ingest can tell a driver
// restart, or a second driver on the same group, from a gap.
std::string makeSessionId()
{
    char achSession[16];
    std::snprintf(achSession, sizeof(achSession), "%08x",
                  static_cast<unsigned>(
                      (static_cast<uint64_t>(hostNowSecondsCoarse()) * 2654435761ULL) ^
                      (static_cast<uint64_t>(getpid()) << 16)) & 0xFFFFFFFFu);
    return achSession;
}

void describeStreams(const Options& o, double dActualFs, const std::string& sDevice,
                     const std::string& sDriverVersion, const std::string& sSessionId,
                     StreamState (&aStreams)[2])
{
    for (unsigned u = 0; u < 2; ++u) {
        Vita49StreamInfo& oInfo = aStreams[u].oInfo;
        oInfo.uStreamId      = u + 1;
        oInfo.uReceiverId    = u + 1;
        oInfo.sReceiverName  = (u == 0) ? o.sRefName : o.sSurName;
        oInfo.dSampleRateHz  = dActualFs;
        oInfo.dCentreFreqHz  = o.dFc;
        oInfo.dBandwidthHz   = dActualFs;
        oInfo.dAdcFullScale  = 1.0;
        oInfo.dGainDb        = (u == 0) ? o.dGain0 : o.dGain1;
        oInfo.uAdcBits       = 14;
        oInfo.uChannel       = u;
        oInfo.bExtRef        = o.bExtRef;
        oInfo.sDevice        = sDevice;
        oInfo.sDriverVersion = sDriverVersion;
        oInfo.sSessionId     = sSessionId;
    }
}

void printSetup(const Options& o, uint64_t uFs, const std::string& sDevice,
                const std::string& sSessionId)
{
    std::printf("\nuhd_to_vita49\n");
    std::printf("  source        %s\n", sDevice.c_str());
    std::printf("  sample rate   %llu Hz\n", static_cast<unsigned long long>(uFs));
    std::printf("  centre freq   %.1f Hz\n", o.dFc);
    std::printf("  streams       1 \"%s\" (ref)  2 \"%s\" (sur)\n",
                o.sRefName.c_str(), o.sSurName.c_str());
    std::printf("  session       %s\n", sSessionId.c_str());
    std::printf("  packet        %llu samples, %llu bytes, %.2f ms\n",
                static_cast<unsigned long long>(o.uSamples),
                static_cast<unsigned long long>(kVita49DataOverheadBytes + 4 * o.uSamples),
                1000.0 * static_cast<double>(o.uSamples) / static_cast<double>(uFs));
    std::printf("  run for       %s\n\n", o.dSeconds > 0.0 ? "--seconds" : "until Ctrl-C");
}

// What the loop counts, for the summary.
struct Counts {
    uint64_t uSamplesSent = 0;
    uint64_t uOverflows   = 0;
    uint64_t uReanchors   = 0;
};

void printSummary(double dElapsed, uint64_t uFs, const StreamState (&aStreams)[2],
                  const Counts& oCounts, const UdpSender& oSender)
{
    std::printf("\n================ uhd_to_vita49 summary ================\n");
    std::printf("elapsed            %.2f s\n", dElapsed);
    std::printf("samples per stream %llu  (%.2f s of signal)\n",
                static_cast<unsigned long long>(oCounts.uSamplesSent),
                static_cast<double>(oCounts.uSamplesSent) / static_cast<double>(uFs));
    for (unsigned u = 0; u < 2; ++u)
        std::printf("stream %u           %llu data, %llu context packets\n", u + 1,
                    static_cast<unsigned long long>(aStreams[u].uDataPackets),
                    static_cast<unsigned long long>(aStreams[u].uContextPackets));
    std::printf("overflows          %llu\n", static_cast<unsigned long long>(oCounts.uOverflows));
    std::printf("re-anchors (gaps)  %llu\n", static_cast<unsigned long long>(oCounts.uReanchors));
    std::printf("send failures      %llu\n", static_cast<unsigned long long>(oSender.sendFailures()));
    std::printf("=======================================================\n");
}

} // namespace

int main(int argc, char* argv[])
{
    Options o;
    if (!parseArgs(argc, argv, o)) {
        printUsage(argv[0]);
        return 1;
    }
    uint64_t uFs = 0;
    if (!applyConfigFile(o) || !checkOptions(o, uFs))
        return 1;

    std::unique_ptr<SampleSource> pSource;
    std::string sDevice, sDriverVersion;
    const int iOpenCode = openSource(o, uFs, pSource, sDevice, sDriverVersion);
    if (iOpenCode != 0)
        return iOpenCode;

    // The source's actual rate is what the packets carry; it must be the
    // integer the ingest is configured with.
    const double dActualFs = pSource->sampleRateHz();
    if (std::fabs(dActualFs - static_cast<double>(uFs)) > 1e-6) {
        std::fprintf(stderr, "source delivers %.6f Hz, not the %llu Hz the packets would claim\n",
                     dActualFs, static_cast<unsigned long long>(uFs));
        return 2;
    }

    const std::string sSessionId = makeSessionId();
    StreamState aStreams[2];
    describeStreams(o, dActualFs, sDevice, sDriverVersion, sSessionId, aStreams);

    UdpSender oSender;
    if (!oSender.open(o.sDestHost.c_str(), o.uDestPort, o.sIface.c_str()))
        return 1;
    printSetup(o, uFs, sDevice, sSessionId);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // Buffers, allocated once: 2 * N int16 per channel plus one datagram.
    std::vector<int16_t> vRef(static_cast<size_t>(2 * o.uSamples));
    std::vector<int16_t> vSur(static_cast<size_t>(2 * o.uSamples));
    std::vector<uint8_t> vDatagram(kDatagramCapacity);
    const int16_t* const apSamples[2] = { vRef.data(), vSur.data() };

    if (!pSource->start())
        return 2;

    const std::chrono::steady_clock::time_point oStart = std::chrono::steady_clock::now();

    // ---- the loop -----------------------------------------------------------
    Counts   oCounts;
    bool     bAnchored    = false;
    bool     bOpened      = false;
    bool     bLossPending = false;   // next data packets carry the sample-loss bit
    bool     bFatal       = false;
    uint64_t uSampleIndex = 0;       // absolute index of the next packet's first sample

    while (!g_iStop) {
        if (o.dSeconds > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - oStart).count()
                >= o.dSeconds)
            break;

        uint64_t uGot      = 0;
        uint64_t uSrcIndex = 0;
        bool     bOverflow = false;
        if (!pSource->receive(vRef.data(), vSur.data(), o.uSamples, uGot, uSrcIndex, bOverflow)) {
            bFatal = true;
            break;
        }
        if (bOverflow)
            ++oCounts.uOverflows;
        if (uGot == 0)
            continue;

        // A dead front end delivers packets full of zeros. The packets are
        // valid and the chain must say "nothing to process", not crash.
        if (o.bSynthSilent) {
            std::fill(vRef.begin(), vRef.end(), int16_t(0));
            std::fill(vSur.begin(), vSur.end(), int16_t(0));
        }

        // Anchor the counter from the source's tick, and afterwards trust the
        // counter unless the source disagrees: a disagreement is a gap.
        if (!bAnchored) {
            uSampleIndex = uSrcIndex;
            bAnchored    = true;
            const Vita49Time oT = makeTime(uSampleIndex, uFs);
            std::printf("first sample: index %llu = UTC %u.%012llu\n",
                        static_cast<unsigned long long>(uSampleIndex),
                        oT.uSeconds, static_cast<unsigned long long>(oT.uPicoseconds));
        } else if (uSrcIndex != uSampleIndex) {
            ++oCounts.uReanchors;
            const long long llDelta = static_cast<long long>(uSrcIndex) -
                                      static_cast<long long>(uSampleIndex);
            std::fprintf(stderr, "gap: source is %lld samples from the counter; re-anchoring "
                                 "(%llu so far)\n",
                         llDelta, static_cast<unsigned long long>(oCounts.uReanchors));
            uSampleIndex = uSrcIndex;
            bLossPending = true;
            aStreams[0].bContextLossPending = true;
            aStreams[1].bContextLossPending = true;
        }

        // A context packet must name each stream before its first data
        // packet, or the ingest drops the data as unresolved.
        const bool bLocked = pSource->referenceLocked();
        if (!bOpened) {
            if (!sendContexts(aStreams, oSender, vDatagram, uSampleIndex, uFs, bLocked,
                              kVita49FlagStreamOpen)) {
                bFatal = true;
                break;
            }
            bOpened = true;
        }
        if (!sendBlock(aStreams, oSender, vDatagram, uSampleIndex, uFs, apSamples, uGot,
                       bLocked, bLossPending)) {
            bFatal = true;
            break;
        }

        bLossPending  = false;
        uSampleIndex += uGot;
        oCounts.uSamplesSent += uGot;
    }

    if (bOpened)
        sendContexts(aStreams, oSender, vDatagram, uSampleIndex, uFs,
                     pSource->referenceLocked(), kVita49FlagStreamClose);
    pSource->stop();

    const double dElapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - oStart).count();
    printSummary(dElapsed, uFs, aStreams, oCounts, oSender);

    if (bFatal) {
        std::printf("FAIL -- stopped on a fatal error, see above.\n");
        return 2;
    }
    std::printf("%s\n", (oCounts.uOverflows == 0 && oCounts.uReanchors == 0)
                        ? "OK -- continuous stream, no gaps."
                        : "OK with gaps -- the ingest zero-filled them; see counts above.");
    return 0;
}
