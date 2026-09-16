#include "pbr/UdpReceiver.hpp"

#include "pbr/check.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

bool UdpReceiver::open(uint16_t uPort, int iRequestedRcvBufBytes)
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
    if (iFlags < 0 || ::fcntl(miSocket, F_SETFL, iFlags | O_NONBLOCK) < 0) {
        std::fprintf(stderr, "UdpReceiver: setting O_NONBLOCK failed: %s\n",
                     std::strerror(errno));
        close();
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

ReceiveResult UdpReceiver::receive(uint8_t* pBuffer, size_t uCapacity, size_t& uBytesOut)
{
    uBytesOut = 0;

    if (miSocket < 0 || pBuffer == nullptr || uCapacity == 0) {
        std::fprintf(stderr, "UdpReceiver::receive called with an invalid socket or buffer\n");
        return ReceiveResult::Error;
    }

    const ssize_t iRead = ::recvfrom(miSocket, pBuffer, uCapacity, 0, nullptr, nullptr);

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
