#include "pbr/HttpServer.hpp"

#include "pbr/check.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace pbr {

namespace {

const size_t kMaxHeaderBytes = 8192;
const size_t kMaxRequestBodyBytes  = 65536;             // a whole config is under 2 KB
const size_t kMaxResponseBodyBytes = 16 * 1024 * 1024;  // what the client will take back
const int    kClientTimeoutSeconds = 5;

const char* reasonPhrase(int iStatus)
{
    switch (iStatus) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default:  return "Internal Server Error";
    }
}

void setTimeouts(int iSocket, double dSeconds)
{
    timeval oTimeout;
    oTimeout.tv_sec  = static_cast<long>(dSeconds);
    oTimeout.tv_usec = static_cast<long>((dSeconds - static_cast<double>(oTimeout.tv_sec)) * 1e6);
    ::setsockopt(iSocket, SOL_SOCKET, SO_RCVTIMEO, &oTimeout, sizeof(oTimeout));
    ::setsockopt(iSocket, SOL_SOCKET, SO_SNDTIMEO, &oTimeout, sizeof(oTimeout));
}

// Write all of a buffer, or give up on error or timeout.
bool sendAll(int iSocket, const uint8_t* pBytes, size_t uLength)
{
    size_t uSent = 0;
    while (uSent < uLength) {
        const ssize_t iN = ::send(iSocket, pBytes + uSent, uLength - uSent, 0);
        if (iN <= 0) {
            if (iN < 0 && errno == EINTR)
                continue;
            return false;
        }
        uSent += static_cast<size_t>(iN);
    }
    return true;
}

