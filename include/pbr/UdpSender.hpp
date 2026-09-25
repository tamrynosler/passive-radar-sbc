#ifndef PBR_UDP_SENDER_HPP
#define PBR_UDP_SENDER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

namespace pbr {

// One datagram per call to a fixed destination: the sending half of
// UdpReceiver. Nothing is allocated in the packet path.
//
// The destination may be a unicast address (one receiver), a multicast group
// in 224.0.0.0/4 (any number of receivers that join it, which is how several
// clients read one stream on the lab switch), or a subnet broadcast address.
// The socket is configured for all three; only the address differs.
class UdpSender {
public:
    UdpSender();
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // Resolve pHost (dotted IPv4 or a name) once and remember it. For a
    // multicast destination, pInterfaceIp (dotted IPv4, may be null or empty)
    // selects the outgoing interface on a host with more than one; otherwise
    // the kernel's default route decides.
    bool open(const char* pHost, uint16_t uPort, const char* pInterfaceIp);

    void close();
    bool isOpen() const { return miSocket >= 0; }
    bool isMulticast() const { return mbMulticast; }

    // Send one datagram. A failure is counted, logged at most once per
    // second, and reported as false; the caller decides whether to care.
    // ECONNREFUSED on loopback simply means nobody is listening yet.
    bool send(const uint8_t* pBytes, size_t uLength);

    uint64_t sendFailures() const { return muFailures; }

private:
    int         miSocket;
    sockaddr_in moDest;
    bool        mbMulticast;
    uint64_t    muFailures;
    std::chrono::steady_clock::time_point moLastFailureLog;
    bool        mbHaveFailureLog;
};

} // namespace pbr

#endif // PBR_UDP_SENDER_HPP
