#include "gfx.h"
#include "config.h"
#include "platform.h"
#include "http.h"
#include "util.h"

#ifdef UNITV_PS5
#include <ft2build.h>
#include FT_FREETYPE_H
#else
#include <proto-include.h>
#endif

#include <algorithm>
#include <map>
#include <unordered_map>
#include <list>
#include <deque>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

namespace gfx {

static SDL_Window* g_window = nullptr;
static SDL_Surface* g_screen = nullptr;
static SDL_Renderer* g_ren = nullptr;

static FT_Library g_ft;
static FT_Face g_faceRegular = nullptr;
static FT_Face g_faceBold = nullptr;

struct Glyph {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0, left = 0, top = 0, advance = 0;
};

struct FontCache {
    FT_Face face;
    int size;
    int ascender;
    int lineHeight;
    std::unordered_map<uint32_t, Glyph> glyphs;
};

static std::map<std::pair<int, bool>, FontCache*> g_fonts;

SDL_Renderer* renderer() { return g_ren; }
SDL_Surface* screen() { return g_screen; }

bool init()
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        platform::log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    g_window = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_W, SCREEN_H, 0);
    if (!g_window) { platform::log("SDL_CreateWindow: %s", SDL_GetError()); return false; }
#ifdef UNITV_PS5
    // o SDL do PS5 abriria o teclado do sistema sozinho; o app abre quando precisa (ime.cpp)
    SDL_StopTextInput();
#endif
    g_screen = SDL_GetWindowSurface(g_window);
    if (!g_screen) { platform::log("SDL_GetWindowSurface: %s", SDL_GetError()); return false; }
    g_ren = SDL_CreateSoftwareRenderer(g_screen);
    if (!g_ren) { platform::log("SDL_CreateSoftwareRenderer: %s", SDL_GetError()); return false; }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    if (FT_Init_FreeType(&g_ft) != 0) { platform::log("FT_Init_FreeType falhou"); return false; }
    if (FT_New_Face(g_ft, APP_ASSETS "/fonts/DejaVuSans.ttf", 0, &g_faceRegular) != 0) {
        platform::log("Fonte regular não encontrada");
        return false;
    }
    if (FT_New_Face(g_ft, APP_ASSETS "/fonts/DejaVuSans-Bold.ttf", 0, &g_faceBold) != 0)
        g_faceBold = g_faceRegular;
    return true;
}

void shutdown()
{
    SDL_Quit();
}

void beginFrame(Color c)
{
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, 255);
    SDL_RenderClear(g_ren);
}

void flush()
{
    SDL_RenderFlush(g_ren);
}

void endFrame()
{
    SDL_RenderPresent(g_ren);
    SDL_UpdateWindowSurface(g_window);
}

