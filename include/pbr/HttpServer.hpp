#ifndef PBR_HTTP_SERVER_HPP
#define PBR_HTTP_SERVER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

// A minimal HTTP/1.1 server for ard_view and for pbr_run's control port:
// GET and POST, one request per connection, handled inline, POSIX sockets
// and nothing else. It serves a handful of paths on the lab network; it is
// not a web server.
//
// A stalled client cannot hang it: the accepted socket carries 5 s send and
// receive timeouts, and a failed client is dropped and the loop goes on.

struct HttpResponse {
    int                  iStatus = 200;               // 200, 400, 404, 405, 500
    std::string          sContentType = "text/plain; charset=utf-8";
    std::string          sExtraHeader;                // one "Name: value" line, or empty
    std::vector<uint8_t> vBody;
};

// Fill oResponse for a request. sMethod is "GET" or "POST"; sBody is the
// POST body, empty for GET. Return false for "not found".
typedef bool (*HttpHandler)(const std::string& sMethod, const std::string& sPath,
                            const std::string& sBody, void* pContext,
                            HttpResponse& oResponse);

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Listen on uPort, all interfaces.
    bool open(uint16_t uPort);
    void close();
    bool isOpen() const { return miListen >= 0; }

    // Accept one connection, answer it, close it. Blocks until a client
    // connects. Returns false only when the listening socket itself has
    // failed; a bad client returns true.
    bool serveOne(HttpHandler pHandler, void* pContext);

    // The same, but returns at once with bServedOut = false when no client
    // is waiting. For a loop that has other work (pbr_run's).
    bool servePending(HttpHandler pHandler, void* pContext, bool& bServedOut);

private:
    int miListen;
};

// A one-shot HTTP client for the same profile: connect, send one request,
// read the whole response, close. iStatusOut is the response status and
// sBodyOut its body. Returns false, with sBodyOut holding the reason, if the
// server could not be reached or answered within dTimeoutSeconds.
bool httpRequest(const char* pHost, uint16_t uPort, const char* pMethod,
                 const char* pPath, const std::string& sBody,
                 double dTimeoutSeconds, int& iStatusOut, std::string& sBodyOut);

} // namespace pbr

#endif // PBR_HTTP_SERVER_HPP
