// Player interno baseado no FFmpeg (substitui o ExoPlayer do Android e o AvPlayer do PS4).
// Lê TS, HLS, MP4, MKV, AVI... via sceHttp (AVIOContext próprio) e decodifica H.264/HEVC/AAC/AC3 etc.
//
// Threads: leitura (demux) -> filas de pacotes -> decodificação de vídeo / áudio.
// O áudio dita o relógio; a thread principal mostra o quadro de vídeo cujo horário chegou.
#include "player.h"
#include "platform.h"
#include "http.h"

#include <algorithm>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <pthread.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#ifdef WILLPRO_PS5
// O PS5 tem a mesma saída de áudio do PS4 (libSceAudioOut); valores do OpenOrbis
extern "C" {
int32_t sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int32_t sceAudioOutOutput(int32_t port, const void* ptr);
int32_t sceAudioOutClose(int32_t port);
}
#define ORBIS_USER_SERVICE_USER_ID_SYSTEM 0xFF
#define ORBIS_AUDIO_OUT_PORT_TYPE_MAIN 0
#define ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1
#else
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#endif

#ifdef WILLPRO_PS5
// PS5: o FFmpeg vem pronto do PacBrew; o CMakeLists.txt liga com --wrap=pthread_create e todas
// as threads criadas sem atributos (FFmpeg, std::thread) passam por aqui e ganham pilha de 1 MB.
extern "C" int __real_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg);
extern "C" int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg)
{
    static std::atomic<int> count{0};
    if (a) return __real_pthread_create(t, a, fn, arg);
    if (count++ == 0) platform::log("threads sem atributos usando pilha de 1 MB");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int r = __real_pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return r;
}
#else
// O FFmpeg é compilado com -Dpthread_create=willpro_pthread_create (scripts/build-ffmpeg.sh):
// as threads dele passam por aqui e ganham pilha de 1 MB (a padrão do musl é pequena).
extern "C" int willpro_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg)
{
    static std::atomic<int> count{0};
    if (count++ == 0) platform::log("threads do FFmpeg usando pilha de 1 MB");
    if (a) return pthread_create(t, a, fn, arg);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int r = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return r;
}
#endif

namespace player {

// Mensagens de erro/aviso do FFmpeg (inclui "Assertion ... failed" antes de um abort) vão para o log
static void ffLogCb(void*, int level, const char* fmt, va_list vl)
{
    if (level > AV_LOG_WARNING) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == 10 || buf[n - 1] == 13)) buf[--n] = 0;
    if (n) platform::log("ffmpeg[%d]: %s", level, buf);
}

// ---------------------------------------------------------------------------
// Threads com pilha grande (a pilha padrão do musl é pequena para decodificação)
// ---------------------------------------------------------------------------
struct Thread {
    pthread_t t{};
    bool running = false;
    std::function<void()> fn;
    static void* entry(void* p)
    {
        static_cast<Thread*>(p)->fn();
        return nullptr;
    }
    void start(std::function<void()> f)
    {
        fn = std::move(f);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
        running = pthread_create(&t, &attr, entry, this) == 0;
        pthread_attr_destroy(&attr);
    }
    void join()
    {
        if (running) pthread_join(t, nullptr);
        running = false;
    }
};

// ---------------------------------------------------------------------------
// Estado global do player
// ---------------------------------------------------------------------------
static std::atomic<bool> g_stop{false};
static std::atomic<int> g_state{(int)State::Idle};
static std::atomic<bool> g_paused{false};
static std::atomic<bool> g_stall{true};      // enchendo o buffer: relógio parado
static std::atomic<double> g_stallTarget{1.5}; // segundos necessários para voltar a tocar
static std::atomic<uint64_t> g_stallEndAt{0};  // quando o buffer ficou pronto pela última vez
static std::atomic<bool> g_eof{false};
static std::atomic<int> g_serial{0};
static std::atomic<bool> g_opened{false};   // avformat aberto e threads rodando
static std::atomic<bool> g_active{false};   // open() chamado e ainda não fechado
static std::mutex g_errMtx;
static std::string g_error;

static AVFormatContext* g_fmt = nullptr;
static AVIOContext* g_mainIO = nullptr;
static AVCodecContext* g_vdec = nullptr;
static AVCodecContext* g_adec = nullptr;
static std::atomic<int> g_vidx{-1}, g_aidx{-1};
static AVRational g_vtb{1, 1000}, g_atb{1, 1000};
static int64_t g_durationMs = 0;
static double g_startTime = 0; // início do arquivo (s)

static Thread g_openThread, g_readThread, g_videoThread, g_audioThread;

// busca
static std::mutex g_seekMtx;
static bool g_seekReq = false;
static std::atomic<bool> g_seekFlag{false}; // busca pendente (acorda a leitura bloqueada)
static int64_t g_seekTargetMs = 0;
static std::atomic<int64_t> g_seekShowMs{-1}; // posição exibida enquanto busca