void fillRect(int x, int y, int w, int h, Color c)
{
    if (w <= 0 || h <= 0) return;
    SDL_Rect r = {x, y, w, h};
    SDL_SetRenderDrawBlendMode(g_ren, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(g_ren, &r);
}

void drawRect(int x, int y, int w, int h, int t, Color c)
{
    fillRect(x, y, w, t, c);
    fillRect(x, y + h - t, w, t, c);
    fillRect(x, y + t, t, h - 2 * t, c);
    fillRect(x + w - t, y + t, t, h - 2 * t, c);
}

void fillGradientV(int x, int y, int w, int h, Color a, Color b)
{
    const int steps = 24;
    for (int i = 0; i < steps; i++) {
        int y0 = y + h * i / steps, y1 = y + h * (i + 1) / steps;
        Color c = {
            (uint8_t)(a.r + (b.r - a.r) * i / (steps - 1)),
            (uint8_t)(a.g + (b.g - a.g) * i / (steps - 1)),
            (uint8_t)(a.b + (b.b - a.b) * i / (steps - 1)),
            (uint8_t)(a.a + (b.a - a.a) * i / (steps - 1)),
        };
        fillRect(x, y0, w, y1 - y0, c);
    }
}

void drawTexture(SDL_Texture* tex, int x, int y, int w, int h, uint8_t alpha)
{
    if (!tex) return;
    SDL_Rect dst = {x, y, w, h};
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_RenderCopy(g_ren, tex, NULL, &dst);
}

void drawTextureCover(SDL_Texture* tex, int x, int y, int w, int h, uint8_t alpha)
{
    if (!tex) return;
    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    SDL_Rect src;
    if ((int64_t)tw * h > (int64_t)th * w) { // textura mais larga
        src.h = th;
        src.w = (int)((int64_t)th * w / h);
        src.x = (tw - src.w) / 2;
        src.y = 0;
    } else {
        src.w = tw;
        src.h = (int)((int64_t)tw * h / w);
        src.x = 0;
        src.y = (th - src.h) / 2;
    }
    SDL_Rect dst = {x, y, w, h};
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_RenderCopy(g_ren, tex, &src, &dst);
}

void drawTextureFit(SDL_Texture* tex, int x, int y, int w, int h, uint8_t alpha)
{
    if (!tex) return;
    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    int dw = w, dh = (int)((int64_t)th * w / tw);
    if (dh > h) { dh = h; dw = (int)((int64_t)tw * h / th); }
    drawTexture(tex, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, alpha);
}

static FontCache* font(int size, bool bold)
{
    auto key = std::make_pair(size, bold);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    FontCache* fc = new FontCache();
    fc->face = bold ? g_faceBold : g_faceRegular;
    fc->size = size;
    FT_Set_Pixel_Sizes(fc->face, 0, (FT_UInt)size);
    fc->ascender = (int)(fc->face->size->metrics.ascender >> 6);
    fc->lineHeight = (int)(fc->face->size->metrics.height >> 6);
    g_fonts[key] = fc;
    return fc;
}

static const Glyph& glyph(FontCache* fc, uint32_t cp)
{
    auto it = fc->glyphs.find(cp);
    if (it != fc->glyphs.end()) return it->second;

    Glyph g;
    FT_Face face = fc->face;
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)fc->size);
    FT_UInt idx = FT_Get_Char_Index(face, cp);
    if (idx == 0 && cp > 0x7F) idx = FT_Get_Char_Index(face, '?');
    if (FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT) == 0 && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0) {
        FT_GlyphSlot slot = face->glyph;
        g.w = (int)slot->bitmap.width;
        g.h = (int)slot->bitmap.rows;
        g.left = slot->bitmap_left;
        g.top = slot->bitmap_top;
        g.advance = (int)(slot->advance.x >> 6);
        if (g.w > 0 && g.h > 0) {
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, g.w, g.h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (s) {
                for (int yy = 0; yy < g.h; yy++) {
                    uint32_t* row = (uint32_t*)((uint8_t*)s->pixels + yy * s->pitch);
                    const uint8_t* src = slot->bitmap.buffer + yy * slot->bitmap.pitch;
                    for (int xx = 0; xx < g.w; xx++)
                        row[xx] = ((uint32_t)src[xx] << 24) | 0x00FFFFFFu;
                }
                g.tex = SDL_CreateTextureFromSurface(g_ren, s);
                if (g.tex) SDL_SetTextureBlendMode(g.tex, SDL_BLENDMODE_BLEND);
                SDL_FreeSurface(s);
            }
        }
    }
    return fc->glyphs.emplace(cp, g).first->second;
}

int textWidth(const std::string& s, int size, bool bold)
{
    FontCache* fc = font(size, bold);
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = util::nextCodepoint(s, i);
        if (cp == '\n') continue;
        w += glyph(fc, cp).advance;
    }
    return w;
}

