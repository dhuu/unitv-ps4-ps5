#pragma once
// Renderização 2D (SDL2 software renderer) + texto FreeType + imagens remotas assíncronas
#include <string>
#include <cstdint>
#include <SDL2/SDL.h>

struct Color {
    uint8_t r, g, b, a;
};

namespace col {
static const Color bg        = {10, 12, 24, 255};
static const Color panel     = {20, 24, 44, 235};
static const Color panel2    = {30, 36, 64, 245};
static const Color text      = {240, 242, 250, 255};
static const Color dim       = {150, 158, 185, 255};
static const Color accent    = {0, 168, 255, 255};
static const Color accent2   = {255, 196, 0, 255};
static const Color focus     = {0, 168, 255, 255};
static const Color danger    = {230, 60, 70, 255};
static const Color ok        = {40, 200, 120, 255};
static const Color black     = {0, 0, 0, 255};
static const Color shade     = {0, 0, 0, 170};
static const Color white     = {255, 255, 255, 255};
}

enum class Align { Left, Center, Right };

namespace gfx {

bool init();
void shutdown();
SDL_Renderer* renderer();
SDL_Surface* screen();

void beginFrame(Color clear = col::bg);
void endFrame();
void flush();

void fillRect(int x, int y, int w, int h, Color c);
void drawRect(int x, int y, int w, int h, int thickness, Color c);
void fillGradientV(int x, int y, int w, int h, Color top, Color bottom);
void drawTexture(SDL_Texture* tex, int x, int y, int w, int h, uint8_t alpha = 255);
// Preenche o retângulo mantendo proporção (corta o excesso)
void drawTextureCover(SDL_Texture* tex, int x, int y, int w, int h, uint8_t alpha = 255);
// Cabe inteiro no retângulo mantendo proporção
void drawTextureFit(SDL_Texture* tex, int x, int y, int w, int h, uint8_t alpha = 255);

// Texto UTF-8. Retorna a largura desenhada. maxWidth > 0 corta com "..."
int text(const std::string& s, int x, int y, int size, Color c, bool bold = false, int maxWidth = 0,
         Align align = Align::Left);
int textWidth(const std::string& s, int size, bool bold = false);
// Texto com quebra de linha; retorna altura usada
int textWrapped(const std::string& s, int x, int y, int w, int size, Color c, int maxLines = 0,
                bool bold = false, int lineGap = 8);

SDL_Texture* loadTextureFile(const std::string& path);
SDL_Texture* textureFromRGBA(const uint8_t* rgba, int w, int h, bool opaque = false);

} // namespace gfx

namespace images {
// Retorna a textura (já no tamanho final) se estiver carregada; senão agenda o download e retorna nullptr.
// fit=false: exatamente w x h cortando o excesso; fit=true: cabe dentro de w x h mantendo proporção.
// Aceita URL http(s) ou caminho local ("/app0/...").
SDL_Texture* get(const std::string& url, int w, int h, bool fit = false);
int pump();         // chamar na thread principal a cada frame; retorna quantas imagens terminaram
void clearQueue();  // descarta pedidos pendentes (ao trocar de tela)
void init();
}