// relógio
static std::mutex g_clockMtx;
static double g_clockPts = 0;        // segundos
static uint64_t g_clockAt = 0;       // ticks em que g_clockPts foi medido
static bool g_clockReady = false;
static int g_clockSerial = -1;
static std::atomic<bool> g_videoMaster{false}; // áudio ausente/travado: vídeo dita o relógio
static std::atomic<bool> g_audioBroken{false}; // saída de áudio não abriu
static std::atomic<uint64_t> g_firstFrameAt{0};

// saída de vídeo (ARGB na resolução da tela)
static std::vector<uint32_t> g_frame;
static int g_frameW = 0, g_frameH = 0;
static std::atomic<bool> g_hasFrame{false};
static SwsContext* g_sws = nullptr;
static int g_lastSrcW = 0, g_lastSrcH = 0, g_lastSrcFmt = -1;
static int g_dx = 0, g_dy = 0, g_dw = 0, g_dh = 0;

static void setError(const std::string& e)
{
    std::lock_guard<std::mutex> lk(g_errMtx);
    if (g_error.empty()) g_error = e;
    g_state = (int)State::Error;
    platform::log("Player erro: %s", e.c_str());
}

static std::string averr(int e)
{
    char b[128];
    av_strerror(e, b, sizeof(b));
    return b;
}

// ---------------------------------------------------------------------------
// E/S HTTP para o FFmpeg (usa sceHttp; suporta Range para avançar/voltar)
// ---------------------------------------------------------------------------
struct HttpIO {
    std::string url;
    http::Stream st;
    int64_t pos = 0;
    int64_t size = -1;
    bool seekable = false;
};

static std::mutex g_ioMtx;
static std::vector<HttpIO*> g_ios;

static int ioRead(void* opaque, uint8_t* buf, int size)
{
    HttpIO* io = (HttpIO*)opaque;
    if (g_stop) return AVERROR_EXIT;
    if (io->size >= 0 && io->pos >= io->size) return AVERROR_EOF;
    // pequeno salto para frente na mesma conexão: lê e descarta em vez de reabrir
    if (io->st.open && io->pos > (int64_t)io->st.pos && io->pos - (int64_t)io->st.pos < (1 << 20)) {
        uint8_t tmp[16384];
        while ((int64_t)io->st.pos < io->pos) {
            uint32_t want = (uint32_t)std::min<int64_t>((int64_t)sizeof(tmp), io->pos - (int64_t)io->st.pos);
            if (http::streamRead(io->st, tmp, want) <= 0) { http::streamClose(io->st); break; }
        }
    }
    // Conexões ociosas (buffer cheio) costumam ser fechadas pelo servidor: reconecta e
    // continua do mesmo byte em vez de tratar a queda como fim do arquivo.
    for (int attempt = 0; attempt < 6 && !g_stop; attempt++) {
        if (!io->st.open || (int64_t)io->st.pos != io->pos) {
            // ao vivo (tamanho desconhecido): reconecta do início da transmissão
            uint64_t from = io->size >= 0 ? (uint64_t)io->pos : 0;
            if (!http::streamOpen(io->st, io->url, from)) {
                platform::log("reconexao %d falhou (%s)", attempt + 1, http::lastStreamError().c_str());
                platform::sleepMs(500u * (attempt + 1));
                continue;
            }
            if (io->size < 0) io->st.pos = (uint64_t)io->pos; // fluxo contínuo: só segue lendo
        }
        int n = http::streamRead(io->st, buf, (uint32_t)size);
        if (n > 0) {
            io->pos += n;
            return n;
        }
        http::streamClose(io->st);
        if (n == 0 && io->size >= 0 && io->pos >= io->size) return AVERROR_EOF; // fim real
        if (g_stop) break;
        platform::log("conexao caiu em %lld/%lld (n=%d), reconectando", (long long)io->pos, (long long)io->size, n);
    }
    return g_stop ? AVERROR_EXIT : AVERROR(EIO);
}

static int64_t ioSeek(void* opaque, int64_t offset, int whence)
{
    HttpIO* io = (HttpIO*)opaque;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return io->size;
    if (!io->seekable) return AVERROR(ENOSYS);
    int64_t np;
    if (whence == SEEK_SET) np = offset;
    else if (whence == SEEK_CUR) np = io->pos + offset;
    else if (whence == SEEK_END) np = io->size + offset;
    else return AVERROR(EINVAL);
    if (np < 0) return AVERROR(EINVAL);
    io->pos = np;
    return np;
}

