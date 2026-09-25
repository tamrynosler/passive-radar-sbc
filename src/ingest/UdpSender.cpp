#include "pbr/UdpSender.hpp"

#include "pbr/check.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace pbr {

UdpSender::UdpSender()
    : miSocket(-1),
      mbMulticast(false),
      muFailures(0),
      mbHaveFailureLog(false)
{
    std::memset(&moDest, 0, sizeof(moDest));
}

UdpSender::~UdpSender()
{
    close();
}

bool UdpSender::open(const char* pHost, uint16_t uPort, const char* pInterfaceIp)
{
    CHECK(miSocket < 0, "UdpSender::open called on an already-open socket");
    CHECK(pHost != nullptr && pHost[0] != '\0', "UdpSender: empty destination host");
    CHECK(uPort != 0, "UdpSender: destination port is zero");

    // Resolve once, IPv4 only: the whole chain is IPv4 (UdpReceiver binds
    // AF_INET), and FERS is addressed the same way.
    addrinfo oHints;
    std::memset(&oHints, 0, sizeof(oHints));
    oHints.ai_family   = AF_INET;
    oHints.ai_socktype = SOCK_DGRAM;

    addrinfo* pResult = nullptr;
    const int iErr = ::getaddrinfo(pHost, nullptr, &oHints, &pResult);
    if (iErr != 0 || pResult == nullptr) {
        std::fprintf(stderr, "UdpSender: cannot resolve %s: %s\n", pHost, ::gai_strerror(iErr));
        return false;
    }
    std::memcpy(&moDest, pResult->ai_addr, sizeof(moDest));
    ::freeaddrinfo(pResult);
    moDest.sin_port = htons(uPort);

    mbMulticast = (ntohl(moDest.sin_addr.s_addr) >> 28) == 0xEu;

    miSocket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (miSocket < 0) {
        std::fprintf(stderr, "UdpSender: socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    // Room for a stall on the receiving side: 4 MiB is about 2.4 s of both
    // streams at 200 kS/s. Linux clamps to net.core.wmem_max, silently.
    int iSndBuf = 4 * 1024 * 1024;
    if (::setsockopt(miSocket, SOL_SOCKET, SO_SNDBUF, &iSndBuf, sizeof(iSndBuf)) < 0)
        std::fprintf(stderr, "UdpSender: SO_SNDBUF failed: %s\n", std::strerror(errno));

    // Permit a subnet broadcast destination. Harmless otherwise.
    int iBroadcast = 1;
    if (::setsockopt(miSocket, SOL_SOCKET, SO_BROADCAST, &iBroadcast, sizeof(iBroadcast)) < 0)
        std::fprintf(stderr, "UdpSender: SO_BROADCAST failed: %s\n", std::strerror(errno));

    if (mbMulticast) {
        // TTL 1: the group stays on the local segment, which is the lab
        // switch. unsigned char is the type both Linux and macOS accept.
        const unsigned char ucTtl = 1;
        if (::setsockopt(miSocket, IPPROTO_IP, IP_MULTICAST_TTL, &ucTtl, sizeof(ucTtl)) < 0) {
            std::fprintf(stderr, "UdpSender: IP_MULTICAST_TTL failed: %s\n", std::strerror(errno));
            close();
            return false;
        }

        // Local receivers (pbr_run on the same board) get a copy too. This is
        // the default on Linux; set explicitly so it is not a default we rely on.
        const unsigned char ucLoop = 1;
        if (::setsockopt(miSocket, IPPROTO_IP, IP_MULTICAST_LOOP, &ucLoop, sizeof(ucLoop)) < 0)
            std::fprintf(stderr, "UdpSender: IP_MULTICAST_LOOP failed: %s\n", std::strerror(errno));

        if (pInterfaceIp != nullptr && pInterfaceIp[0] != '\0') {
            in_addr oIface;
            if (::inet_pton(AF_INET, pInterfaceIp, &oIface) != 1) {
                std::fprintf(stderr, "UdpSender: interface address %s is not dotted IPv4\n",
                             pInterfaceIp);
                close();
                return false;
            }
            if (::setsockopt(miSocket, IPPROTO_IP, IP_MULTICAST_IF, &oIface, sizeof(oIface)) < 0) {
                std::fprintf(stderr, "UdpSender: IP_MULTICAST_IF %s failed: %s\n",
                             pInterfaceIp, std::strerror(errno));
                close();
                return false;
            }
        }
    }

    char achAddr[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &moDest.sin_addr, achAddr, sizeof(achAddr));
    std::printf("UdpSender: sending to %s:%u (%s)\n", achAddr, static_cast<unsigned>(uPort),
                mbMulticast ? "multicast, TTL 1" : "unicast or broadcast");
    return true;
}

void UdpSender::close()
{
    if (miSocket >= 0) {
        ::close(miSocket);
        miSocket = -1;
    }
}

bool UdpSender::send(const uint8_t* pBytes, size_t uLength)
{
    if (miSocket < 0 || pBytes == nullptr || uLength == 0) {
        std::fprintf(stderr, "UdpSender::send called with an invalid socket or buffer\n");
        return false;
    }

    const ssize_t iSent = ::sendto(miSocket, pBytes, uLength, 0,
                                   reinterpret_cast<const sockaddr*>(&moDest), sizeof(moDest));
    if (iSent == static_cast<ssize_t>(uLength))
        return true;

    ++muFailures;

    // Once per second at most: a receiver that has not started yet would
    // otherwise produce a thousand lines a second.
    const std::chrono::steady_clock::time_point oNow = std::chrono::steady_clock::now();
    if (!mbHaveFailureLog ||
        std::chrono::duration<double>(oNow - moLastFailureLog).count() >= 1.0) {
        if (iSent < 0)
            std::fprintf(stderr, "UdpSender: sendto failed: %s (%llu failures so far)\n",
                         std::strerror(errno), static_cast<unsigned long long>(muFailures));
        else
            std::fprintf(stderr, "UdpSender: short send, %zd of %zu bytes\n", iSent, uLength);
        moLastFailureLog = oNow;
        mbHaveFailureLog = true;
    }
    return false;
}

} // namespace pbr