bool sendAll(int iSocket, const std::string& s)
{
    return sendAll(iSocket, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Read from a socket until the header terminator has been seen, then keep
// reading until uContentLength body bytes follow it. Returns false if the
// peer stopped or the limits were exceeded before that.
bool readHttpMessage(int iSocket, size_t uMaxBodyBytes, std::string& sHeadOut, std::string& sBodyOut)
{
    std::string sAll;
    size_t uHeadEnd = std::string::npos;
    char achChunk[4096];

    while (uHeadEnd == std::string::npos) {
        if (sAll.size() > kMaxHeaderBytes)
            return false;
        const ssize_t iN = ::recv(iSocket, achChunk, sizeof(achChunk), 0);
        if (iN <= 0)
            return false;
        sAll.append(achChunk, static_cast<size_t>(iN));
        uHeadEnd = sAll.find("\r\n\r\n");
    }

    sHeadOut = sAll.substr(0, uHeadEnd);
    sBodyOut = sAll.substr(uHeadEnd + 4);

    // Content-Length, case-insensitive on the name as the standard requires.
    size_t uContentLength = 0;
    std::string sLower = sHeadOut;
    for (size_t u = 0; u < sLower.size(); ++u)
        sLower[u] = static_cast<char>(std::tolower(static_cast<unsigned char>(sLower[u])));
    const size_t uAt = sLower.find("content-length:");
    if (uAt != std::string::npos)
        uContentLength = static_cast<size_t>(std::strtoul(sLower.c_str() + uAt + 15, nullptr, 10));
    if (uContentLength > uMaxBodyBytes)
        return false;

    while (sBodyOut.size() < uContentLength) {
        const ssize_t iN = ::recv(iSocket, achChunk, sizeof(achChunk), 0);
        if (iN <= 0)
            return false;
        sBodyOut.append(achChunk, static_cast<size_t>(iN));
    }
    sBodyOut.resize(uContentLength);
    return true;
}

void sendResponse(int iSocket, const HttpResponse& oResponse)
{
    char achHead[512];
    const int iHead = std::snprintf(
        achHead, sizeof(achHead),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "%s%s"
        "\r\n",
        oResponse.iStatus, reasonPhrase(oResponse.iStatus),
        oResponse.sContentType.c_str(), oResponse.vBody.size(),
        oResponse.sExtraHeader.c_str(),
        oResponse.sExtraHeader.empty() ? "" : "\r\n");
    if (iHead <= 0 || static_cast<size_t>(iHead) >= sizeof(achHead))
        return;

    if (sendAll(iSocket, reinterpret_cast<const uint8_t*>(achHead), static_cast<size_t>(iHead)) &&
        !oResponse.vBody.empty())
        sendAll(iSocket, oResponse.vBody.data(), oResponse.vBody.size());
}

void setTextBody(HttpResponse& oResponse, const char* pText)
{
    oResponse.vBody.assign(reinterpret_cast<const uint8_t*>(pText),
                           reinterpret_cast<const uint8_t*>(pText) + std::strlen(pText));
}

} // namespace

HttpServer::HttpServer()
    : miListen(-1)
{
}

HttpServer::~HttpServer()
{
    close();
}

bool HttpServer::open(uint16_t uPort)
{
    CHECK(miListen < 0, "HttpServer::open on an open server");

    miListen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (miListen < 0) {
        std::fprintf(stderr, "HttpServer: socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    // Restart without waiting for the old port to time out.
    int iReuse = 1;
    ::setsockopt(miListen, SOL_SOCKET, SO_REUSEADDR, &iReuse, sizeof(iReuse));

    sockaddr_in oAddress;
    std::memset(&oAddress, 0, sizeof(oAddress));
    oAddress.sin_family      = AF_INET;
    oAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    oAddress.sin_port        = htons(uPort);

    if (::bind(miListen, reinterpret_cast<sockaddr*>(&oAddress), sizeof(oAddress)) < 0) {
        std::fprintf(stderr, "HttpServer: bind to port %u failed: %s\n"
                             "  another instance is probably still running (make stop)\n",
                     static_cast<unsigned>(uPort), std::strerror(errno));
        close();
        return false;
    }
    if (::listen(miListen, 8) < 0) {
        std::fprintf(stderr, "HttpServer: listen failed: %s\n", std::strerror(errno));
        close();
        return false;
    }
    return true;
}

void HttpServer::close()
{
    if (miListen >= 0) {
        ::close(miListen);
        miListen = -1;
    }
}

bool HttpServer::servePending(HttpHandler pHandler, void* pContext, bool& bServedOut)
{
    bServedOut = false;
    CHECK(miListen >= 0, "HttpServer::servePending on a closed server");

    pollfd oPoll;
    oPoll.fd      = miListen;
    oPoll.events  = POLLIN;
    oPoll.revents = 0;
    const int iReady = ::poll(&oPoll, 1, 0);
    if (iReady <= 0)
        return true;    // nothing waiting, or a transient poll error
    bServedOut = true;
    return serveOne(pHandler, pContext);
}

bool HttpServer::serveOne(HttpHandler pHandler, void* pContext)
{
    CHECK(miListen >= 0, "HttpServer::serveOne on a closed server");
    CHECK(pHandler != nullptr, "HttpServer::serveOne with a null handler");

    const int iClient = ::accept(miListen, nullptr, nullptr);
    if (iClient < 0) {
        if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN)
            return true;
        std::fprintf(stderr, "HttpServer: accept failed: %s\n", std::strerror(errno));
        return false;
    }
    setTimeouts(iClient, kClientTimeoutSeconds);

    std::string sHead, sBody;
    if (!readHttpMessage(iClient, kMaxRequestBodyBytes, sHead, sBody)) {
        ::close(iClient);
        return true;
    }

    // "GET /path HTTP/1.1": the method and the path up to the first space.
    HttpResponse oResponse;
    const size_t uSpace1 = sHead.find(' ');
    const size_t uSpace2 = (uSpace1 == std::string::npos) ? std::string::npos
                                                          : sHead.find(' ', uSpace1 + 1);
    if (uSpace1 == std::string::npos || uSpace2 == std::string::npos) {
        oResponse.iStatus = 400;
        setTextBody(oResponse, "bad request line\n");
        sendResponse(iClient, oResponse);
        ::close(iClient);
        return true;
    }
    const std::string sMethod = sHead.substr(0, uSpace1);
    std::string sPath = sHead.substr(uSpace1 + 1, uSpace2 - uSpace1 - 1);
    const size_t uQuery = sPath.find('?');
    if (uQuery != std::string::npos)
        sPath.erase(uQuery);

    if (sMethod != "GET" && sMethod != "POST") {
        oResponse.iStatus = 405;
        setTextBody(oResponse, "GET or POST only\n");
    } else if (!pHandler(sMethod, sPath, sBody, pContext, oResponse)) {
        oResponse = HttpResponse();
        oResponse.iStatus = 404;
        setTextBody(oResponse, "not found\n");
    }

    sendResponse(iClient, oResponse);
    ::close(iClient);
    return true;
}

bool httpRequest(const char* pHost, uint16_t uPort, const char* pMethod,
                 const char* pPath, const std::string& sBody,
                 double dTimeoutSeconds, int& iStatusOut, std::string& sBodyOut)
{
    iStatusOut = 0;
    sBodyOut.clear();

    addrinfo oHints;
    std::memset(&oHints, 0, sizeof(oHints));
    oHints.ai_family   = AF_INET;
    oHints.ai_socktype = SOCK_STREAM;
    addrinfo* pResult = nullptr;
    if (::getaddrinfo(pHost, nullptr, &oHints, &pResult) != 0 || pResult == nullptr) {
        sBodyOut = std::string("cannot resolve ") + pHost;
        return false;
    }
    sockaddr_in oAddress;
    std::memcpy(&oAddress, pResult->ai_addr, sizeof(oAddress));
    ::freeaddrinfo(pResult);
    oAddress.sin_port = htons(uPort);

    const int iSocket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (iSocket < 0) {
        sBodyOut = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    setTimeouts(iSocket, dTimeoutSeconds);

    if (::connect(iSocket, reinterpret_cast<sockaddr*>(&oAddress), sizeof(oAddress)) < 0) {
        char achAddr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &oAddress.sin_addr, achAddr, sizeof(achAddr));
        sBodyOut = std::string("cannot connect to ") + achAddr + ":" +
                   std::to_string(uPort) + ": " + std::strerror(errno);
        ::close(iSocket);
        return false;
    }

    char achHead[512];
    std::snprintf(achHead, sizeof(achHead),
                  "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %zu\r\n"
                  "Connection: close\r\n\r\n",
                  pMethod, pPath, pHost, sBody.size());
    if (!sendAll(iSocket, std::string(achHead)) || !sendAll(iSocket, sBody)) {
        sBodyOut = "send failed";
        ::close(iSocket);
        return false;
    }

    std::string sHead;
    if (!readHttpMessage(iSocket, kMaxResponseBodyBytes, sHead, sBodyOut)) {
        sBodyOut = "no complete response";
        ::close(iSocket);
        return false;
    }
    ::close(iSocket);

    // "HTTP/1.1 200 OK"
    const size_t uSpace = sHead.find(' ');
    if (uSpace == std::string::npos) {
        sBodyOut = "malformed status line";
        return false;
    }
    iStatusOut = std::atoi(sHead.c_str() + uSpace + 1);
    return true;
}

} // namespace pbr