static AVIOContext* ioOpen(const std::string& url)
{
    HttpIO* io = new HttpIO();
    io->url = url;
    {
        std::lock_guard<std::mutex> lk(g_ioMtx);
        g_ios.push_back(io); // registrado antes de conectar, para poder abortar
    }
    auto unregister = [io] {
        std::lock_guard<std::mutex> lk(g_ioMtx);
        for (size_t i = 0; i < g_ios.size(); i++)
            if (g_ios[i] == io) { g_ios.erase(g_ios.begin() + (long)i); break; }
    };
    if (g_stop || !http::streamOpen(io->st, url, 0)) {
        unregister();
        http::streamClose(io->st);
        delete io;
        return nullptr;
    }
    io->size = io->st.total ? (int64_t)io->st.total : -1;
    io->seekable = io->st.rangeOk && io->size > 0;
    const int bufSize = 256 * 1024;
    uint8_t* buf = (uint8_t*)av_malloc(bufSize);
    AVIOContext* pb = avio_alloc_context(buf, bufSize, 0, io, ioRead, nullptr, ioSeek);
    if (!pb) {
        av_free(buf);
        unregister();
        http::streamClose(io->st);
        delete io;
        return nullptr;
    }
    pb->seekable = io->seekable ? AVIO_SEEKABLE_NORMAL : 0;
    platform::log("IO aberto: tamanho=%lld seekable=%d", (long long)io->size, io->seekable ? 1 : 0);
    return pb;
}

static void ioFree(AVIOContext* pb)
{
    if (!pb) return;
    HttpIO* io = (HttpIO*)pb->opaque;
    if (io) {
        {
            std::lock_guard<std::mutex> lk(g_ioMtx);
            for (size_t i = 0; i < g_ios.size(); i++)
                if (g_ios[i] == io) { g_ios.erase(g_ios.begin() + (long)i); break; }
        }
        http::streamClose(io->st);
        delete io;
    }
    av_freep(&pb->buffer);
    avio_context_free(&pb);
}

// HLS e outros formatos abrem URLs adicionais (segmentos) por aqui
static int ioOpenCb(AVFormatContext*, AVIOContext** pb, const char* url, int, AVDictionary**)
{
    AVIOContext* c = ioOpen(url);
    if (!c) return AVERROR(EIO);
    *pb = c;
    return 0;
}

static int ioCloseCb(AVFormatContext*, AVIOContext* pb)
{
    ioFree(pb);
    return 0;
}

static int interruptCb(void*) { return g_stop ? 1 : 0; }

static void abortAllIO()
{
    std::lock_guard<std::mutex> lk(g_ioMtx);
    for (auto* io : g_ios) http::streamAbort(io->st);
}

// ---------------------------------------------------------------------------
// Filas
// ---------------------------------------------------------------------------
struct PktEntry {
    AVPacket* pkt; // nullptr = fim (esvaziar o decodificador)
    int serial;
    double ts;     // carimbo de tempo (s) ou NAN
};

struct PacketQueue {
    std::deque<PktEntry> q;
    size_t bytes = 0;
    std::mutex m;
    std::condition_variable cv;
    size_t maxBytes = 32u << 20;
    size_t maxCount = 1500; // ~1 min de vídeo: limita a memória mesmo com pacotes pequenos
    const AVRational* tb = nullptr;

    void push(AVPacket* p, int serial)
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] {
            return g_stop || g_seekFlag || q.size() < 8 || (bytes < maxBytes && q.size() < maxCount);
        });
        // parado ou busca pendente: descarta (a busca vai esvaziar as filas de qualquer forma)
        if (g_stop || (g_seekFlag && p)) { if (p) av_packet_free(&p); return; }
        double ts = NAN;
        if (p && tb) {
            int64_t t = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
            if (t != AV_NOPTS_VALUE) ts = (double)t * av_q2d(*tb);
        }
        q.push_back({p, serial, ts});
        if (p) bytes += (size_t)p->size;
        cv.notify_all();
    }
    // Espera sem tempo limite: no PS4 o wait_for da libc++ lança exceção quando o tempo esgota
    // (a libkernel devolve um código de ETIMEDOUT diferente do esperado). close() acorda com wake().
    bool pop(PktEntry& out, int /*timeoutMs*/)
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return g_stop || !q.empty(); });
        if (g_stop || q.empty()) return false;
        out = q.front();
        q.pop_front();
        if (out.pkt) bytes -= (size_t)out.pkt->size;
        cv.notify_all();
        return true;
    }
    void flush()
    {
        std::lock_guard<std::mutex> lk(m);
        for (auto& e : q) if (e.pkt) av_packet_free(&e.pkt);
        q.clear();
        bytes = 0;
        cv.notify_all();
    }
    size_t size()
    {
        std::lock_guard<std::mutex> lk(m);
        return q.size();
    }
    // segundos de mídia guardados na fila
    double duration()
    {
        std::lock_guard<std::mutex> lk(m);
        double first = NAN, last = NAN;
        for (auto& e : q) if (!std::isnan(e.ts)) { first = e.ts; break; }
        for (auto it = q.rbegin(); it != q.rend(); ++it) if (!std::isnan(it->ts)) { last = it->ts; break; }
        if (std::isnan(first) || std::isnan(last) || last < first) return 0.0;
        return last - first;
    }
    bool full()
    {
        std::lock_guard<std::mutex> lk(m);
        return bytes >= maxBytes || q.size() >= maxCount;
    }
    void wake()
    {
        std::lock_guard<std::mutex> lk(m);
        cv.notify_all();
    }
};

