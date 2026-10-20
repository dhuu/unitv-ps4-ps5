#include "ime.h"
#include "platform.h"
#include "util.h"

#include <cstring>
#include <vector>
#ifdef WILLPRO_PS5
// API do teclado do sistema no PS5 (mesma estrutura usada pelo SDL do PS5: SDL_ps5keyboard.c)
extern "C" {
struct Ps5ImeDialogParam {
    int userId;
    int type; // 0 padrão, 1 latino básico, 2 URL, 3 e-mail, 4 número
    uint64_t supportedLanguages;
    int enterLabel;
    int inputMethod;
    void* filter;
    uint32_t option;
    uint32_t maxTextLength;
    uint16_t* inputTextBuffer;
    float posx;
    float posy;
    int halign; // 0 esquerda, 1 centro, 2 direita
    int valign; // 0 topo, 1 centro, 2 base
    const uint16_t* placeholder;
    const uint16_t* title;
    int8_t reserved[16];
};
struct Ps5ImeDialogResult {
    int outcome; // 0 OK, 1 cancelado, 2 abortado
    int8_t reserved[12];
};
int sceImeDialogInit(const Ps5ImeDialogParam*, void*);
int sceImeDialogGetStatus(void); // 0 nenhum, 1 aberto, 2 terminou
int sceImeDialogGetResult(Ps5ImeDialogResult*);
int sceImeDialogTerm(void);
}
#include "platform.h"
#else
#include <orbis/ImeDialog.h>
#endif

namespace ime {

// As bibliotecas do sistema usam wchar_t de 16 bits (UTF-16); o OpenOrbis compila com 32 bits.
static bool g_active = false;
static uint16_t g_buffer[512];
static std::vector<uint16_t> g_title;

static std::vector<uint16_t> toUtf16(const std::string& s)
{
    std::vector<uint16_t> out;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = util::nextCodepoint(s, i);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back((uint16_t)(0xD800 + (cp >> 10)));
            out.push_back((uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((uint16_t)cp);
        }
    }
    out.push_back(0);
    return out;
}

static std::string fromUtf16(const uint16_t* w)
{
    std::string out;
    for (; *w; w++) {
        uint32_t cp = *w;
        if (cp >= 0xD800 && cp <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[1] - 0xDC00);
            w++;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}
static std::function<void(bool, const std::string&)> g_cb;

#ifdef WILLPRO_PS5
static bool g_confirmed = false;   // PS5: Enter enviado pelo SDL ao confirmar
static bool g_seenRunning = false;
static uint64_t g_openedAt = 0;

void noteConfirm()
{
    if (g_active) g_confirmed = true;
}

bool open(const std::string& title, const std::string& initial, Type type,
          std::function<void(bool, const std::string&)> onDone, int maxLen)
{
    if (g_active) return false;
    memset(g_buffer, 0, sizeof(g_buffer));
    std::vector<uint16_t> init = toUtf16(initial);
    if (maxLen > 500) maxLen = 500;
    for (size_t i = 0; i < init.size() && i < (size_t)maxLen; i++) g_buffer[i] = init[i];
    g_title = toUtf16(title);

    Ps5ImeDialogParam s;
    memset(&s, 0, sizeof(s));
    s.userId = platform::userId();
    s.maxTextLength = (uint32_t)maxLen;
    s.inputTextBuffer = g_buffer;
    s.title = g_title.data();
    s.posx = 1920 / 2;
    s.posy = 1080 / 2;
    s.halign = 1;
    s.valign = 1;
    switch (type) {
    case Type::Url: s.type = 2; break;
    case Type::Number: s.type = 4; break;
    case Type::Password:
    case Type::Text: s.type = 1; break; // credenciais: sem acentos/autocorreção
    default: s.type = 0; break;
    }

    int ret = sceImeDialogInit(&s, NULL);
    if (ret < 0) {
        platform::log("sceImeDialogInit 0x%08X", ret);
        return false;
    }
    g_cb = std::move(onDone);
    g_active = true;
    g_confirmed = false;
    g_seenRunning = false;
    g_openedAt = platform::ticksMs();
    return true;
}

bool active() { return g_active; }

void update()
{
    if (!g_active) return;
    int st = sceImeDialogGetStatus();
    bool ok;
    if (st == 1) { g_seenRunning = true; return; }
    if (st == 2) {
        Ps5ImeDialogResult res;
        memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        ok = res.outcome == 0;
        sceImeDialogTerm();
    } else {
        // o SDL já leu o resultado e fechou o teclado (ele manda Enter quando confirmado)
        if (!g_seenRunning && platform::ticksMs() - g_openedAt < 3000) return;
        ok = g_confirmed;
    }
    std::string out = fromUtf16(g_buffer);
    g_active = false;
    auto cb = std::move(g_cb);
    g_cb = nullptr;
    if (cb) cb(ok, util::trim(out));
}

#else
bool open(const std::string& title, const std::string& initial, Type type,
          std::function<void(bool, const std::string&)> onDone, int maxLen)
{
    if (g_active) return false;
    memset(g_buffer, 0, sizeof(g_buffer));
    std::vector<uint16_t> init = toUtf16(initial);
    if (maxLen > 500) maxLen = 500;
    for (size_t i = 0; i < init.size() && i < (size_t)maxLen; i++) g_buffer[i] = init[i];
    g_title = toUtf16(title);

    OrbisImeDialogSetting s;
    memset(&s, 0, sizeof(s));
    s.userId = (uint32_t)platform::userId();
    s.supportedLanguages = 0;
    s.maxTextLength = (uint32_t)maxLen;
    s.inputTextBuffer = (wchar_t*)g_buffer;
    s.title = (const wchar_t*)g_title.data();
    s.posx = 1920 / 2;
    s.posy = 1080 / 2;
    s.horizontalAlignment = ORBIS_H_CENTER;
    s.verticalAlignment = ORBIS_V_CENTER;
    s.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    switch (type) {
    case Type::Url: s.type = ORBIS_TYPE_TYPE_URL; break;
    case Type::Number: s.type = ORBIS_TYPE_NUMBER; break;
    case Type::Password:
    case Type::Text: s.type = ORBIS_TYPE_BASIC_LATIN; break; // credenciais: sem acentos/autocorreção
    default: s.type = ORBIS_TYPE_DEFAULT; break;
    }

    int ret = sceImeDialogInit(&s, NULL);
    if (ret < 0) {
        platform::log("sceImeDialogInit 0x%08X", ret);
        return false;
    }
    g_cb = std::move(onDone);
    g_active = true;
    return true;
}

bool active() { return g_active; }

void update()
{
    if (!g_active) return;
    OrbisDialogStatus st = sceImeDialogGetStatus();
    if (st != ORBIS_DIALOG_STATUS_STOPPED) return;

    OrbisDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    bool ok = res.endstatus == ORBIS_DIALOG_OK;
    std::string out = fromUtf16(g_buffer);
    sceImeDialogTerm();
    g_active = false;
    auto cb = std::move(g_cb);
    g_cb = nullptr;
    if (cb) cb(ok, util::trim(out));
}

void noteConfirm() {}
#endif

} // namespace ime
