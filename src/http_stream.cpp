// Download por faixas (HTTP Range) usado pelo player (FFmpeg) para ler vídeos remotos
#ifndef UNITV_PS5 // PS4: sceHttp (a versão do PS5 está em http_curl.cpp)
#include "http.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <orbis/Http.h>

#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

namespace http {

typedef int32_t (*SetTimeoutFn)(int32_t, uint32_t);

static int skipSsl(int, unsigned int, void* const[], int, void*) { return 1; }

static std::string g_lastErr;
std::string lastStreamError() { return g_lastErr; }

static uint64_t parseTotal(const char* hdr, size_t len, uint64_t from, int status)
{
    std::string h(hdr ? hdr : "", hdr ? len : 0);
    std::string low = h;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    size_t p = low.find("content-range:");
    if (p != std::string::npos) {
        size_t slash = low.find('/', p);
        size_t eol = low.find('\n', p);
        if (slash != std::string::npos && (eol == std::string::npos || slash < eol)) {
            uint64_t t = strtoull(h.c_str() + slash + 1, nullptr, 10);
            if (t) return t;
        }
    }
    p = low.find("content-length:");
    if (p != std::string::npos) {
        uint64_t l = strtoull(h.c_str() + p + 15, nullptr, 10);
        if (l) return status == 206 ? from + l : l;
    }
    return 0;
}

// Abre uma requisição; useRange=false manda GET simples (alguns painéis respondem 400 ao Range)
static int openRequest(Stream& st, const std::string& urlIn, uint64_t from, bool useRange)
{
    // o fragmento (#...) não faz parte da requisição HTTP
    std::string url = urlIn.substr(0, urlIn.find('#'));
    streamClose(st);
    int httpCtx, sslCtx;
    contexts(httpCtx, sslCtx);
    uint32_t usec = 20u * 1000000u;
    st.tpl = sceHttpCreateTemplate(httpCtx, UA, ORBIS_HTTP_VERSION_1_1, 1);
    if (st.tpl < 0) { g_lastErr = "template"; return -1; }
    sceHttpsSetSslCallback(st.tpl, skipSsl, NULL);
    sceHttpsDisableOption(st.tpl, 0x01);
    sceHttpSetConnectTimeOut(st.tpl, usec);
    sceHttpSetResolveTimeOut(st.tpl, usec);
    ((SetTimeoutFn)(void*)&sceHttpSetRecvTimeOut)(st.tpl, usec);
    st.conn = sceHttpCreateConnectionWithURL(st.tpl, url.c_str(), true);
    if (st.conn < 0) { g_lastErr = "conexão"; streamClose(st); return -1; }
    st.req = sceHttpCreateRequestWithURL(st.conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (st.req < 0) { g_lastErr = "requisição"; streamClose(st); return -1; }
    if (useRange) {
        char range[64];
        snprintf(range, sizeof(range), "bytes=%llu-", (unsigned long long)from);
        sceHttpAddRequestHeader(st.req, "Range", range, 0);
    }
    sceHttpAddRequestHeader(st.req, "Accept", "*/*", 0);
    sceHttpAddRequestHeader(st.req, "Connection", "keep-alive", 0);
    int ret = sceHttpSendRequest(st.req, NULL, 0);
    if (ret < 0) {
        char e[64];
        snprintf(e, sizeof(e), "falha de rede 0x%08X", (unsigned)ret);
        g_lastErr = e;
        platform::log("stream send 0x%08X", ret);
        streamClose(st);
        return -1;
    }
    int32_t status = 0;
    sceHttpGetStatusCode(st.req, &status);
    return status;
}

bool streamOpen(Stream& st, const std::string& url, uint64_t from)
{
    // do início: GET simples; com deslocamento: tenta Range e, se recusado, GET simples + descarte
    bool useRange = from > 0;
    int status = openRequest(st, url, from, useRange);
    if (useRange && (status == 400 || status == 416 || status == 403)) {
        platform::log("Range recusado (HTTP %d), tentando sem Range", status);
        status = openRequest(st, url, from, false);
    }
    if (status < 0) return false;
    if (status != 200 && status != 206) {
        g_lastErr = "HTTP " + std::to_string(status);
        platform::log("stream HTTP %d", status);
        streamClose(st);
        return false;
    }
    char* hdr = nullptr;
    size_t hlen = 0;
    st.total = 0;
    st.rangeOk = status == 206;
    if (sceHttpGetAllResponseHeaders(st.req, &hdr, &hlen) >= 0 && hdr) {
        st.total = parseTotal(hdr, hlen, from, status);
        std::string low(hdr, hlen);
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.find("accept-ranges: bytes") != std::string::npos) st.rangeOk = true;
    }
    st.open = true;
    st.pos = 0;
    if (status == 206 || from == 0) {
        st.pos = from;
    } else {
        // servidor ignorou o Range: descarta bytes até chegar em "from"
        std::vector<char> skip(128 * 1024);
        while (st.pos < from) {
            uint32_t want = (uint32_t)std::min<uint64_t>(skip.size(), from - st.pos);
            int n = sceHttpReadData(st.req, skip.data(), want);
            if (n <= 0) { streamClose(st); return false; }
            st.pos += (uint64_t)n;
        }
    }
    return true;
}

void streamAbort(Stream& st)
{
    int req = st.req;
    if (req >= 0) sceHttpAbortRequest(req);
}

int streamRead(Stream& st, void* buf, uint32_t len)
{
    if (!st.open) return -1;
    int n = sceHttpReadData(st.req, buf, len);
    if (n > 0) st.pos += (uint64_t)n;
    return n;
}

void streamClose(Stream& st)
{
    if (st.req >= 0) sceHttpDeleteRequest(st.req);
    if (st.conn >= 0) sceHttpDeleteConnection(st.conn);
    if (st.tpl >= 0) sceHttpDeleteTemplate(st.tpl);
    st.req = st.conn = st.tpl = -1;
    st.open = false;
}

typedef int32_t (*SetAutoRedirectFn)(int32_t, int32_t);

std::string resolveRedirects(const std::string& urlIn)
{
    std::string url = urlIn;
    int httpCtx, sslCtx;
    contexts(httpCtx, sslCtx);
    for (int hop = 0; hop < 6; hop++) {
        int tpl = sceHttpCreateTemplate(httpCtx, UA, ORBIS_HTTP_VERSION_1_1, 1);
        if (tpl < 0) break;
        sceHttpsSetSslCallback(tpl, skipSsl, NULL);
        sceHttpsDisableOption(tpl, 0x01);
        ((SetAutoRedirectFn)(void*)&sceHttpSetAutoRedirect)(tpl, 0);
        sceHttpSetConnectTimeOut(tpl, 15u * 1000000u);
        ((SetTimeoutFn)(void*)&sceHttpSetRecvTimeOut)(tpl, 15u * 1000000u);
        int conn = sceHttpCreateConnectionWithURL(tpl, url.c_str(), true);
        int req = conn >= 0 ? sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url.c_str(), 0) : -1;
        std::string next;
        if (req >= 0) {
            sceHttpAddRequestHeader(req, "Range", "bytes=0-0", 0);
            if (sceHttpSendRequest(req, NULL, 0) >= 0) {
                int32_t status = 0;
                sceHttpGetStatusCode(req, &status);
                if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                    char* hdr = nullptr;
                    size_t hlen = 0;
                    if (sceHttpGetAllResponseHeaders(req, &hdr, &hlen) >= 0 && hdr) {
                        std::string h(hdr, hlen), low = h;
                        for (auto& c : low) c = (char)tolower((unsigned char)c);
                        size_t p = low.find("\nlocation:");
                        if (p == std::string::npos && low.compare(0, 9, "location:") == 0) p = 0;
                        else if (p != std::string::npos) p += 1;
                        if (p != std::string::npos) {
                            size_t st = p + 9, en = h.find_first_of("\r\n", st);
                            next = h.substr(st, en == std::string::npos ? std::string::npos : en - st);
                            while (!next.empty() && next[0] == ' ') next.erase(0, 1);
                        }
                    }
                }
            }
        }
        if (req >= 0) sceHttpDeleteRequest(req);
        if (conn >= 0) sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        if (next.empty()) break;
        if (next[0] == '/') { // relativo ao host
            size_t s3 = url.find("://");
            size_t slash = s3 == std::string::npos ? std::string::npos : url.find('/', s3 + 3);
            next = (slash == std::string::npos ? url : url.substr(0, slash)) + next;
        }
        platform::log("Redirect -> %s", next.c_str());
        url = next;
    }
    return url;
}

} // namespace http

#endif // UNITV_PS5