struct VFrame {
    AVFrame* f;
    double pts;
    int serial;
};

static PacketQueue g_vq, g_aq;
static struct QueueInit { QueueInit() { g_aq.maxBytes = 6u << 20; g_aq.maxCount = 3000; } } g_queueInit;
static std::deque<VFrame> g_fq;
static std::mutex g_fqMtx;
static std::condition_variable g_fqCv;
static const size_t kMaxFrames = 4;

static void clearFrames()
{
    std::lock_guard<std::mutex> lk(g_fqMtx);
    for (auto& v : g_fq) av_frame_free(&v.f);
    g_fq.clear();
    g_fqCv.notify_all();
}

// ---------------------------------------------------------------------------
// Relógio
// ---------------------------------------------------------------------------
static void setClock(double pts, int serial)
{
    std::lock_guard<std::mutex> lk(g_clockMtx);
    g_clockPts = pts;
    g_clockAt = platform::ticksMs();
    g_clockReady = true;
    g_clockSerial = serial;
}

static bool getClock(double& out)
{
    std::lock_guard<std::mutex> lk(g_clockMtx);
    if (!g_clockReady || g_clockSerial != g_serial) return false;
    double elapsed = (g_paused || g_stall) ? 0.0 : (double)(platform::ticksMs() - g_clockAt) / 1000.0;
    // com áudio o relógio é atualizado a cada ~21 ms; se parar por mais de 1 s, o vídeo assume
    if (g_aidx >= 0 && !g_videoMaster && elapsed > 1.0) {
        platform::log("audio parou de atualizar o relogio: video assume");
        g_videoMaster = true;
    }
    out = g_clockPts + elapsed;
    return true;
}

static void resetClock()
{
    std::lock_guard<std::mutex> lk(g_clockMtx);
    g_clockReady = false;
}

