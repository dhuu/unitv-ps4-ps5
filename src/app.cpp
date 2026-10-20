#include "ui.h"
#include "config.h"
#include "platform.h"
#include "jobs.h"
#include "ime.h"
#include "storage.h"
#include "util.h"

#include <cmath>

#ifndef WILLPRO_PS5
#include <orbis/SystemService.h>
#endif

namespace app {

static std::vector<std::unique_ptr<Screen>> g_stack;
static std::vector<std::unique_ptr<Screen>> g_graveyard; // telas removidas durante o frame
static bool g_running = true;
static bool g_needEnter = false;
static Branding g_branding;
static bool g_adultUnlocked = false;
static bool g_dirty = true;

void redraw() { g_dirty = true; }

struct Toast {
    std::string msg;
    uint64_t until = 0;
};
static Toast g_toast;

struct Dialog {
    std::string title, msg;
    std::vector<std::string> buttons;
    std::function<void(int)> cb;
    int sel = 0;
};
static std::vector<Dialog> g_dialogs;

Branding& branding() { return g_branding; }

void push(std::unique_ptr<Screen> s)
{
    g_stack.push_back(std::move(s));
    g_needEnter = true;
}

void pop()
{
    if (g_stack.size() <= 1) return;
    g_graveyard.push_back(std::move(g_stack.back()));
    g_stack.pop_back();
    g_needEnter = true;
}

void replaceAll(std::unique_ptr<Screen> s)
{
    for (auto& x : g_stack) g_graveyard.push_back(std::move(x));
    g_stack.clear();
    push(std::move(s));
}

void toast(const std::string& msg)
{
    g_toast.msg = msg;
    g_toast.until = platform::ticksMs() + 3200;
    g_dirty = true;
}

void dialog(const std::string& title, const std::string& msg, const std::vector<std::string>& buttons,
            std::function<void(int)> cb)
{
    Dialog d;
    d.title = title;
    d.msg = msg;
    d.buttons = buttons.empty() ? std::vector<std::string>{"OK"} : buttons;
    d.cb = std::move(cb);
    g_dialogs.push_back(std::move(d));
    g_dirty = true;
}

void message(const std::string& title, const std::string& msg) { dialog(title, msg, {"OK"}); }

bool adultUnlocked() { return g_adultUnlocked; }

void requirePin(std::function<void()> onSuccess)
{
    if (g_adultUnlocked) { onSuccess(); return; }
    ime::open("Senha parental (padrão 0000)", "", ime::Type::Number, [onSuccess](bool ok, const std::string& pin) {
        if (!ok) return;
        if (pin == storage::settings().parentalPin) {
            g_adultUnlocked = true;
            onSuccess();
        } else {
            toast("Senha incorreta");
        }
    }, 8);
}

void quit() { g_running = false; }

static void drawDialog(const Dialog& d)
{
    gfx::fillRect(0, 0, SCREEN_W, SCREEN_H, {0, 0, 0, 160});
    // estimativa de linhas; mensagens longas (diagnóstico) usam fonte menor e caixa mais larga
    int lines = 1, acc = 0;
    for (char c : d.msg) { if (c == '\n') { lines++; acc = 0; } else if (++acc > 60) { lines++; acc = 0; } }
    bool big = lines > 10;
    int w = big ? 1500 : 980;
    int fs = big ? 22 : 28;
    int maxLines = big ? 24 : 12;
    if (lines > maxLines) lines = maxLines;
    int textH = lines * (fs + 8);
    int h = 220 + textH;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    gfx::fillRect(x, y, w, h, col::panel2);
    gfx::fillRect(x, y, w, 6, col::accent);
    gfx::text(d.title, x + 40, y + 36, 38, col::text, true, w - 80);
    gfx::textWrapped(d.msg, x + 40, y + 100, w - 80, fs, col::dim, maxLines);
    int bw = 240, bh = 64, gap = 24;
    int total = (int)d.buttons.size() * bw + ((int)d.buttons.size() - 1) * gap;
    int bx = x + (w - total) / 2, by = y + h - bh - 30;
    for (size_t i = 0; i < d.buttons.size(); i++)
        ui::button(bx + (int)i * (bw + gap), by, bw, bh, d.buttons[i], (int)i == d.sel, i == 0);
}

static void handleDialogInput(Btn b)
{
    Dialog& d = g_dialogs.front();
    int n = (int)d.buttons.size();
    if (b == Btn::Left && d.sel > 0) d.sel--;
    else if (b == Btn::Right && d.sel < n - 1) d.sel++;
    else if (b == Btn::Cross || b == Btn::Circle) {
        int result = b == Btn::Cross ? d.sel : -1;
        auto cb = d.cb;
        g_dialogs.erase(g_dialogs.begin());
        if (cb) cb(result);
    }
}

void run()
{
    uint64_t lastDraw = 0;
    bool toastWasVisible = false;
    bool imeWasActive = false;
    while (g_running) {
        uint64_t frameStart = platform::ticksMs();
        if (jobs::pump()) g_dirty = true;
        if (images::pump()) g_dirty = true;
        ime::update();
        if (ime::active() != imeWasActive) { imeWasActive = ime::active(); g_dirty = true; }

        if (g_needEnter && !g_stack.empty()) {
            g_needEnter = false;
            g_stack.back()->enter();
            g_dirty = true;
        }

        std::vector<Btn> btns = input::poll();
        if (!btns.empty()) g_dirty = true;
        if (!ime::active()) {
            for (Btn b : btns) {
                if (!g_dialogs.empty()) handleDialogInput(b);
                else if (!g_stack.empty()) g_stack.back()->input(b);
                if (g_needEnter) break; // tela mudou: descarta o resto
            }
        }

        if (g_stack.empty()) break;
        Screen* top = g_stack.back().get();
        top->update();

        uint64_t now0 = platform::ticksMs();
        bool toastVisible = now0 < g_toast.until && !g_toast.msg.empty();
        if (toastVisible != toastWasVisible) { toastWasVisible = toastVisible; g_dirty = true; }
        // Renderização por software é cara: só redesenha quando algo mudou (ou o vídeo/relógio pede)
        bool draw = g_dirty || top->drawsVideo() || now0 - lastDraw >= 1000;
        if (!draw) {
            g_graveyard.clear();
            platform::sleepMs(8);
            continue;
        }
        g_dirty = false;
        lastDraw = now0;

        if (top->drawsVideo()) {
            gfx::flush();
        } else {
            gfx::beginFrame(col::bg);
        }
        top->render();

        if (!g_dialogs.empty()) drawDialog(g_dialogs.front());

        uint64_t now = platform::ticksMs();
        if (now < g_toast.until && !g_toast.msg.empty()) {
            int tw = gfx::textWidth(g_toast.msg, 28) + 80;
            int tx = (SCREEN_W - tw) / 2, ty = SCREEN_H - 170;
            gfx::fillRect(tx, ty, tw, 70, {0, 0, 0, 210});
            gfx::fillRect(tx, ty, 6, 70, col::accent);
            gfx::text(g_toast.msg, SCREEN_W / 2, ty + 18, 28, col::text, false, 0, Align::Center);
        }

        gfx::endFrame();
        g_graveyard.clear();

#ifndef WILLPRO_PS5
        // O PS4 mantém a splash do sistema (pic1.png) por cima do app até ela ser escondida
        static bool splashHidden = false;
        if (!splashHidden) {
            splashHidden = true;
            sceSystemServiceHideSplashScreen();
        }
#endif

        // limita a ~60 fps (o flip do SDL pode não esperar o vsync)
        uint64_t spent = platform::ticksMs() - frameStart;
        if (spent < 16) platform::sleepMs((uint32_t)(16 - spent));
    }
}

} // namespace app

