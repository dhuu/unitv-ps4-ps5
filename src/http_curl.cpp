// HTTP do PS5 via libcurl (ps5-payload-sdk). Mesma interface de http.cpp + http_stream.cpp do PS4.
#ifdef WILLPRO_PS5
#include "http.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <curl/curl.h>

// Mesmo User-Agent usado no app Android (NetworkManager.kt)
#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

namespace http {

static bool g_ready = false;
static std::string g_lastErr;
std::string lastStreamError() { return g_lastErr; }

bool init()
{
    CURLcode r = curl_global_init(CURL_GLOBAL_ALL);
    if (r != CURLE_OK) {
        platform::log("curl_global_init: %s", curl_easy_strerror(r));
        return false;
    }
    g_ready = true;
    platform::log("%s", curl_version());
    return true;
}

void contexts(int& httpCtx, int& sslCtx)
{
    httpCtx = -1;
    sslCtx = -1;
}

// Opções comuns: ignora validação de certificado, como o OkHttp "unsafe" do app Android
static void setCommon(CURL* c, const std::string& url, long connectSec)
{
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, connectSec);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
}

struct GetCtx {
    std::string* body;
    size_t maxBytes;
    bool tooBig;
};

static size_t getWrite(char* p, size_t sz, size_t n, void* ud)
{
    GetCtx* g = (GetCtx*)ud;
    size_t len = sz * n;
    g->body->append(p, len);
    if (g->maxBytes && g->body->size() > g->maxBytes) {
        g->tooBig = true;
        return 0; // interrompe
    }
    return len;
}

Response get(const std::string& urlIn, size_t maxBytes, int timeoutSec)
{
    std::string url = urlIn.substr(0, urlIn.find('#')); // fragmento não vai na requisição
    Response res;
    if (!g_ready) { res.error = "HTTP não inicializado"; return res; }
    CURL* c = curl_easy_init();
    if (!c) { res.error = "curl"; return res; }

    setCommon(c, url, timeoutSec);
    // sem dados por timeoutSec: desiste (equivale ao timeout de leitura do PS4)
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)timeoutSec);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // gzip/deflate, como o OkHttp
    curl_slist* hl = nullptr;
    hl = curl_slist_append(hl, "Accept: application/json, text/plain, */*");
    hl = curl_slist_append(hl, "Accept-Language: pt-BR,pt;q=0.9,en-US;q=0.8,en;q=0.7");
    hl = curl_slist_append(hl, "Cache-Control: no-cache");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    GetCtx g{&res.body, maxBytes, false};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, getWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &g);

    CURLcode r = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    res.status = (int)status;
    if (g.tooBig) res.error = "resposta muito grande";
    else if (r != CURLE_OK) {
        res.error = std::string("falha de rede (") + curl_easy_strerror(r) + ")";
        if (res.body.empty()) res.status = 0;
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return res;
}

static size_t discardWrite(char*, size_t sz, size_t n, void*) { return sz * n; }

std::string resolveRedirects(const std::string& urlIn)
{
    std::string url = urlIn.substr(0, urlIn.find('#'));
    CURL* c = curl_easy_init();
    if (!c) return urlIn;
    setCommon(c, url, 15);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discardWrite);
    std::string out = urlIn;
    if (curl_easy_perform(c) == CURLE_OK) {
        char* eff = nullptr;
        if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff && url != eff) {
            out = eff;
            platform::log("Redirect -> %s", eff);
        }
    }
    curl_easy_cleanup(c);
    return out;
}

// ---------------------------------------------------------------------------
// Download sequencial (o player lê no ritmo dele: o curl pausa quando o buffer enche)
// ---------------------------------------------------------------------------
struct CurlStream {
    CURLM* multi = nullptr;
    CURL* easy = nullptr;
    curl_slist* hl = nullptr;
    std::vector<uint8_t> buf;
    size_t off = 0;         // primeiro byte ainda não entregue em buf
    bool paused = false;
    bool done = false;      // transferência terminou (ver result)
    CURLcode result = CURLE_OK;
    bool headersDone = false;
    std::string headers;    // cabeçalhos da última resposta (após redirecionamentos)
};

static const size_t kMaxBuffered = 2u << 20;

static size_t streamWrite(char* p, size_t sz, size_t n, void* ud)
{
    CurlStream* cs = (CurlStream*)ud;
    size_t len = sz * n;
    if (cs->buf.size() - cs->off >= kMaxBuffered) {
        cs->paused = true;
        return CURL_WRITEFUNC_PAUSE; // o curl entrega de novo os mesmos dados ao retomar
    }
    if (cs->off > 0 && cs->off >= cs->buf.size() / 2) {
        cs->buf.erase(cs->buf.begin(), cs->buf.begin() + (long)cs->off);
        cs->off = 0;
    }
    cs->buf.insert(cs->buf.end(), (const uint8_t*)p, (const uint8_t*)p + len);
    return len;
}

static size_t streamHeader(char* p, size_t sz, size_t n, void* ud)
{
    CurlStream* cs = (CurlStream*)ud;
    size_t len = sz * n;
    std::string line(p, len);
    if (line.compare(0, 5, "HTTP/") == 0) cs->headers.clear(); // nova resposta (redirecionamento)
    cs->headers += line;
    if (line == "\r\n" || line == "\n") {
        long code = 0;
        curl_easy_getinfo(cs->easy, CURLINFO_RESPONSE_CODE, &code);
        if (code < 300 || code >= 400) cs->headersDone = true; // 3xx: o curl segue o Location
    }
    return len;
}

