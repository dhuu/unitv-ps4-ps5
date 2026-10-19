#pragma once
// Núcleo da interface: pilha de telas, diálogos modais, avisos e componentes reutilizáveis
#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "gfx.h"
#include "input.h"

class Screen {
protected:
    // Callbacks assíncronos verificam *alive_ antes de usar "this" (a tela pode ter sido fechada)
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

public:
    virtual ~Screen() { *alive_ = false; }
    virtual void enter() {}                 // ao ficar no topo da pilha
    virtual void input(Btn b) = 0;
    virtual void update() {}
    virtual void render() = 0;
    virtual bool drawsVideo() const { return false; } // tela do player (não limpa o fundo)
};

namespace app {

void push(std::unique_ptr<Screen> s);
void pop();
void replaceAll(std::unique_ptr<Screen> s);   // limpa a pilha
void toast(const std::string& msg);
// Diálogo modal com botões. cb(índice) — -1 se cancelado com Círculo
void dialog(const std::string& title, const std::string& msg, const std::vector<std::string>& buttons,
            std::function<void(int)> cb = nullptr);
void message(const std::string& title, const std::string& msg);
// Pede a senha parental (padrão 0000) antes de abrir conteúdo adulto
void requirePin(std::function<void()> onSuccess);
bool adultUnlocked();

void run();    // laço principal
// Pede um novo desenho da tela (a interface só redesenha quando algo muda)
void redraw();
void quit();

// Branding vindo do painel (get_config)
struct Branding {
    std::string logoUrl, backgroundUrl, message, whatsapp, eventsUrl, newsBanner;
};
Branding& branding();

} // namespace app

namespace ui {

// Fundo padrão (imagem do painel ou bg_main.jpg) com escurecimento — composto uma vez e guardado em cache
void background(uint8_t dim = 150);
// Fundo de tela cheia a partir de uma imagem remota (detalhes de filme/série); cai no background() se faltar
void backdrop(const std::string& url, uint8_t dim);
// Barra superior com logo, título e relógio
void header(const std::string& title, const std::string& subtitle = "");
// Rodapé com dicas de botões, ex.: {{"X","Abrir"},{"O","Voltar"}}
void footer(const std::vector<std::pair<std::string, std::string>>& hints);
void spinner(int cx, int cy, int r = 26);
void button(int x, int y, int w, int h, const std::string& label, bool focused, bool primary = false);
void panel(int x, int y, int w, int h, Color c = col::panel);
void focusFrame(int x, int y, int w, int h);
// Imagem remota com fallback (retângulo + iniciais)
void poster(const std::string& url, int x, int y, int w, int h, const std::string& fallbackText, bool cover = true);
SDL_Texture* logo(int size);

// Lista vertical com rolagem
struct ListNav {
    int sel = 0;
    int top = 0;
    void clamp(int count, int visible);
    bool move(int delta, int count, int visible); // true se mudou
};

// Grade com rolagem por linha
struct GridNav {
    int sel = 0;
    int topRow = 0;
    int cols = 5;
    void clamp(int count, int visibleRows);
    bool move(int dx, int dy, int count, int visibleRows); // false se saiu pela borda esquerda
    bool atLeftEdge() const { return sel % cols == 0; }
};

} // namespace ui
