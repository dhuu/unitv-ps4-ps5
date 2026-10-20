#ifndef WILLPRO_PS5 // PS4: sceHttp (a versão do PS5 está em http_curl.cpp)
#include "http.h"
#include "platform.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>

// Mesmo User-Agent usado no app Android (NetworkManager.kt)
#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

namespace http {

static int g_netPool = -1, g_ssl = -1, g_http = -1;

// O header do OpenOrbis declara esta função sem parâmetros; chamamos com a assinatura real.
typedef int32_t (*SetTimeoutFn)(int32_t, uint32_t);

// Ignora validação de certificado, como o OkHttp "unsafe" do app Android.
static int skipSslCallback(int libsslId, unsigned int verifyErr, void* const sslCert[], int certNum, void* userArg)
{
    (void)libsslId; (void)verifyErr; (void)sslCert; (void)certNum; (void)userArg;
    return 1;
}

bool init()
{
    sceNetInit();
    g_netPool = sceNetPoolCreate("willpro_net", 64 * 1024, 0);
    if (g_netPool < 0) { platform::log("sceNetPoolCreate 0x%08X", g_netPool); return false; }
    g_ssl = sceSslInit(1024 * 1024);
    if (g_ssl < 0) { platform::log("sceSslInit 0x%08X", g_ssl); return false; }
    g_http = sceHttpInit(g_netPool, g_ssl, 2 * 1024 * 1024);
    if (g_http < 0) { platform::log("sceHttpInit 0x%08X", g_http); return false; }
    return true;
}

void contexts(int& httpCtx, int& sslCtx)
{
    httpCtx = g_http;
    sslCtx = g_ssl;
}

Response get(const std::string& urlIn, size_t maxBytes, int timeoutSec)
{
    std::string url = urlIn.substr(0, urlIn.find('#')); // fragmento não vai na requisição
    Response res;
    int tpl = -1, conn = -1, req = -1, ret;
    int32_t status = 0;
    uint32_t usec = (uint32_t)timeoutSec * 1000000u;

    if (g_http < 0) { res.error = "HTTP não inicializado"; return res; }

    tpl = sceHttpCreateTemplate(g_http, UA, ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { res.error = "template"; return res; }
    sceHttpsSetSslCallback(tpl, skipSslCallback, NULL);
    // Desativa verificação estrita de servidor/CA (flags 0x01|0x02 -> SERVER_VERIFY | CLIENT)
    sceHttpsDisableOption(tpl, 0x01);
    sceHttpSetConnectTimeOut(tpl, usec);
    sceHttpSetResolveTimeOut(tpl, usec);
    ((SetTimeoutFn)(void*)&sceHttpSetRecvTimeOut)(tpl, usec);

    conn = sceHttpCreateConnectionWithURL(tpl, url.c_str(), true);
    if (conn < 0) { res.error = "conexão"; goto done; }

    req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (req < 0) { res.error = "requisição"; goto done; }

    sceHttpAddRequestHeader(req, "Accept", "application/json, text/plain, */*", 0);
    sceHttpAddRequestHeader(req, "Accept-Language", "pt-BR,pt;q=0.9,en-US;q=0.8,en;q=0.7", 0);
    sceHttpAddRequestHeader(req, "Cache-Control", "no-cache", 0);

    ret = sceHttpSendRequest(req, NULL, 0);
    if (ret < 0) {
        char e[64];
        snprintf(e, sizeof(e), "falha de rede (0x%08X)", ret);
        res.error = e;
        goto done;
    }

    sceHttpGetStatusCode(req, &status);
    res.status = status;

    {
        int lenType = 0;
        uint64_t contentLength = 0;
        if (sceHttpGetResponseContentLength(req, &lenType, &contentLength) >= 0 &&
            lenType == ORBIS_HTTP_CONTENTLEN_EXIST && contentLength > 0 && contentLength < (512ULL << 20))
            res.body.reserve((size_t)contentLength);

        std::vector<char> buf(128 * 1024);
        for (;;) {
            int n = sceHttpReadData(req, buf.data(), (uint32_t)buf.size());
            if (n < 0) { res.error = "leitura"; break; }
            if (n == 0) break;
            res.body.append(buf.data(), (size_t)n);
            if (maxBytes && res.body.size() > maxBytes) { res.error = "resposta muito grande"; break; }
        }
    }

done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return res;
}

} // namespace http

#endif // WILLPRO_PS5
