// PS5: enviado como payload (elfldr na porta 9021 ou autoload do HEN), o processo não recebe memória
// de vídeo e a tela não abre (sceKernelAllocateMainDirectMemory: EAGAIN). Aqui o .elf grava o app
// embutido (assets/app: eboot.elf + icon0.png) em BIGAPP_DIR, sobe o websrv do ps5-payload-dev (também
// embutido) e pede a ele (/hbldr) para abrir o eboot.elf como "big app", que tem memória de vídeo.
// Aberto assim, argv[0] é o caminho do eboot.elf.
#ifdef WILLPRO_PS5
#include "platform.h"

#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern "C" int sceUserServiceInitialize(void*);

struct EmbeddedAsset {
    const char* path;
    const unsigned char* data;
    unsigned int size;
};
extern const EmbeddedAsset g_embeddedAssets[];
extern const unsigned int g_embeddedAssetCount;

namespace platform {

static const EmbeddedAsset* findAsset(const char* path);

#define WEBSRV_PORT 8080
#define ELFLDR_PORT 9021

static int connectLocal(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool sendAll(int fd, const void* data, size_t len)
{
    const char* p = (const char*)data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool websrvRunning()
{
    int fd = connectLocal(WEBSRV_PORT);
    if (fd < 0) return false;
    close(fd);
    return true;
}

// Envia o websrv-ps5.elf embutido ao elfldr e espera a porta 8080 abrir
static bool startWebsrv()
{
    const EmbeddedAsset* elf = findAsset("websrv/websrv-ps5.elf");
    if (!elf) {
        log("bootstrap: websrv não está embutido neste build");
        return false;
    }
    int fd = connectLocal(ELFLDR_PORT);
    if (fd < 0) {
        log("bootstrap: elfldr não responde na porta %d", ELFLDR_PORT);
        return false;
    }
    bool ok = sendAll(fd, elf->data, elf->size);
    close(fd);
    if (!ok) {
        log("bootstrap: falha ao enviar o websrv ao elfldr");
        return false;
    }
    for (int i = 0; i < 100; i++) {
        if (websrvRunning()) return true;
        usleep(200 * 1000);
    }
    log("bootstrap: websrv não abriu a porta %d", WEBSRV_PORT);
    return false;
}

// GET /hbldr no websrv: abre o eboot.elf como big app (pipe=0: responde sem esperar o app terminar)
static bool launchViaWebsrv()
{
    int fd = connectLocal(WEBSRV_PORT);
    if (fd < 0) return false;
    std::string req = "GET /hbldr?pipe=0&daemon=0&cwd=" BIGAPP_DIR "&path=" BIGAPP_DIR "/eboot.elf HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";
    bool ok = sendAll(fd, req.data(), req.size());
    char buf[64] = {0};
    ssize_t n = ok ? recv(fd, buf, sizeof(buf) - 1, 0) : -1;
    close(fd);
    if (n <= 0) {
        log("bootstrap: sem resposta do websrv");
        return false;
    }
    buf[n] = 0;
    // "HTTP/1.1 200 OK"
    bool launched = strncmp(buf, "HTTP/1.", 7) == 0 && strncmp(buf + 9, "200", 3) == 0;
    if (!launched) log("bootstrap: websrv respondeu: %.20s", buf);
    return launched;
}

static const EmbeddedAsset* findAsset(const char* path)
{
    for (unsigned int i = 0; i < g_embeddedAssetCount; i++)
        if (strcmp(g_embeddedAssets[i].path, path) == 0) return &g_embeddedAssets[i];
    return nullptr;
}

// Grava um asset embutido em dest se faltar ou tiver outro tamanho (um .elf novo atualiza o app)
static bool installFile(const char* asset, const char* dest)
{
    const EmbeddedAsset* a = findAsset(asset);
    struct stat st;
    if (!a) return stat(dest, &st) == 0; // build sem o app embutido: usa o que já estiver lá
    if (stat(dest, &st) == 0 && (unsigned long long)st.st_size == a->size) return true;
    if (!writeFile(dest, std::string((const char*)a->data, a->size))) {
        log("bootstrap: falha ao gravar %s", dest);
        return false;
    }
    log("bootstrap: gravado %s (%u bytes)", dest, a->size);
    return true;
}

static bool endsWith(const char* s, const char* suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

bool bootstrapAsApp(int argc, char* argv[])
{
    // já é o big app: o websrv põe em argv[0] o caminho do eboot.elf (ícone, Launcher ou bootstrap)
    if (argc > 0 && argv && argv[0] && endsWith(argv[0], "/eboot.elf")) return false;

    // o app regrava o log.txt ao abrir: o bootstrap usa um arquivo próprio
    mkdir(APP_DATA_DIR, 0777);
    setLogFile(APP_DATA_DIR "/bootstrap.txt");
    log("bootstrap: argv[0]=%s", argc > 0 && argv && argv[0] ? argv[0] : "(nenhum)");

    mkdir(BIGAPP_DIR, 0777);
    mkdir(BIGAPP_DIR "/sce_sys", 0777);
    if (!installFile("app/eboot.elf", BIGAPP_DIR "/eboot.elf") ||
        !installFile("app/icon0.png", BIGAPP_DIR "/sce_sys/icon0.png")) {
        notify("WillPro-TV: não foi possível criar os arquivos em " BIGAPP_DIR " (veja bootstrap.txt)");
        return true;
    }
    sceUserServiceInitialize(NULL);

    if (!websrvRunning() && !startWebsrv()) {
        notify("WillPro-TV: não foi possível iniciar o websrv (veja " APP_DATA_DIR "/bootstrap.txt)");
        return true;
    }
    installShortcut();
    if (!launchViaWebsrv()) {
        notify("WillPro-TV: falha ao abrir o app (veja " APP_DATA_DIR "/bootstrap.txt)");
        return true;
    }
    log("bootstrap: app aberto pelo websrv");
    return true;
}

} // namespace platform
#endif
