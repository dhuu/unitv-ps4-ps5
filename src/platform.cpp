#include "platform.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <pthread.h>

#ifdef WILLPRO_PS5
#include <unistd.h>
#include <csignal>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
// Bibliotecas do sistema do PS5 (stubs do ps5-payload-sdk); assinaturas iguais às do PS4
extern "C" {
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int32_t*);
int sceNetInit(void);
int sceNetGetMacAddress(uint8_t* addr, int flags);
int sceAudioOutInit(void);
int sceKernelAvailableFlexibleMemorySize(size_t*);
int sceKernelSendNotificationRequest(int, void*, size_t, int);
}
// Imagens e fontes embutidas no .elf (gerado por scripts/embed_assets.py)
struct EmbeddedAsset {
    const char* path;
    const unsigned char* data;
    unsigned int size;
};
extern const EmbeddedAsset g_embeddedAssets[];
extern const unsigned int g_embeddedAssetCount;
#else
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/Net.h>
#include <orbis/CommonDialog.h>
#include <orbis/AudioOut.h>
#endif

namespace platform {

static int32_t g_userId = -1;

#ifdef WILLPRO_PS5
// Grava os assets embutidos em APP_ASSETS (só os que faltam ou mudaram de tamanho)
static void extractAssets()
{
    ensureDir(APP_DATA_DIR);
    ensureDir(APP_ASSETS);
    int written = 0;
    for (unsigned int i = 0; i < g_embeddedAssetCount; i++) {
        const EmbeddedAsset& a = g_embeddedAssets[i];
        std::string path = std::string(APP_ASSETS) + "/" + a.path;
        size_t slash = path.rfind('/');
        if (slash != std::string::npos) ensureDir(path.substr(0, slash));
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && (unsigned long long)st.st_size == a.size) continue;
        if (writeFile(path, std::string((const char*)a.data, a.size))) written++;
        else log("falha ao gravar %s", path.c_str());
    }
    log("assets: %u embutidos, %d gravados em %s", g_embeddedAssetCount, written, APP_ASSETS);
}

// Nome do processo (o loader dá o mesmo nome a cada envio do payload)
static std::string processName(pid_t pid)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    memset(&kp, 0, sizeof(kp));
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0) return "";
    return kp.ki_comm;
}

// Um envio anterior do payload que continua aberto segura a memória de vídeo
// (sceKernelAllocateMainDirectMemory falha com EAGAIN): encerra-o antes de abrir a tela.
static void closePreviousInstance()
{
    const std::string pidFile = std::string(APP_DATA_DIR) + "/pid.txt";
    pid_t self = getpid();
    std::string old;
    if (readFile(pidFile, old)) {
        pid_t pid = (pid_t)atoi(old.c_str());
        std::string name = pid > 0 && pid != self ? processName(pid) : "";
        if (!name.empty() && name == processName(self)) {
            log("encerrando execução anterior (pid %d, %s)", (int)pid, name.c_str());
            kill(pid, SIGKILL);
            for (int i = 0; i < 30 && !processName(pid).empty(); i++) usleep(100 * 1000);
            usleep(500 * 1000); // tempo para o sistema liberar a memória de vídeo
        }
    }
    writeFile(pidFile, std::to_string((int)self));
}

bool init()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    ensureDir(APP_DATA_DIR);
    closePreviousInstance();
    int r = sceUserServiceInitialize(NULL);
    if (r != 0 && r != (int)0x80960003) log("sceUserServiceInitialize 0x%08X", r);
    if (sceUserServiceGetForegroundUser(&g_userId) != 0) g_userId = -1;
    sceNetInit();
    sceAudioOutInit();
    extractAssets();
    return true;
}
#else
bool init()
{
    setvbuf(stdout, NULL, _IONBF, 0);

    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_FREETYPE_OL);
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);

    sceUserServiceInitialize(NULL);
    sceUserServiceGetInitialUser(&g_userId);
    sceCommonDialogInitialize();
    sceAudioOutInit();

    ensureDir(APP_DATA_DIR);
    return true;
}
#endif

int32_t userId() { return g_userId; }

std::string macAddress()
{
#ifdef WILLPRO_PS5
    uint8_t data[8];
    memset(data, 0, sizeof(data));
    if (sceNetGetMacAddress(data, 0) < 0)
        return "02:00:00:00:00:00";
#else
    OrbisNetEtherAddr addr;
    memset(&addr, 0, sizeof(addr));
    if (sceNetGetMacAddress(&addr, 0) < 0)
        return "02:00:00:00:00:00";
    const uint8_t* data = addr.data;
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             data[0], data[1], data[2], data[3], data[4], data[5]);
    return buf;
}

uint64_t ticksMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int64_t unixTime() { return (int64_t)time(NULL); }

#ifdef WILLPRO_PS5
void sleepMs(uint32_t ms) { usleep(ms * 1000); }

int freeMemoryMB()
{
    size_t sz = 0;
    if (sceKernelAvailableFlexibleMemorySize(&sz) < 0) return -1;
    return (int)(sz / (1024 * 1024));
}
#else
void sleepMs(uint32_t ms) { sceKernelUsleep(ms * 1000); }

// O header do OpenOrbis declara o parâmetro como size_t; a assinatura real recebe um ponteiro
typedef int32_t (*AvailFlexFn)(size_t*);

int freeMemoryMB()
{
    size_t sz = 0;
    if (((AvailFlexFn)(void*)&sceKernelAvailableFlexibleMemorySize)(&sz) < 0) return -1;
    return (int)(sz / (1024 * 1024));
}
#endif

void log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("[WillProTV] %s\n", buf);
    // também grava em /data/WillProTV/log.txt (pode ser copiado via FTP para diagnóstico)
    static FILE* lf = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        mkdir(APP_DATA_DIR, 0777);
        lf = fopen(APP_DATA_DIR "/log.txt", "w");
    }
    if (lf) {
        fprintf(lf, "[%llu] %s\n", (unsigned long long)ticksMs(), buf);
        fflush(lf);
    }
}

void notify(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log("aviso: %s", buf);
#ifdef WILLPRO_PS5
    // mesmo formato usado pelo elfldr/ftpsrv do ps5-payload-dev
    struct NotifyRequest {
        char unused[45];
        char message[3075];
    } req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, buf, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
}

bool readFile(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool writeFile(const std::string& path, const std::string& data)
{
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) return false;
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

static void* threadEntry(void* p)
{
    std::function<void()>* fn = (std::function<void()>*)p;
    (*fn)();
    delete fn;
    return nullptr;
}

bool startThread(std::function<void()> fn, size_t stackBytes)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackBytes);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    auto* heapFn = new std::function<void()>(std::move(fn));
    int r = pthread_create(&t, &attr, threadEntry, heapFn);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        delete heapFn;
        log("pthread_create falhou (%d)", r);
        return false;
    }
    return true;
}

void ensureDir(const std::string& path)
{
    mkdir(path.c_str(), 0777);
}

} // namespace platform