// ---------------------------------------------------------------------------
namespace ui {

static SDL_Texture* g_bgLocal = nullptr;

SDL_Texture* logo(int size)
{
    if (!app::branding().logoUrl.empty()) {
        SDL_Texture* t = images::get(app::branding().logoUrl, size, size, true);
        if (t) return t;
    }
    return images::get(APP_ASSETS "/images/uni_logo.png", size, size, true);
}

// Composição em cache: imagem de fundo escalada + escurecimento, numa textura opaca do tamanho da tela.
// Desenhá-la é uma cópia direta (sem escala/mistura), o que é rápido no renderizador por software.
struct Composite {
    SDL_Texture* src;
    uint8_t dim;
    SDL_Texture* tex;
    uint64_t used;
};
static std::vector<Composite> g_comps;

static SDL_Texture* composite(SDL_Texture* src, uint8_t dim)
{
    for (auto& c : g_comps)
        if (c.src == src && c.dim == dim) { c.used = platform::ticksMs(); return c.tex; }
    SDL_Renderer* r = gfx::renderer();
    SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, SCREEN_W, SCREEN_H);
    if (!tex) return nullptr;
    SDL_SetRenderTarget(r, tex);
    gfx::fillRect(0, 0, SCREEN_W, SCREEN_H, col::bg);
    gfx::drawTextureCover(src, 0, 0, SCREEN_W, SCREEN_H);
    if (dim) gfx::fillRect(0, 0, SCREEN_W, SCREEN_H, {6, 8, 18, dim});
    SDL_SetRenderTarget(r, NULL);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    if (g_comps.size() >= 3) { // mantém no máximo 3 (8 MB cada)
        size_t old = 0;
        for (size_t i = 1; i < g_comps.size(); i++) if (g_comps[i].used < g_comps[old].used) old = i;
        SDL_DestroyTexture(g_comps[old].tex);
        g_comps.erase(g_comps.begin() + (long)old);
    }
    g_comps.push_back({src, dim, tex, platform::ticksMs()});
    return tex;
}

