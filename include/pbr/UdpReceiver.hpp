#ifndef PBR_UDP_RECEIVER_HPP
#define PBR_UDP_RECEIVER_HPP

#include <cstddef>
#include <cstdint>

namespace pbr {

// Outcome of one receive attempt.
enum class ReceiveResult {
    Datagram,    // a datagram was read; uBytesOut is its length
    WouldBlock,  // nothing waiting -- not an error, go do other work
    Error        // the socket failed; details already logged to stderr
};

// A non-blocking UDP listener. One datagram per call, into a caller-owned
// buffer, so nothing is allocated in the packet path.
//
// Non-blocking on purpose: the receive loop drains everything currently
// waiting, then goes and does CPI work, then comes back. A blocking socket
// would put the parser in charge of the loop, which is the wrong shape (and
// is the mistake the legacy PXGF reader's blocking processStream() makes).
class UdpReceiver {
public:
    UdpReceiver();
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Bind to uPort on all interfaces and request a receive buffer of
    // iRequestedRcvBufBytes.
    //
    // Sizing: FERS sends ~1198 datagrams/s across both channels at 1400
    // bytes each, about 1.7 MB/s, so 4 MiB is roughly 2.4 s of slack if the
    // processing loop stalls. Note that Linux silently clamps the request to
    // net.core.rmem_max (often ~208 KiB by default) and then doubles what it
    // grants for bookkeeping -- call actualRcvBufBytes() afterwards to see
    // what was really allocated, and raise net.core.rmem_max if it is short.
    bool open(uint16_t uPort, int iRequestedRcvBufBytes = 4 * 1024 * 1024);

    // Join an IPv4 multicast group (224.0.0.0/4) on the open socket, so a
    // stream sent to that group by uhd_to_vita49 arrives here alongside any
    // other client that joined it. Unicast traffic to the port still arrives.
    bool joinMulticastGroup(const char* pGroup);

    void close();
    bool isOpen() const { return miSocket >= 0; }

    // Effective SO_RCVBUF as reported by the kernel, or 0 if not open.
    int actualRcvBufBytes() const { return miActualRcvBufBytes; }

    // Read at most one datagram into pBuffer.
    //
    // A datagram longer than uCapacity is truncated by the kernel and the
    // excess is discarded. We cannot distinguish that from an exactly-full
    // datagram portably, so callers should size pBuffer comfortably above the
    // expected 1400 bytes and treat uBytesOut == uCapacity as suspicious.
    ReceiveResult receive(uint8_t* pBuffer, size_t uCapacity, size_t& uBytesOut);

private:
    int miSocket;
    int miActualRcvBufBytes;
};

} // namespace pbr

#endif // PBR_UDP_RECEIVER_HPP