// ---------------------------------------------------------------------------
// Thread de leitura (demux)
// ---------------------------------------------------------------------------
static void readLoop()
{
    AVPacket* pkt = av_packet_alloc();
    while (!g_stop) {
        bool doSeek = false;
        int64_t target = 0;
        {
            std::lock_guard<std::mutex> lk(g_seekMtx);
            if (g_seekReq) { doSeek = true; target = g_seekTargetMs; g_seekReq = false; g_seekFlag = false; }
        }
        if (doSeek) {
            platform::log("seek para %lld ms (mem livre %d MB)", (long long)target, platform::freeMemoryMB());
            int64_t ts = target * 1000 + (int64_t)(g_startTime * AV_TIME_BASE);
            int r = avformat_seek_file(g_fmt, -1, INT64_MIN, ts, INT64_MAX, 0);
            platform::log("seek resultado: %s", r < 0 ? averr(r).c_str() : "ok");
            g_vq.flush();
            g_aq.flush();
            g_serial++;
            clearFrames();
            resetClock();
            g_firstFrameAt = 0;
            if (!g_audioBroken) g_videoMaster = false;
            g_stallTarget = 1.5;
            g_stall = true;
            g_eof = false;
            platform::log("seek concluido (serial %d)", g_serial.load());
        }

        if (g_eof) { platform::sleepMs(20); continue; }

        static uint64_t lastMemLog = 0;
        if (platform::ticksMs() - lastMemLog > 5000) {
            lastMemLog = platform::ticksMs();
            platform::log("mem livre %d MB | filas v=%zu a=%zu", platform::freeMemoryMB(), g_vq.size(), g_aq.size());
        }
        int r = av_read_frame(g_fmt, pkt);
        if (r < 0) {
            if (g_stop) break;
            platform::log("fim da leitura: %s", averr(r).c_str());
            // esvazia os decodificadores
            if (g_vidx >= 0) g_vq.push(nullptr, g_serial);
            if (g_aidx >= 0) g_aq.push(nullptr, g_serial);
            g_eof = true;
            continue;
        }
        if (pkt->stream_index == g_vidx) g_vq.push(av_packet_clone(pkt), g_serial);
        else if (pkt->stream_index == g_aidx) g_aq.push(av_packet_clone(pkt), g_serial);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// Decodificação de vídeo
// ---------------------------------------------------------------------------
static void videoLoop()
{
    AVFrame* frame = av_frame_alloc();
    int serial = -1;
    while (!g_stop) {
        PktEntry e;
        if (!g_vq.pop(e, 50)) continue;
        if (e.serial != serial) {
            avcodec_flush_buffers(g_vdec);
            serial = e.serial;
        }
        int r = avcodec_send_packet(g_vdec, e.pkt);
        if (e.pkt) av_packet_free(&e.pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) continue;
        for (;;) {
            r = avcodec_receive_frame(g_vdec, frame);
            if (r < 0) break;
            int64_t ts = frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = frame->pts;
            double pts = ts == AV_NOPTS_VALUE ? 0.0 : (double)ts * av_q2d(g_vtb) - g_startTime;
            VFrame v{av_frame_alloc(), pts, serial};
            av_frame_move_ref(v.f, frame);
            std::unique_lock<std::mutex> lk(g_fqMtx);
            g_fqCv.wait(lk, [] { return g_stop || g_fq.size() < kMaxFrames; });
            if (g_stop || serial != g_serial) { av_frame_free(&v.f); continue; }
            g_fq.push_back(v);
        }
    }
    av_frame_free(&frame);
}

// ---------------------------------------------------------------------------
// Decodificação + saída de áudio (S16 estéreo 48 kHz no AudioOut do PS4)
// ---------------------------------------------------------------------------
static void audioLoop()
{
    const int kGrain = 1024;
    int port = -1;
    AVFrame* frame = av_frame_alloc();
    SwrContext* swr = nullptr;
    int swrRate = 0, swrFmt = -1, swrCh = 0;
    std::vector<int16_t> pending;
    double pendingPts = 0;
    int serial = -1;
    std::vector<int16_t> outBuf;

    while (!g_stop) {
        if (g_paused || g_stall) { platform::sleepMs(10); continue; }

        // envia o que já está pronto
        if (pending.size() >= (size_t)kGrain * 2) {
            if (port < 0) {
                port = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrain,
                                       48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                if (port < 0) {
                    platform::log("sceAudioOutOpen(sistema) 0x%08X", port);
                    port = sceAudioOutOpen(platform::userId(), ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrain, 48000,
                                           ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                }
                if (port < 0) {
                    platform::log("sceAudioOutOpen 0x%08X: sem som", port);
                    g_audioBroken = true;
                    g_videoMaster = true;
                    // sem saída de áudio: descarta e continua (o vídeo segue sozinho)
                    pending.clear();
                    platform::sleepMs(20);
                    continue;
                }
                platform::log("audio aberto (porta %d)", port);
            }
            sceAudioOutOutput(port, pending.data()); // bloqueia ~21 ms: dá o ritmo
            if (serial == g_serial && !g_videoMaster) setClock(pendingPts, serial);
            pending.erase(pending.begin(), pending.begin() + kGrain * 2);
            pendingPts += (double)kGrain / 48000.0;
            continue;
        }

        PktEntry e;
        if (!g_aq.pop(e, 30)) continue;
        if (e.serial != serial) {
            avcodec_flush_buffers(g_adec);
            serial = e.serial;
            pending.clear();
        }
        int r = avcodec_send_packet(g_adec, e.pkt);
        if (e.pkt) av_packet_free(&e.pkt);
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) continue;
        while (avcodec_receive_frame(g_adec, frame) >= 0) {
            if (serial != g_serial) { av_frame_unref(frame); continue; }
            int ch = frame->ch_layout.nb_channels;
            if (!swr || swrRate != frame->sample_rate || swrFmt != frame->format || swrCh != ch) {
                swr_free(&swr);
                AVChannelLayout in;
                memset(&in, 0, sizeof(in));
                if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || ch == 0) av_channel_layout_default(&in, ch ? ch : 2);
                else av_channel_layout_copy(&in, &frame->ch_layout);
                AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
                int ok = swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, 48000, &in, (AVSampleFormat)frame->format,
                                             frame->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&in);
                if (ok < 0 || swr_init(swr) < 0) {
                    swr_free(&swr);
                    av_frame_unref(frame);
                    continue;
                }
                swrRate = frame->sample_rate;
                swrFmt = frame->format;
                swrCh = ch;
            }
            int64_t ts = frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = frame->pts;
            int maxOut = swr_get_out_samples(swr, frame->nb_samples) + 32;
            outBuf.resize((size_t)maxOut * 2);
            uint8_t* outp = (uint8_t*)outBuf.data();
            int got = swr_convert(swr, &outp, maxOut, (const uint8_t**)frame->extended_data, frame->nb_samples);
            if (got > 0) {
                if (pending.empty() && ts != AV_NOPTS_VALUE)
                    pendingPts = (double)ts * av_q2d(g_atb) - g_startTime;
                pending.insert(pending.end(), outBuf.begin(), outBuf.begin() + got * 2);
            }
            av_frame_unref(frame);
        }
    }
    if (port >= 0) {
        sceAudioOutOutput(port, NULL);
        sceAudioOutClose(port);
    }
    swr_free(&swr);
    av_frame_free(&frame);
}