static void drawFullscreen(SDL_Texture* src, uint8_t dim)
{
    SDL_Texture* c = composite(src, dim);
    if (c) {
        SDL_RenderCopy(gfx::renderer(), c, NULL, NULL);
    } else { // sem suporte a render target: caminho lento
        gfx::drawTextureCover(src, 0, 0, SCREEN_W, SCREEN_H);
        if (dim) gfx::fillRect(0, 0, SCREEN_W, SCREEN_H, {6, 8, 18, dim});
    }
}

void background(uint8_t dim)
{
    SDL_Texture* bg = nullptr;
    if (!app::branding().backgroundUrl.empty()) bg = images::get(app::branding().backgroundUrl, SCREEN_W, SCREEN_H);
    if (!bg) {
        if (!g_bgLocal) g_bgLocal = gfx::loadTextureFile(APP_ASSETS "/images/bg_main.jpg");
        bg = g_bgLocal;
    }
    if (bg) drawFullscreen(bg, dim);
}

void backdrop(const std::string& url, uint8_t dim)
{
    SDL_Texture* t = images::get(url, SCREEN_W / 2, SCREEN_H / 2);
    if (t) drawFullscreen(t, dim);
    else background(dim);
}

void header(const std::string& title, const std::string& subtitle)
{
    gfx::fillGradientV(0, 0, SCREEN_W, 120, {0, 0, 0, 200}, {0, 0, 0, 0});
    SDL_Texture* lg = logo(90);
    if (lg) gfx::drawTextureFit(lg, 50, 18, 90, 90);
    gfx::text(title, 165, 26, 40, col::text, true, 1200);
    if (!subtitle.empty()) gfx::text(subtitle, 167, 76, 24, col::dim, false, 1200);
    gfx::text(util::formatClock(platform::unixTime()), SCREEN_W - 60, 36, 36, col::text, true, 0, Align::Right);
}

void footer(const std::vector<std::pair<std::string, std::string>>& hints)
{
    int y = SCREEN_H - 62;
    gfx::fillRect(0, y - 10, SCREEN_W, 72, {0, 0, 0, 150});
    int x = 60;
    for (auto& h : hints) {
        Color c = col::accent;
        if (h.first == "O") c = col::danger;
        else if (h.first == "[]") c = {230, 120, 200, 255};
        else if (h.first == "/\\") c = col::ok;
        int bw = gfx::textWidth(h.first, 24, true) + 24;
        gfx::fillRect(x, y + 4, bw, 40, c);
        gfx::text(h.first, x + bw / 2, y + 10, 24, col::white, true, 0, Align::Center);
        x += bw + 12;
        x += gfx::text(h.second, x, y + 9, 26, col::text) + 40;
    }
}

