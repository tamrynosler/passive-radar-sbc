// Unit tests for pbr::HttpServer and httpRequest(): a server in a thread and
// the client talking to it over loopback. Plain main() + asserts.
//
// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out.
#undef NDEBUG
#include <cassert>

#include "pbr/HttpServer.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace pbr;

namespace {

const uint16_t kPort = 48080;

struct Context {
    std::string sLastMethod;
    std::string sLastPath;
    std::string sLastBody;
    int         iCalls = 0;
};

bool handler(const std::string& sMethod, const std::string& sPath,
             const std::string& sBody, void* pContext, HttpResponse& oResponse)
{
    Context* pCtx = static_cast<Context*>(pContext);
    pCtx->sLastMethod = sMethod;
    pCtx->sLastPath   = sPath;
    pCtx->sLastBody   = sBody;
    ++pCtx->iCalls;

    if (sPath == "/hello") {
        const char* pText = "hello\n";
        oResponse.vBody.assign(pText, pText + std::strlen(pText));
        oResponse.sExtraHeader = "X-Test: 7";
        return true;
    }
    if (sPath == "/echo" && sMethod == "POST") {
        oResponse.vBody.assign(sBody.begin(), sBody.end());
        return true;
    }
    if (sPath == "/big") {
        oResponse.vBody.assign(300000, 'z');
        oResponse.sContentType = "application/octet-stream";
        return true;
    }
    return false;
}

void serveN(HttpServer* pServer, Context* pCtx, int iCount)
{
    for (int i = 0; i < iCount; ++i)
        pServer->serveOne(handler, pCtx);
}

} // namespace

int main()
{
    HttpServer oServer;
    assert(oServer.open(kPort));

    Context oCtx;
    std::thread oThread(serveN, &oServer, &oCtx, 5);

    int iStatus = 0;
    std::string sBody;

    // GET
    assert(httpRequest("127.0.0.1", kPort, "GET", "/hello", "", 5.0, iStatus, sBody));
    assert(iStatus == 200);
    assert(sBody == "hello\n");

    // POST with a body; the query string is stripped from the path.
    const std::string sPosted = "CFAR_PFA = 1e-6\nDSI_ALGORITHM = ECA-B\n";
    assert(httpRequest("127.0.0.1", kPort, "POST", "/echo?x=1", sPosted, 5.0, iStatus, sBody));
    assert(iStatus == 200);
    assert(sBody == sPosted);
    assert(oCtx.sLastMethod == "POST" && oCtx.sLastPath == "/echo");
    assert(oCtx.sLastBody == sPosted);

    // A body larger than one read.
    assert(httpRequest("127.0.0.1", kPort, "GET", "/big", "", 5.0, iStatus, sBody));
    assert(iStatus == 200 && sBody.size() == 300000);

    // Not found, and a method the server does not take.
    assert(httpRequest("127.0.0.1", kPort, "GET", "/nope", "", 5.0, iStatus, sBody));
    assert(iStatus == 404);
    assert(httpRequest("127.0.0.1", kPort, "PUT", "/hello", "", 5.0, iStatus, sBody));
    assert(iStatus == 405);

    oThread.join();
    assert(oCtx.iCalls == 4);   // PUT is refused before the handler

    // Nothing pending: servePending returns at once.
    bool bServed = true;
    assert(oServer.servePending(handler, &oCtx, bServed));
    assert(!bServed);

    // Nobody listening there: the client reports why, quickly.
    oServer.close();
    assert(!httpRequest("127.0.0.1", kPort, "GET", "/hello", "", 1.0, iStatus, sBody));
    assert(sBody.find("cannot connect") != std::string::npos);

    std::printf("test_http: all tests passed\n");
    return 0;
}
