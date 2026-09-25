#include "pbr/UdpReceiver.hpp"

#include "pbr/check.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace pbr {

UdpReceiver::UdpReceiver()
    : miSocket(-1),
      miActualRcvBufBytes(0)
{
}

UdpReceiver::~UdpReceiver()
{
    close();
}

bool UdpReceiver::open(uint16_t uPort, int iRequestedRcvBufBytes, bool bBlocking)
{
    CHECK(miSocket < 0, "UdpReceiver::open called on an already-open socket");

    miSocket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (miSocket < 0) {
        std::fprintf(stderr, "UdpReceiver: socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    // Allow a quick restart of the program without waiting on the port.
    int iReuse = 1;
    if (::setsockopt(miSocket, SOL_SOCKET, SO_REUSEADDR, &iReuse, sizeof(iReuse)) < 0)
        std::fprintf(stderr, "UdpReceiver: SO_REUSEADDR failed: %s\n", std::strerror(errno));

    // Request a large receive buffer, then read back what the kernel actually
    // granted. Linux clamps to net.core.rmem_max and doubles the granted value
    // internally, so the number reported here is typically twice what a
    // successful request asked for.
    if (::setsockopt(miSocket, SOL_SOCKET, SO_RCVBUF,
                     &iRequestedRcvBufBytes, sizeof(iRequestedRcvBufBytes)) < 0) {
        std::fprintf(stderr, "UdpReceiver: SO_RCVBUF request of %d bytes failed: %s\n",
                     iRequestedRcvBufBytes, std::strerror(errno));
    }

    socklen_t uOptLen = sizeof(miActualRcvBufBytes);
    if (::getsockopt(miSocket, SOL_SOCKET, SO_RCVBUF, &miActualRcvBufBytes, &uOptLen) < 0) {
        std::fprintf(stderr, "UdpReceiver: reading back SO_RCVBUF failed: %s\n",
                     std::strerror(errno));
        miActualRcvBufBytes = 0;
    }

    sockaddr_in oAddress;
    std::memset(&oAddress, 0, sizeof(oAddress));
    oAddress.sin_family      = AF_INET;
    oAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    oAddress.sin_port        = htons(uPort);

    if (::bind(miSocket, reinterpret_cast<sockaddr*>(&oAddress), sizeof(oAddress)) < 0) {
        std::fprintf(stderr, "UdpReceiver: bind to port %u failed: %s\n",
                     static_cast<unsigned>(uPort), std::strerror(errno));
        close();
        return false;
    }

    const int iFlags = ::fcntl(miSocket, F_GETFL, 0);
    if (bBlocking) {
        if (iFlags < 0) {
            std::fprintf(stderr, "UdpReceiver: reading socket flags failed: %s\n",
                         std::strerror(errno));
            close();
            return false;
        }
        // Wake once a second with WouldBlock even when nothing arrives, so
        // a thread blocked here can notice a stop flag.
        timeval oTimeout;
        oTimeout.tv_sec  = 1;
        oTimeout.tv_usec = 0;
        if (::setsockopt(miSocket, SOL_SOCKET, SO_RCVTIMEO, &oTimeout, sizeof(oTimeout)) < 0)
            std::fprintf(stderr, "UdpReceiver: SO_RCVTIMEO failed: %s\n", std::strerror(errno));
        return true;
    }
    if (iFlags < 0 || ::fcntl(miSocket, F_SETFL, iFlags | O_NONBLOCK) < 0) {
        std::fprintf(stderr, "UdpReceiver: setting O_NONBLOCK failed: %s\n",
                     std::strerror(errno));
        close();
        return false;
    }

    return true;
}

bool UdpReceiver::joinMulticastGroup(const char* pGroup, const char* pInterfaceIp)
{
    CHECK(miSocket >= 0, "joinMulticastGroup before open");
    CHECK(pGroup != nullptr && pGroup[0] != '\0', "joinMulticastGroup with an empty group");

    ip_mreq oReq;
    std::memset(&oReq, 0, sizeof(oReq));
    if (::inet_pton(AF_INET, pGroup, &oReq.imr_multiaddr) != 1) {
        std::fprintf(stderr, "UdpReceiver: multicast group %s is not dotted IPv4\n", pGroup);
        return false;
    }
    if ((ntohl(oReq.imr_multiaddr.s_addr) >> 28) != 0xEu) {
        std::fprintf(stderr, "UdpReceiver: %s is not a multicast address\n", pGroup);
        return false;
    }
    // INADDR_ANY: the kernel joins on the interface its routing table picks
    // for the group, which on a single-homed board is the only one. A
    // laptop with Wi-Fi and a wired port to the switch must say which.
    oReq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (pInterfaceIp != nullptr && pInterfaceIp[0] != '\0' &&
        ::inet_pton(AF_INET, pInterfaceIp, &oReq.imr_interface) != 1) {
        std::fprintf(stderr, "UdpReceiver: interface address %s is not dotted IPv4\n",
                     pInterfaceIp);
        return false;
    }

    if (::setsockopt(miSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &oReq, sizeof(oReq)) < 0) {
        std::fprintf(stderr, "UdpReceiver: joining %s failed: %s\n", pGroup, std::strerror(errno));
        return false;
    }
    return true;
}

void UdpReceiver::close()
{
    if (miSocket >= 0) {
        ::close(miSocket);
        miSocket = -1;
    }
    miActualRcvBufBytes = 0;
}

ReceiveResult UdpReceiver::receive(uint8_t* pBuffer, size_t uCapacity, size_t& uBytesOut,
                                   std::string* pSenderOut)
{
    uBytesOut = 0;

    if (miSocket < 0 || pBuffer == nullptr || uCapacity == 0) {
        std::fprintf(stderr, "UdpReceiver::receive called with an invalid socket or buffer\n");
        return ReceiveResult::Error;
    }

    sockaddr_in oSender;
    socklen_t   uSenderLen = sizeof(oSender);
    const ssize_t iRead = ::recvfrom(miSocket, pBuffer, uCapacity, 0,
                                     pSenderOut ? reinterpret_cast<sockaddr*>(&oSender) : nullptr,
                                     pSenderOut ? &uSenderLen : nullptr);
    if (iRead >= 0 && pSenderOut != nullptr) {
        char achAddr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &oSender.sin_addr, achAddr, sizeof(achAddr));
        *pSenderOut = achAddr;
    }

    if (iRead < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return ReceiveResult::WouldBlock;
        if (errno == EINTR)
            return ReceiveResult::WouldBlock;   // retry on the next pass

        std::fprintf(stderr, "UdpReceiver: recvfrom failed: %s\n", std::strerror(errno));
        return ReceiveResult::Error;
    }

    uBytesOut = static_cast<size_t>(iRead);
    return ReceiveResult::Datagram;
}

} // namespace pbr