void spinner(int cx, int cy, int r)
{
    app::redraw(); // animação: pede o próximo quadro
    uint64_t t = platform::ticksMs();
    for (int i = 0; i < 10; i++) {
        double a = (double)i / 10.0 * 6.2831853 + (double)(t % 1200) / 1200.0 * 6.2831853;
        int x = cx + (int)(std::cos(a) * r), y = cy + (int)(std::sin(a) * r);
        uint8_t al = (uint8_t)(60 + i * 19);
        gfx::fillRect(x - 5, y - 5, 10, 10, {col::accent.r, col::accent.g, col::accent.b, al});
    }
}

void panel(int x, int y, int w, int h, Color c) { gfx::fillRect(x, y, w, h, c); }

void focusFrame(int x, int y, int w, int h)
{
    gfx::drawRect(x - 5, y - 5, w + 10, h + 10, 5, col::focus);
}

void button(int x, int y, int w, int h, const std::string& label, bool focused, bool primary)
{
    Color bg = focused ? col::accent : (primary ? Color{40, 50, 90, 255} : Color{36, 42, 70, 255});
    gfx::fillRect(x, y, w, h, bg);
    if (focused) gfx::drawRect(x - 3, y - 3, w + 6, h + 6, 3, col::white);
    gfx::text(label, x + w / 2, y + (h - 30) / 2, 28, col::white, true, w - 20, Align::Center);
}

void poster(const std::string& url, int x, int y, int w, int h, const std::string& fallbackText, bool cover)
{
    SDL_Texture* t = images::get(url, w, h, !cover);
    if (t) {
        if (!cover) gfx::fillRect(x, y, w, h, {15, 18, 30, 255});
        if (cover) gfx::drawTexture(t, x, y, w, h);
        else gfx::drawTextureFit(t, x, y, w, h);
        return;
    }
    gfx::fillGradientV(x, y, w, h, {40, 48, 84, 255}, {20, 24, 44, 255});
    std::string initials;
    size_t i = 0;
    int n = 0;
    while (i < fallbackText.size() && n < 2) {
        size_t st = i;
        util::nextCodepoint(fallbackText, i);
        if (fallbackText[st] != ' ') { initials += fallbackText.substr(st, i - st); n++; }
    }
    int fs = h > 120 ? 48 : 26;
    gfx::text(initials, x + w / 2, y + h / 2 - fs / 2 - 4, fs, col::dim, true, w - 10, Align::Center);
}

void ListNav::clamp(int count, int visible)
{
    if (count <= 0) { sel = 0; top = 0; return; }
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + visible) top = sel - visible + 1;
    if (top < 0) top = 0;
}

bool ListNav::move(int delta, int count, int visible)
{
    if (count <= 0) return false;
    int old = sel;
    sel += delta;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;
    clamp(count, visible);
    return sel != old;
}

void GridNav::clamp(int count, int visibleRows)
{
    if (count <= 0) { sel = 0; topRow = 0; return; }
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    int row = sel / cols;
    if (row < topRow) topRow = row;
    if (row >= topRow + visibleRows) topRow = row - visibleRows + 1;
}

bool GridNav::move(int dx, int dy, int count, int visibleRows)
{
    if (count <= 0) return dx >= 0;
    if (dx < 0 && sel % cols == 0) return false;
    if (dx > 0 && (sel % cols == cols - 1 || sel + 1 >= count)) { return true; }
    int n = sel + dx + dy * cols;
    if (dy > 0 && n >= count) {
        // última linha parcial: vai para o último item se houver linha abaixo
        if (sel / cols < (count - 1) / cols) n = count - 1;
        else n = sel;
    }
    if (n < 0) n = sel;
    sel = n;
    clamp(count, visibleRows);
    return true;
}

} // namespace ui