// ---------------------------------------------------------------------------
// Abertura (roda fora da thread da interface)
// ---------------------------------------------------------------------------
static AVCodecContext* openDecoder(int idx, bool video)
{
    AVStream* st = g_fmt->streams[idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        platform::log("sem decodificador para %s", avcodec_get_name(st->codecpar->codec_id));
        return nullptr;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c, st->codecpar);
    c->pkt_timebase = st->time_base;
    if (video) {
        // cada thread de quadro guarda quadros inteiros na memória: HEVC/4K usam menos threads
        bool heavy = st->codecpar->codec_id == AV_CODEC_ID_HEVC || st->codecpar->width > 1920;
        c->thread_count = heavy ? 3 : 6;
        c->thread_type = heavy ? FF_THREAD_FRAME : (FF_THREAD_FRAME | FF_THREAD_SLICE);
        c->flags2 |= AV_CODEC_FLAG2_FAST;
        // 1080p ou mais em CPU: pula o deblocking dos quadros não-referência (mais leve)
        if (st->codecpar->height >= 1000) c->skip_loop_filter = AVDISCARD_NONREF;
    }
    int r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
        platform::log("avcodec_open2: %s", averr(r).c_str());
        avcodec_free_context(&c);
        return nullptr;
    }
    platform::log("%s: %s %dx%d threads=%d (mem livre %d MB)", video ? "video" : "audio", codec->name, c->width,
                  c->height, c->thread_count, platform::freeMemoryMB());
    return c;
}

static void openWorker(std::string url)
{
    g_mainIO = ioOpen(url);
    if (!g_mainIO) {
        if (!g_stop) setError("Não foi possível conectar ao vídeo (" + http::lastStreamError() + ").");
        return;
    }
    g_fmt = avformat_alloc_context();
    g_fmt->pb = g_mainIO;
    g_fmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_GENPTS;
    g_fmt->io_open = ioOpenCb;
    g_fmt->io_close2 = ioCloseCb;
    g_fmt->interrupt_callback.callback = interruptCb;
    g_fmt->probesize = 3 * 1024 * 1024;
    g_fmt->max_analyze_duration = 4 * AV_TIME_BASE;

    int r = avformat_open_input(&g_fmt, url.c_str(), nullptr, nullptr);
    if (r < 0) {
        g_fmt = nullptr; // avformat_open_input libera o contexto em caso de erro
        if (!g_stop) setError("Formato não reconhecido (" + averr(r) + ").");
        return;
    }
    r = avformat_find_stream_info(g_fmt, nullptr);
    if (r < 0 && !g_stop) platform::log("find_stream_info: %s", averr(r).c_str());
    if (g_stop) return;

    int vidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int aidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, vidx, nullptr, 0);
    if (vidx >= 0) {
        g_vdec = openDecoder(vidx, true);
        if (!g_vdec) vidx = -1;
        else g_vtb = g_fmt->streams[vidx]->time_base;
    }
    if (aidx >= 0) {
        g_adec = openDecoder(aidx, false);
        if (!g_adec) aidx = -1;
        else g_atb = g_fmt->streams[aidx]->time_base;
    }
    if (vidx < 0 && aidx < 0) {
        setError("Nenhuma faixa de vídeo/áudio suportada neste conteúdo.");
        return;
    }
    g_vidx = vidx;
    g_aidx = aidx;
    g_vq.tb = &g_vtb;
    g_aq.tb = &g_atb;
    // descarta faixas que não usamos (economiza leitura)
    for (unsigned i = 0; i < g_fmt->nb_streams; i++)
        if ((int)i != vidx && (int)i != aidx) g_fmt->streams[i]->discard = AVDISCARD_ALL;

    g_startTime = g_fmt->start_time != AV_NOPTS_VALUE ? (double)g_fmt->start_time / AV_TIME_BASE : 0.0;
    g_durationMs = g_fmt->duration > 0 ? g_fmt->duration / 1000 : 0;
    // transmissão ao vivo (sem acesso aleatório): sem duração/busca
    if (!(g_mainIO->seekable & AVIO_SEEKABLE_NORMAL) && strcmp(g_fmt->iformat->name, "hls") != 0) g_durationMs = 0;
    platform::log("formato=%s duracao=%lldms", g_fmt->iformat->name, (long long)g_durationMs);

    g_opened = true;
    g_readThread.start(readLoop);
    if (vidx >= 0) g_videoThread.start(videoLoop);
    if (aidx >= 0) g_audioThread.start(audioLoop);
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
bool open(const std::string& url)
{
    close();
    {
        std::lock_guard<std::mutex> lk(g_errMtx);
        g_error.clear();
    }
    g_stop = false;
    g_paused = false;
    g_stall = true;
    g_stallTarget = 1.5;
    g_eof = false;
    g_hasFrame = false;
    g_durationMs = 0;
    g_seekShowMs = -1;
    resetClock();
    g_videoMaster = false;
    g_audioBroken = false;
    g_firstFrameAt = 0;
    g_state = (int)State::Opening;
    platform::log("abrindo player (mem livre %d MB)", platform::freeMemoryMB());
    g_active = true;
    static bool logSet = false;
    if (!logSet) { logSet = true; av_log_set_callback(ffLogCb); }
    g_openThread.start([url] { openWorker(url); });
    return true;
}