static void drawRun(FontCache* fc, const std::string& s, size_t from, size_t to, int x, int y, Color c)
{
    int baseline = y + fc->ascender;
    size_t i = from;
    while (i < to) {
        uint32_t cp = util::nextCodepoint(s, i);
        const Glyph& g = glyph(fc, cp);
        if (g.tex) {
            SDL_SetTextureColorMod(g.tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(g.tex, c.a);
            SDL_Rect dst = {x + g.left, baseline - g.top, g.w, g.h};
            SDL_RenderCopy(g_ren, g.tex, NULL, &dst);
        }
        x += g.advance;
    }
}

int text(const std::string& sIn, int x, int y, int size, Color c, bool bold, int maxWidth, Align align)
{
    if (sIn.empty()) return 0;
    FontCache* fc = font(size, bold);
    std::string s = sIn;
    int w = textWidth(s, size, bold);
    if (maxWidth > 0 && w > maxWidth) {
        int ell = textWidth("...", size, bold);
        // corta por codepoints até caber
        int acc = 0;
        size_t cut = 0, i = 0;
        while (i < s.size()) {
            uint32_t cp = util::nextCodepoint(s, i);
            int adv = cp == '\n' ? 0 : glyph(fc, cp).advance;
            if (acc + adv + ell > maxWidth) break;
            acc += adv;
            cut = i;
        }
        s = s.substr(0, cut) + "...";
        w = acc + ell;
    }
    int dx = x;
    if (align == Align::Center) dx = x - w / 2;
    else if (align == Align::Right) dx = x - w;
    drawRun(fc, s, 0, s.size(), dx, y, c);
    return w;
}

int textWrapped(const std::string& s, int x, int y, int w, int size, Color c, int maxLines, bool bold, int lineGap)
{
    // quebra gulosa por palavras
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= s.size()) {
        size_t nl = s.find('\n', p);
        std::string para = s.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        std::string line;
        size_t q = 0;
        while (q < para.size()) {
            size_t sp = para.find(' ', q);
            std::string word = para.substr(q, sp == std::string::npos ? std::string::npos : sp - q);
            q = sp == std::string::npos ? para.size() : sp + 1;
            if (word.empty()) continue;
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(cand, size, bold) > w) {
                lines.push_back(line);
                line = word;
            } else {
                line = cand;
            }
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    int lineH = size + lineGap;
    int n = (int)lines.size();
    if (maxLines && n > maxLines) n = maxLines;
    for (int i = 0; i < n; i++) {
        std::string ln = lines[(size_t)i];
        if (maxLines && i == n - 1 && (int)lines.size() > n) ln += " " + lines[(size_t)i + 1] + "...";
        text(ln, x, y + i * lineH, size, c, bold, w);
    }
    return n * lineH;
}

SDL_Texture* textureFromRGBA(const uint8_t* rgba, int w, int h, bool opaque)
{
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom((void*)rgba, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(g_ren, s);
    // imagens opacas sem mistura: cópia direta, bem mais rápida no renderizador por software
    if (t) SDL_SetTextureBlendMode(t, opaque ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    return t;
}

SDL_Texture* loadTextureFile(const std::string& path)
{
    int w, h, n;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return nullptr;
    bool opaque = true;
    for (size_t i = 3; i < (size_t)w * h * 4; i += 4)
        if (px[i] != 255) { opaque = false; break; }
    SDL_Texture* t = textureFromRGBA(px, w, h, opaque);
    stbi_image_free(px);
    return t;
}

} // namespace gfx

// ---------------------------------------------------------------------------
// Carregador de imagens remotas (pôsteres/logos) — substitui o Glide do Android
// ---------------------------------------------------------------------------
namespace images {

struct Decoded {
    std::string key;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool opaque = true;
    bool failed = false;
};

struct Entry {
    SDL_Texture* tex = nullptr;
    bool pending = false;
    bool failed = false;
    uint64_t lastUse = 0;
};

struct Job {
    std::string key, url;
    int w, h;
    bool fit;
};

static std::unordered_map<std::string, Entry> g_cache;
static std::deque<Job> g_queue;
static std::vector<Decoded> g_done;
static std::mutex g_mtx;
static std::condition_variable g_cv;
static const size_t kMaxTextures = 140;

// Reamostra a região (cx,cy,cw,ch) da origem para exatamente dw x dh (média por caixa).
// Gerar a textura no tamanho final evita escala a cada quadro no renderizador por software.
static void resample(const uint8_t* src, int sw, int cx, int cy, int cw, int ch, int dw, int dh, Decoded& out)
{
    out.w = dw;
    out.h = dh;
    out.rgba.resize((size_t)dw * dh * 4);
    out.opaque = true;
    for (int y = 0; y < dh; y++) {
        int y0 = cy + (int)((int64_t)y * ch / dh), y1 = cy + (int)((int64_t)(y + 1) * ch / dh);
        if (y1 <= y0) y1 = y0 + 1;
        int ys = y1 - y0 > 3 ? (y1 - y0) / 3 : 1;
        for (int x = 0; x < dw; x++) {
            int x0 = cx + (int)((int64_t)x * cw / dw), x1 = cx + (int)((int64_t)(x + 1) * cw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            int xs = x1 - x0 > 3 ? (x1 - x0) / 3 : 1;
            unsigned acc[4] = {0, 0, 0, 0}, cnt = 0;
            for (int yy = y0; yy < y1; yy += ys)
                for (int xx = x0; xx < x1; xx += xs) {
                    const uint8_t* p = src + ((size_t)yy * sw + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    cnt++;
                }
            uint8_t* d = &out.rgba[((size_t)y * dw + x) * 4];
            for (int k = 0; k < 4; k++) d[k] = (uint8_t)(acc[k] / cnt);
            if (d[3] != 255) out.opaque = false;
        }
    }
}

static void process(const uint8_t* px, int sw, int sh, const Job& j, Decoded& d)
{
    if (j.fit) {
        // cabe inteiro em w x h mantendo proporção
        int dw = j.w, dh = (int)((int64_t)sh * j.w / sw);
        if (dh > j.h) { dh = j.h; dw = (int)((int64_t)sw * j.h / sh); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        resample(px, sw, 0, 0, sw, sh, dw, dh, d);
    } else {
        // preenche w x h exatamente, cortando o excesso (centro)
        int cw = sw, ch = sh, cx = 0, cy = 0;
        if ((int64_t)sw * j.h > (int64_t)sh * j.w) { cw = (int)((int64_t)sh * j.w / j.h); cx = (sw - cw) / 2; }
        else { ch = (int)((int64_t)sw * j.h / j.w); cy = (sh - ch) / 2; }
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        resample(px, sw, cx, cy, cw, ch, j.w, j.h, d);
    }
}

static void worker()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            job = g_queue.back(); // LIFO: itens visíveis mais recentes primeiro
            g_queue.pop_back();
        }
        Decoded d;
        d.key = job.key;
        std::string data;
        bool got = false;
        if (util::startsWith(job.url, "/")) {
            got = platform::readFile(job.url, data);
        } else {
            http::Response r = http::get(job.url, 12u << 20, 15);
            got = r.ok();
            if (got) data.swap(r.body);
        }
        int w = 0, h = 0, n = 0;
        uint8_t* px = got ? stbi_load_from_memory((const uint8_t*)data.data(), (int)data.size(), &w, &h, &n, 4) : nullptr;
        if (px && w > 0 && h > 0) process(px, w, h, job, d);
        else d.failed = true;
        if (px) stbi_image_free(px);
        std::lock_guard<std::mutex> lk(g_mtx);
        g_done.push_back(std::move(d));
    }
}

void init()
{
    for (int i = 0; i < 3; i++) platform::startThread(worker, 512 * 1024);
}

static void evict()
{
    if (g_cache.size() <= kMaxTextures) return;
    // remove as menos usadas recentemente
    std::vector<std::pair<uint64_t, std::string>> order;
    for (auto& kv : g_cache)
        if (!kv.second.pending) order.push_back({kv.second.lastUse, kv.first});
    std::sort(order.begin(), order.end());
    size_t toRemove = g_cache.size() - kMaxTextures + 20;
    for (size_t i = 0; i < order.size() && i < toRemove; i++) {
        auto it = g_cache.find(order[i].second);
        if (it->second.tex) SDL_DestroyTexture(it->second.tex);
        g_cache.erase(it);
    }
}

SDL_Texture* get(const std::string& url, int w, int h, bool fit)
{
    if (url.empty() || w <= 0 || h <= 0 || (!util::startsWith(url, "http") && !util::startsWith(url, "/"))) return nullptr;
    std::string key = url + (fit ? "#f" : "#c") + std::to_string(w) + "x" + std::to_string(h);
    uint64_t now = platform::ticksMs();
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        it->second.lastUse = now;
        return it->second.tex;
    }
    Entry e;
    e.pending = true;
    e.lastUse = now;
    g_cache[key] = e;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_queue.push_back({key, url, w, h, fit});
        if (g_queue.size() > 80) { // descarta pedidos antigos (já saíram da tela)
            Job old = g_queue.front();
            g_queue.pop_front();
            g_cache.erase(old.key);
        }
    }
    g_cv.notify_one();
    return nullptr;
}

void clearQueue()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& q : g_queue) g_cache.erase(q.key);
    g_queue.clear();
}

int pump()
{
    std::vector<Decoded> done;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        done.swap(g_done);
    }
    int created = 0;
    for (auto& d : done) {
        auto it = g_cache.find(d.key);
        if (it == g_cache.end()) continue;
        it->second.pending = false;
        if (d.failed) { it->second.failed = true; continue; }
        it->second.tex = gfx::textureFromRGBA(d.rgba.data(), d.w, d.h, d.opaque);
        created++;
    }
    if (created) evict();
    return (int)done.size();
}

} // namespace images