// Avança a transferência por até waitMs; retorna false se terminou
static bool pump(CurlStream* cs, int waitMs)
{
    if (cs->done) return false;
    if (cs->paused && cs->buf.size() - cs->off < kMaxBuffered) {
        cs->paused = false;
        curl_easy_pause(cs->easy, CURLPAUSE_CONT);
    }
    int running = 0;
    curl_multi_perform(cs->multi, &running);
    int msgs = 0;
    while (CURLMsg* m = curl_multi_info_read(cs->multi, &msgs)) {
        if (m->msg == CURLMSG_DONE) {
            cs->done = true;
            cs->result = m->data.result;
        }
    }
    if (cs->done) return false;
    if (!cs->paused) curl_multi_poll(cs->multi, nullptr, 0, waitMs, nullptr);
    return true;
}

static void freeStream(Stream& st)
{
    CurlStream* cs = (CurlStream*)st.impl;
    st.impl = nullptr;
    st.open = false;
    if (!cs) return;
    if (cs->multi && cs->easy) curl_multi_remove_handle(cs->multi, cs->easy);
    if (cs->easy) curl_easy_cleanup(cs->easy);
    if (cs->multi) curl_multi_cleanup(cs->multi);
    curl_slist_free_all(cs->hl);
    delete cs;
}

static uint64_t parseTotal(const std::string& h, uint64_t from, int status)
{
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

// Abre uma requisição e espera os cabeçalhos; useRange=false manda GET simples
static int openRequest(Stream& st, const std::string& urlIn, uint64_t from, bool useRange)
{
    std::string url = urlIn.substr(0, urlIn.find('#'));
    freeStream(st);
    CurlStream* cs = new CurlStream();
    st.impl = cs;
    cs->easy = curl_easy_init();
    cs->multi = curl_multi_init();
    if (!cs->easy || !cs->multi) { g_lastErr = "curl"; freeStream(st); return -1; }
    setCommon(cs->easy, url, 20);
    if (useRange) {
        char range[64];
        snprintf(range, sizeof(range), "%llu-", (unsigned long long)from);
        curl_easy_setopt(cs->easy, CURLOPT_RANGE, range);
    }
    cs->hl = curl_slist_append(cs->hl, "Accept: */*");
    curl_easy_setopt(cs->easy, CURLOPT_HTTPHEADER, cs->hl);
    curl_easy_setopt(cs->easy, CURLOPT_WRITEFUNCTION, streamWrite);
    curl_easy_setopt(cs->easy, CURLOPT_WRITEDATA, cs);
    curl_easy_setopt(cs->easy, CURLOPT_HEADERFUNCTION, streamHeader);
    curl_easy_setopt(cs->easy, CURLOPT_HEADERDATA, cs);
    curl_multi_add_handle(cs->multi, cs->easy);

    uint64_t deadline = platform::ticksMs() + 25000;
    while (!cs->headersDone) {
        if (st.aborted) { g_lastErr = "interrompido"; freeStream(st); return -1; }
        if (!pump(cs, 100)) break;
        if (platform::ticksMs() > deadline) { g_lastErr = "tempo esgotado"; freeStream(st); return -1; }
    }
    if (!cs->headersDone) {
        g_lastErr = std::string("falha de rede (") + curl_easy_strerror(cs->result) + ")";
        platform::log("stream: %s", g_lastErr.c_str());
        freeStream(st);
        return -1;
    }
    long status = 0;
    curl_easy_getinfo(cs->easy, CURLINFO_RESPONSE_CODE, &status);
    return (int)status;
}

bool streamOpen(Stream& st, const std::string& url, uint64_t from)
{
    st.aborted = false;
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
        freeStream(st);
        return false;
    }
    CurlStream* cs = (CurlStream*)st.impl;
    st.rangeOk = status == 206;
    st.total = parseTotal(cs->headers, from, status);
    {
        std::string low = cs->headers;
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
            int n = streamRead(st, skip.data(), want);
            if (n <= 0) { streamClose(st); return false; }
        }
    }
    return true;
}

void streamAbort(Stream& st)
{
    // só marca: a thread que lê confere a cada 100 ms e fecha a conexão ela mesma
    st.aborted = true;
}

int streamRead(Stream& st, void* buf, uint32_t len)
{
    CurlStream* cs = (CurlStream*)st.impl;
    if (!st.open || !cs) return -1;
    uint64_t lastData = platform::ticksMs();
    while (cs->buf.size() == cs->off) {
        if (st.aborted) return -1;
        if (cs->done) {
            if (cs->result != CURLE_OK) {
                g_lastErr = std::string("falha de rede (") + curl_easy_strerror(cs->result) + ")";
                return -1;
            }
            return 0; // fim
        }
        size_t before = cs->buf.size();
        pump(cs, 100);
        if (cs->buf.size() != before) lastData = platform::ticksMs();
        else if (platform::ticksMs() - lastData > 20000) { // 20 s sem dados (timeout de leitura do PS4)
            g_lastErr = "tempo esgotado";
            return -1;
        }
    }
    size_t n = std::min<size_t>(len, cs->buf.size() - cs->off);
    memcpy(buf, cs->buf.data() + cs->off, n);
    cs->off += n;
    st.pos += (uint64_t)n;
    return (int)n;
}

void streamClose(Stream& st)
{
    freeStream(st);
}

} // namespace http

#endif // WILLPRO_PS5