void close()
{
    if (!g_active) return;
    g_stop = true;
    abortAllIO();
    g_vq.wake();
    g_aq.wake();
    {
        std::lock_guard<std::mutex> lk(g_fqMtx);
        g_fqCv.notify_all();
    }
    g_openThread.join();
    g_readThread.join();
    g_videoThread.join();
    g_audioThread.join();

    g_vq.flush();
    g_aq.flush();
    clearFrames();
    if (g_vdec) avcodec_free_context(&g_vdec);
    if (g_adec) avcodec_free_context(&g_adec);
    if (g_fmt) avformat_close_input(&g_fmt);
    if (g_mainIO) { ioFree(g_mainIO); g_mainIO = nullptr; }
    g_vidx = -1;
    g_aidx = -1;
    g_opened = false;
    g_active = false;
    g_hasFrame = false;
    g_state = (int)State::Idle;
}

bool isOpen() { return g_active; }

State state()
{
    State s = (State)g_state.load();
    if (s == State::Error || s == State::Idle) return s;
    if (!g_opened) return State::Opening;
    if (g_paused) return State::Paused;
    if (g_stall && g_hasFrame) return State::Buffering;
    size_t fq;
    {
        std::lock_guard<std::mutex> lk(g_fqMtx);
        fq = g_fq.size();
    }
    if (g_eof && g_vq.size() == 0 && g_aq.size() == 0 && fq == 0) return State::Ended;
    if (g_vidx >= 0 && !g_hasFrame) return State::Opening;
    if (g_vidx >= 0 && fq == 0 && g_vq.size() == 0 && !g_eof) return State::Buffering;
    return State::Playing;
}

std::string errorText()
{
    std::lock_guard<std::mutex> lk(g_errMtx);
    return g_error;
}

uint64_t positionMs()
{
    int64_t s = g_seekShowMs;
    double c;
    if (getClock(c)) {
        g_seekShowMs = -1;
        return c > 0 ? (uint64_t)(c * 1000.0) : 0;
    }
    return s >= 0 ? (uint64_t)s : 0;
}

uint64_t durationMs() { return g_durationMs > 0 ? (uint64_t)g_durationMs : 0; }

bool hasVideoFrame() { return g_hasFrame || (g_vidx < 0 && g_opened); }

void pause()
{
    if (!g_opened) return;
    double c;
    if (getClock(c)) setClock(c, g_serial); // congela o relógio
    g_paused = true;
}

void resume()
{
    if (!g_opened) return;
    double c;
    bool ok = getClock(c);
    g_paused = false;
    if (ok) setClock(c, g_serial);
}

void togglePause()
{
    if (g_paused) resume();
    else pause();
}

void seekTo(uint64_t ms)
{
    if (!g_opened || g_durationMs == 0) return;
    {
        std::lock_guard<std::mutex> lk(g_seekMtx);
        g_seekReq = true;
        g_seekTargetMs = (int64_t)ms;
        g_seekShowMs = (int64_t)ms;
        g_seekFlag = true;
    }
    platform::log("busca pedida: %llu ms", (unsigned long long)ms);
    g_vq.wake(); // a leitura pode estar esperando espaço na fila
    g_aq.wake();
}

// ---------------------------------------------------------------------------
// Vídeo -> tela
// ---------------------------------------------------------------------------
static void convertFrame(AVFrame* f, SDL_Surface* dst)
{
    int outW = dst->w, outH = dst->h;
    if (f->width <= 0 || f->height <= 0 || f->format < 0 || !f->data[0]) return;
    // folga de 2 linhas: o swscale (SIMD) pode escrever alguns pixels além da largura pedida
    const size_t needed = (size_t)outW * (outH + 2);
    if (g_frame.size() != needed) {
        g_frame.assign(needed, 0xFF000000u);
        g_lastSrcW = 0;
    }
    g_frameW = outW;
    g_frameH = outH;
    if (f->width != g_lastSrcW || f->height != g_lastSrcH || f->format != g_lastSrcFmt) {
        // área de destino respeitando a proporção (inclui pixel não quadrado)
        AVRational sar = f->sample_aspect_ratio;
        double ar = (double)f->width / (double)f->height;
        if (sar.num > 0 && sar.den > 0) ar *= av_q2d(sar);
        g_dw = outW;
        g_dh = (int)(outW / ar);
        if (g_dh > outH) { g_dh = outH; g_dw = (int)(outH * ar); }
        g_dw &= ~15; // largura múltipla de 16: caminho SIMD seguro do swscale
        g_dh &= ~1;
        if (g_dw < 16 || g_dh < 2) return;
        g_dx = (outW - g_dw) / 2;
        g_dy = (outH - g_dh) / 2;
        std::fill(g_frame.begin(), g_frame.end(), 0xFF000000u);
        platform::log("video %dx%d fmt=%s -> %dx%d", f->width, f->height,
                      av_get_pix_fmt_name((AVPixelFormat)f->format), g_dw, g_dh);
        g_lastSrcW = f->width;
        g_lastSrcH = f->height;
        g_lastSrcFmt = f->format;
    }
    // formato de pixel da superfície: ARGB8888 (memória B,G,R,A) ou ABGR8888 (R,G,B,A)
    AVPixelFormat outFmt = dst->format->Rmask == 0x000000FF ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
    g_sws = sws_getCachedContext(g_sws, f->width, f->height, (AVPixelFormat)f->format, g_dw, g_dh, outFmt,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!g_sws) { platform::log("sws_getCachedContext falhou"); return; }
    uint8_t* dstData[4] = {(uint8_t*)&g_frame[(size_t)g_dy * outW + g_dx], nullptr, nullptr, nullptr};
    int dstLines[4] = {outW * 4, 0, 0, 0};
    sws_scale(g_sws, f->data, f->linesize, 0, f->height, dstData, dstLines);
    g_hasFrame = true;
}

