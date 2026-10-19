#include "input.h"
#include "platform.h"
#include "ime.h"

#include <SDL2/SDL.h>

namespace input {

#ifdef UNITV_PS5
// Índices de botão do port SDL2 do PS5 (ps5-payload-dev/SDL, src/joystick/ps5/SDL_ps5joystick.c)
enum {
    PAD_CROSS = 0, PAD_CIRCLE = 1, PAD_SQUARE = 2, PAD_TRIANGLE = 3,
    PAD_TOUCH = 4, PAD_OPTIONS = 6, PAD_L3 = 7, PAD_R3 = 8, PAD_L1 = 9, PAD_R1 = 10,
    PAD_UP = 11, PAD_DOWN = 12, PAD_LEFT = 13, PAD_RIGHT = 14,
    PAD_L2 = 15, PAD_R2 = 16,
};
#else
// Índices de botão do port SDL2 do OpenOrbis (samples/SDL2/Game.h)
enum {
    PAD_CROSS = 0, PAD_CIRCLE = 1, PAD_SQUARE = 2, PAD_TRIANGLE = 3,
    PAD_L1 = 4, PAD_R1 = 5, PAD_OPTIONS = 9, PAD_L3 = 11, PAD_R3 = 12,
    PAD_UP = 13, PAD_DOWN = 14, PAD_LEFT = 15, PAD_RIGHT = 16,
    PAD_TOUCH = 17, PAD_L2 = 18, PAD_R2 = 19,
};
#endif
static const int kMaxButtons = 32;

static SDL_Joystick* g_joy = nullptr;
static bool g_prevBtn[kMaxButtons] = {false};

// Direções só pelo direcional (o analógico é ignorado). Os toques vêm da fila de
// eventos do SDL, então nenhum se perde mesmo quando um quadro demora a desenhar.
// O SDL informa o direcional de dois jeitos (botões PAD_UP..PAD_RIGHT e hat): assim que
// chega o primeiro evento de botão do direcional, o hat passa a ser ignorado, para
// cada toque contar uma vez só.
enum { SRC_BTN = 0, SRC_HAT = 1 };
static bool g_dpadButtons = false; // já chegaram eventos de botão do direcional
static Btn g_hatLast = Btn::None;
static Btn g_held = Btn::None;     // direção que está repetindo
static int g_heldSrc = -1;         // fonte que iniciou a repetição
static uint64_t g_nextRepeat = 0;

void init()
{
    if (SDL_NumJoysticks() > 0) g_joy = SDL_JoystickOpen(0);
    if (!g_joy) platform::log("Nenhum controle encontrado");
    else platform::log("Controle: %d botões, %d hats, %d eixos", SDL_JoystickNumButtons(g_joy),
                       SDL_JoystickNumHats(g_joy), SDL_JoystickNumAxes(g_joy));
}

static Btn mapButton(int b)
{
    switch (b) {
    case PAD_CROSS: return Btn::Cross;
    case PAD_CIRCLE: return Btn::Circle;
    case PAD_SQUARE: return Btn::Square;
    case PAD_TRIANGLE: return Btn::Triangle;
    case PAD_L1: return Btn::L1;
    case PAD_R1: return Btn::R1;
    case PAD_L2: return Btn::L2;
    case PAD_R2: return Btn::R2;
    case PAD_OPTIONS: return Btn::Options;
    case PAD_TOUCH: return Btn::Touchpad;
    default: return Btn::None;
    }
}

static const char* dirName(Btn d)
{
    switch (d) {
    case Btn::Up: return "cima";
    case Btn::Down: return "baixo";
    case Btn::Left: return "esq";
    case Btn::Right: return "dir";
    default: return "-";
    }
}

static Btn hatDir(Uint8 h)
{
    if (h & SDL_HAT_UP) return Btn::Up;
    if (h & SDL_HAT_DOWN) return Btn::Down;
    if (h & SDL_HAT_LEFT) return Btn::Left;
    if (h & SDL_HAT_RIGHT) return Btn::Right;
    return Btn::None;
}

static Btn dpadButtonDir(int b)
{
    switch (b) {
    case PAD_UP: return Btn::Up;
    case PAD_DOWN: return Btn::Down;
    case PAD_LEFT: return Btn::Left;
    case PAD_RIGHT: return Btn::Right;
    default: return Btn::None;
    }
}

// Direção que a fonte informa agora (para a auto-repetição)
static Btn heldNow(int src)
{
    if (!g_joy) return Btn::None;
    if (src == SRC_HAT) return SDL_JoystickNumHats(g_joy) > 0 ? hatDir(SDL_JoystickGetHat(g_joy, 0)) : Btn::None;
    if (SDL_JoystickNumButtons(g_joy) <= PAD_RIGHT) return Btn::None;
    for (int b = PAD_UP; b <= PAD_RIGHT; b++)
        if (SDL_JoystickGetButton(g_joy, b)) return dpadButtonDir(b);
    return Btn::None;
}

std::vector<Btn> poll()
{
    std::vector<Btn> out;
    uint64_t now = platform::ticksMs();
    auto press = [&](Btn d, int src) {
        out.push_back(d);
        g_held = d;
        g_heldSrc = src;
        g_nextRepeat = now + 380;
    };
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_JOYDEVICEADDED && !g_joy) {
            g_joy = SDL_JoystickOpen(0);
            for (bool& b : g_prevBtn) b = false;
        } else if (ev.type == SDL_JOYBUTTONDOWN) {
            Btn d = dpadButtonDir(ev.jbutton.button);
            if (d == Btn::None) continue;
            platform::log("direcional: botao %s", dirName(d));
            if (!g_dpadButtons) {
                g_dpadButtons = true;
                platform::log("direcional: usando botoes, hat ignorado");
                // o mesmo toque já pode ter chegado pelo hat neste quadro
                if (!out.empty() && out.back() == d && g_heldSrc == SRC_HAT) {
                    g_heldSrc = SRC_BTN;
                    continue;
                }
            }
            press(d, SRC_BTN);
        } else if (ev.type == SDL_KEYDOWN &&
                   (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER)) {
            ime::noteConfirm(); // PS5: o SDL confirma o teclado do sistema com um Enter
        } else if (ev.type == SDL_JOYHATMOTION && ev.jhat.hat == 0) {
            Btn d = hatDir(ev.jhat.value);
            bool isNew = d != Btn::None && d != g_hatLast;
            g_hatLast = d;
            if (!isNew || g_dpadButtons) continue;
            platform::log("direcional: hat %s", dirName(d));
            press(d, SRC_HAT);
        }
    }
    if (!g_joy) return out;
    SDL_JoystickUpdate();

    // botões de ação: borda de subida (apertou agora)
    int nb = SDL_JoystickNumButtons(g_joy);
    if (nb > kMaxButtons) nb = kMaxButtons;
    for (int i = 0; i < nb; i++) {
        bool down = SDL_JoystickGetButton(g_joy, i) != 0;
        if (down && !g_prevBtn[i]) {
            Btn b = mapButton(i);
            if (b != Btn::None) out.push_back(b);
        }
        g_prevBtn[i] = down;
    }

    // auto-repetição enquanto a fonte que iniciou continua segurando
    if (g_held != Btn::None) {
        if (heldNow(g_heldSrc) != g_held) {
            g_held = Btn::None;
            g_heldSrc = -1;
        } else if (now >= g_nextRepeat) {
            out.push_back(g_held);
            g_nextRepeat = now + 85;
        }
    }
    return out;
}

} // namespace input