// Pausa o relógio quando o buffer esvazia e só retoma com alguns segundos guardados
static void updateBuffering()
{
    if (!g_opened) return;
    PacketQueue& q = g_vidx >= 0 ? g_vq : g_aq;
    double have = q.duration();
    if (!g_stall) {
        bool empty = g_vidx >= 0 ? (g_vq.size() == 0) : (g_aq.size() == 0);
        size_t fq;
        {
            std::lock_guard<std::mutex> lk(g_fqMtx);
            fq = g_fq.size();
        }
        if (empty && fq == 0 && !g_eof && g_hasFrame) {
            double c;
            if (getClock(c)) setClock(c, g_serial);
            g_stallTarget = 3.0;
            g_stall = true;
            platform::log("buffer vazio: carregando %.1f s", g_stallTarget.load());
        }
    } else if (g_eof || have >= g_stallTarget || q.full()) {
        double c;
        bool ok = getClock(c);
        g_stall = false;
        g_stallEndAt = platform::ticksMs();
        if (ok) setClock(c, g_serial);
        platform::log("buffer pronto (%.1f s guardados)", have);
    }
}

void render(SDL_Surface* dst)
{
    if (!dst) return;
    updateBuffering();
    AVFrame* show = nullptr;
    if (g_opened && g_vidx >= 0) {
        std::lock_guard<std::mutex> lk(g_fqMtx);
        while (!g_fq.empty()) {
            VFrame& v = g_fq.front();
            if (v.serial != g_serial) {
                av_frame_free(&v.f);
                g_fq.pop_front();
                continue;
            }
            double clock;
            bool haveClock = getClock(clock);
            uint64_t since = std::max<uint64_t>(g_firstFrameAt, g_stallEndAt);
            if (!haveClock && g_aidx >= 0 && !g_stall && g_hasFrame && g_firstFrameAt &&
                platform::ticksMs() - since > 1200) {
                platform::log("audio nao iniciou: video assume o relogio");
                g_videoMaster = true;
            }
            if (!haveClock) {
                if (g_aidx < 0 || g_videoMaster) {
                    // sem áudio: o vídeo dita o relógio
                    setClock(v.pts, v.serial);
                    haveClock = getClock(clock);
                } else {
                    // aguardando o áudio começar: mostra o primeiro quadro como prévia
                    if (!g_hasFrame && !show) {
                        show = v.f;
                        g_fq.pop_front();
                    }
                    break;
                }
            }
            if (!haveClock) break;
            if (v.pts <= clock + 0.010) {
                if (show) av_frame_free(&show);
                show = v.f; // o mais recente cujo horário já chegou
                g_fq.pop_front();
                continue;
            }
            // salto de timestamp em transmissão sem áudio: realinha o relógio
            if (v.pts - clock > 3.0 && (g_aidx < 0 || g_videoMaster)) setClock(v.pts, v.serial);
            break;
        }
        g_fqCv.notify_all();
    }
    if (show) {
        convertFrame(show, dst);
        av_frame_free(&show);
        if (!g_firstFrameAt) g_firstFrameAt = platform::ticksMs();
    }

    if (g_hasFrame && g_frameW == dst->w && g_frameH == dst->h) {
        SDL_LockSurface(dst);
        for (int y = 0; y < dst->h; y++)
            memcpy((uint8_t*)dst->pixels + (size_t)y * dst->pitch, &g_frame[(size_t)y * g_frameW], (size_t)g_frameW * 4);
        SDL_UnlockSurface(dst);
    } else {
        SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));
    }
}

} // namespace player
