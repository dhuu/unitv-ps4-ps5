// Home, Busca, Favoritos/Histórico e Configurações
// (MainActivity.kt, SearchActivity.kt, HistoryActivity.kt, SettingsActivity.kt)
#include "screens.h"
#include "config.h"
#include "platform.h"
#include "jobs.h"
#include "ime.h"
#include "storage.h"
#include "catalog.h"
#include "util.h"

#include <ctime>

void drawKindBadge(Kind k, int x, int y)
{
    const char* t = k == Kind::Live ? "AO VIVO" : (k == Kind::Series ? "SÉRIE" : "FILME");
    Color c = k == Kind::Live ? col::danger : (k == Kind::Series ? Color{140, 80, 220, 255} : Color{0, 130, 210, 255});
    int w = gfx::textWidth(t, 18, true) + 16;
    gfx::fillRect(x, y, w, 30, c);
    gfx::text(t, x + 8, y + 4, 18, col::white, true);
}

void openStream(const Stream& s)
{
    if (s.kind == Kind::Live) {
        PlayItem p;
        p.kind = Kind::Live;
        p.id = s.id;
        p.name = s.name;
        p.icon = s.icon;
        startPlayback({p}, 0);
    } else if (s.kind == Kind::Series) {
        app::push(makeSeriesDetails(s));
    } else {
        app::push(makeMovieDetails(s));
    }
}

static std::string formatExp(const std::string& exp)
{
    if (exp.empty() || exp == "null") return "Ilimitado";
    time_t t = (time_t)atoll(exp.c_str());
    if (t <= 0) return exp;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
    return buf;
}

// ---------------------------------------------------------------------------
class HomeScreen : public Screen {
    struct Tile {
        std::string label, sub;
        Color c1, c2;
        std::string image;
        std::function<void()> action;
    };
    std::vector<Tile> tiles_;
    int sel_ = 0;
    bool shownMsg_ = false;

public:
    HomeScreen()
    {
        tiles_ = {
            {"TV AO VIVO", "Canais e esportes", {220, 40, 70, 255}, {120, 10, 40, 255}, "",
             [] { app::push(makeBrowse(Kind::Live, BrowseFilter::All)); }},
            {"FILMES", "Lançamentos e catálogo", {0, 140, 230, 255}, {0, 50, 120, 255}, "",
             [] { app::push(makeBrowse(Kind::Vod, BrowseFilter::All)); }},
            {"SÉRIES", "Temporadas completas", {140, 70, 220, 255}, {60, 20, 120, 255}, "tile_series.png",
             [] { app::push(makeBrowse(Kind::Series, BrowseFilter::All)); }},
            {"KIDS", "Infantil e animação", {255, 170, 0, 255}, {200, 90, 0, 255}, "tile_kids.jpg",
             [] { app::push(makeBrowse(Kind::Vod, BrowseFilter::Kids)); }},
            {"ADULTOS", "Protegido por senha", {90, 20, 40, 255}, {40, 10, 20, 255}, "tile_adult.png",
             [] { app::requirePin([] { app::push(makeBrowse(Kind::Vod, BrowseFilter::Adult)); }); }},
            {"BUSCAR", "Filmes, séries e canais", {20, 160, 140, 255}, {10, 70, 70, 255}, "",
             [] { app::push(makeSearch()); }},
            {"FAVORITOS", "Seus itens salvos", {230, 80, 150, 255}, {110, 20, 70, 255}, "",
             [] { app::push(makeSaved(true)); }},
            {"CONTINUAR", "Histórico recente", {60, 180, 90, 255}, {20, 80, 40, 255}, "",
             [] { app::push(makeSaved(false)); }},
            {"SUPORTE", "Fale com a gente", {70, 90, 130, 255}, {30, 40, 70, 255}, "",
             [] {
                 std::string w = app::branding().whatsapp;
                 app::message("Suporte", w.empty() ? "Entre em contato com o seu revendedor." : ("WhatsApp: " + w));
             }},
            {"AJUSTES", "Conta, DNS e senha", {90, 100, 120, 255}, {40, 44, 60, 255}, "",
             [] { app::push(makeSettings()); }},
        };

    }

    void enter() override
    {
        if (!shownMsg_) {
            shownMsg_ = true;
            if (!app::branding().message.empty()) app::message("Aviso", app::branding().message);
        }
    }

    void input(Btn b) override
    {
        int n = (int)tiles_.size(), cols = 5;
        switch (b) {
        case Btn::Left: if (sel_ % cols > 0) sel_--; break;
        case Btn::Right: if (sel_ % cols < cols - 1 && sel_ + 1 < n) sel_++; break;
        case Btn::Up: if (sel_ >= cols) sel_ -= cols; break;
        case Btn::Down: if (sel_ + cols < n) sel_ += cols; break;
        case Btn::Cross: tiles_[(size_t)sel_].action(); break;
        case Btn::Options: app::push(makeSettings()); break;
        case Btn::Circle:
            app::dialog("Sair", "Deseja fechar o " APP_NAME "?", {"Sim", "Não"}, [](int r) {
                if (r == 0) app::quit();
            });
            break;
        default: break;
        }
    }

    void render() override
    {
        ui::background(140);
        std::string sub = "Usuário: " + api::session().user + "   |   Vencimento: " + formatExp(g_account.expDate);
        ui::header(APP_NAME, sub);

        int y0 = 190;
        const std::string& msg = app::branding().message;
        if (!msg.empty()) {
            gfx::fillRect(60, 150, SCREEN_W - 120, 56, {0, 0, 0, 150});
            gfx::fillRect(60, 150, 8, 56, col::accent2);
            gfx::text(msg, 90, 162, 26, col::text, false, SCREEN_W - 200);
            y0 = 240;
        }

        int cols = 5, tw = 330, th = 300, gap = 28;
        int total = cols * tw + (cols - 1) * gap;
        int x0 = (SCREEN_W - total) / 2;
        for (size_t i = 0; i < tiles_.size(); i++) {
            int c = (int)i % cols, r = (int)i / cols;
            int x = x0 + c * (tw + gap), y = y0 + r * (th + gap + 10);
            const Tile& t = tiles_[i];
            gfx::fillGradientV(x, y, tw, th, t.c1, t.c2);
            if (!t.image.empty()) {
                SDL_Texture* img = images::get(std::string(APP_ASSETS "/images/") + t.image, tw, th);
                if (img) gfx::drawTexture(img, x, y, tw, th);
            }
            gfx::fillGradientV(x, y + th - 130, tw, 130, {0, 0, 0, 0}, {0, 0, 0, 200});
            gfx::text(t.label, x + 22, y + th - 96, 36, col::white, true, tw - 40);
            gfx::text(t.sub, x + 22, y + th - 48, 22, {220, 225, 240, 255}, false, tw - 40);
            if ((int)i == sel_) ui::focusFrame(x, y, tw, th);
        }
        ui::footer({{"X", "Abrir"}, {"OPTIONS", "Ajustes"}, {"O", "Sair"}});
    }
};

std::unique_ptr<Screen> makeHome() { return std::unique_ptr<Screen>(new HomeScreen()); }

// ---------------------------------------------------------------------------
// Grade de pôsteres compartilhada por Busca e Favoritos/Histórico
static void drawPosterGrid(const std::vector<Stream>& items, const ui::GridNav& nav, int x0, int y0, int rows,
                           bool focused)
{
    int cols = nav.cols, pw = 230, ph = 330, gapX = 36, gapY = 70;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = (nav.topRow + r) * cols + c;
            if (i >= (int)items.size()) return;
            const Stream& s = items[(size_t)i];
            int x = x0 + c * (pw + gapX), y = y0 + r * (ph + gapY);
            if (s.kind == Kind::Live) {
                gfx::fillRect(x, y, pw, ph, {24, 28, 50, 255});
                ui::poster(s.icon, x + 20, y + 60, pw - 40, pw - 40, s.name, false);
            } else {
                ui::poster(s.icon, x, y, pw, ph, s.name);
            }
            drawKindBadge(s.kind, x + 8, y + 8);
            gfx::text(s.name, x, y + ph + 10, 22, i == nav.sel && focused ? col::text : col::dim, i == nav.sel, pw);
            if (i == nav.sel && focused) ui::focusFrame(x, y, pw, ph);
        }
    }
}

class SearchScreen : public Screen {
    std::string query_;
    std::vector<Stream> results_;
    ui::GridNav nav_;
    bool pending_ = false;   // aguardando listas completas
    bool askedOnce_ = false;

public:
    SearchScreen() { nav_.cols = 6; }

    void enter() override
    {
        if (!askedOnce_) { askedOnce_ = true; ask(); }
    }

    void ask()
    {
        auto alive = alive_;
        ime::open("Buscar filmes, séries e canais", query_, ime::Type::Search, [this, alive](bool ok, const std::string& q) {
            if (!*alive || !ok) return;
            query_ = q;
            pending_ = !query_.empty();
            results_.clear();
            nav_ = ui::GridNav();
            nav_.cols = 6;
        });
    }

    void update() override
    {
        if (!pending_) return;
        catalog::Load s1, s2, s3;
        const std::vector<Stream>* vod = catalog::all(Kind::Vod, &s1);
        const std::vector<Stream>* ser = catalog::all(Kind::Series, &s2);
        const std::vector<Stream>* live = catalog::all(Kind::Live, &s3);
        bool loading = s1 == catalog::Load::Loading || s2 == catalog::Load::Loading || s3 == catalog::Load::Loading;
        if (loading) return;
        pending_ = false;
        std::string q = util::normalize(query_);
        results_.clear();
        auto scan = [&](const std::vector<Stream>* list) {
            if (!list) return;
            for (auto& s : *list) {
                if (results_.size() >= 300) return;
                std::string cat = catalog::categoryName(s.kind, s.categoryId);
                if (!app::adultUnlocked() && util::isAdultCategory(cat)) continue;
                if (util::containsNormalized(s.name, q)) results_.push_back(s);
            }
        };
        scan(vod);
        scan(ser);
        scan(live);
    }

    void input(Btn b) override
    {
        int n = (int)results_.size();
        switch (b) {
        case Btn::Left: nav_.move(-1, 0, n, 2); break;
        case Btn::Right: nav_.move(1, 0, n, 2); break;
        case Btn::Up: nav_.move(0, -1, n, 2); break;
        case Btn::Down: nav_.move(0, 1, n, 2); break;
        case Btn::Cross: if (n) openStream(results_[(size_t)nav_.sel]); break;
        case Btn::Triangle: ask(); break;
        case Btn::Circle: app::pop(); break;
        default: break;
        }
    }

    void render() override
    {
        ui::background(200);
        ui::header("Buscar", query_.empty() ? "Pressione Triângulo para digitar" : ("Resultados para \"" + query_ + "\""));
        if (pending_) {
            ui::spinner(SCREEN_W / 2, SCREEN_H / 2);
            gfx::text("Carregando catálogo completo...", SCREEN_W / 2, SCREEN_H / 2 + 60, 28, col::dim, false, 0, Align::Center);
        } else if (results_.empty()) {
            gfx::text(query_.empty() ? "Digite um termo para buscar" : "Nenhum resultado encontrado", SCREEN_W / 2,
                      SCREEN_H / 2, 30, col::dim, false, 0, Align::Center);
        } else {
            drawPosterGrid(results_, nav_, 120, 170, 2, true);
            gfx::text(std::to_string(nav_.sel + 1) + " / " + std::to_string(results_.size()), SCREEN_W - 60, 120, 24,
                      col::dim, false, 0, Align::Right);
        }
        ui::footer({{"X", "Abrir"}, {"/\\", "Nova busca"}, {"O", "Voltar"}});
    }
};

std::unique_ptr<Screen> makeSearch() { return std::unique_ptr<Screen>(new SearchScreen()); }

// ---------------------------------------------------------------------------
class SavedScreen : public Screen {
    bool favs_;
    std::vector<Stream> items_;
    ui::GridNav nav_;

    void reload()
    {
        items_.clear();
        for (auto& it : favs_ ? storage::favorites() : storage::history()) {
            Stream s;
            s.kind = it.kind == Kind::Episode ? Kind::Series : it.kind;
            s.id = it.id;
            s.name = it.name;
            s.icon = it.icon;
            s.ext = it.ext;
            items_.push_back(s);
        }
        nav_.clamp((int)items_.size(), 2);
    }

public:
    explicit SavedScreen(bool favs) : favs_(favs) { nav_.cols = 6; }

    void enter() override { reload(); }

    void input(Btn b) override
    {
        int n = (int)items_.size();
        switch (b) {
        case Btn::Left: nav_.move(-1, 0, n, 2); break;
        case Btn::Right: nav_.move(1, 0, n, 2); break;
        case Btn::Up: nav_.move(0, -1, n, 2); break;
        case Btn::Down: nav_.move(0, 1, n, 2); break;
        case Btn::Cross:
            if (!n) break;
            if (favs_ && items_[(size_t)nav_.sel].kind == Kind::Live) {
                // favoritos ao vivo: playlist com todos os canais favoritos (zapping)
                std::vector<PlayItem> list;
                int idx = 0;
                for (size_t i = 0; i < items_.size(); i++) {
                    if (items_[i].kind != Kind::Live) continue;
                    if ((int)i == nav_.sel) idx = (int)list.size();
                    PlayItem p;
                    p.kind = Kind::Live;
                    p.id = items_[i].id;
                    p.name = items_[i].name;
                    p.icon = items_[i].icon;
                    list.push_back(p);
                }
                startPlayback(list, idx);
            } else {
                openStream(items_[(size_t)nav_.sel]);
            }
            break;
        case Btn::Square:
            if (favs_ && n) {
                SavedItem it;
                it.kind = items_[(size_t)nav_.sel].kind;
                it.id = items_[(size_t)nav_.sel].id;
                storage::toggleFavorite(it);
                app::toast("Removido dos favoritos");
                reload();
            }
            break;
        case Btn::Triangle:
            if (!favs_ && n)
                app::dialog("Histórico", "Limpar todo o histórico?", {"Sim", "Não"}, [this, alive = alive_](int r) {
                    if (r == 0 && *alive) { storage::clearHistory(); reload(); }
                });
            break;
        case Btn::Circle: app::pop(); break;
        default: break;
        }
    }

    void render() override
    {
        ui::background(200);
        ui::header(favs_ ? "Favoritos" : "Continuar assistindo", std::to_string(items_.size()) + " itens");
        if (items_.empty())
            gfx::text(favs_ ? "Nenhum favorito ainda. Use Quadrado para favoritar." : "Nada assistido ainda.",
                      SCREEN_W / 2, SCREEN_H / 2, 30, col::dim, false, 0, Align::Center);
        else
            drawPosterGrid(items_, nav_, 120, 170, 2, true);
        if (favs_) ui::footer({{"X", "Abrir"}, {"[]", "Remover"}, {"O", "Voltar"}});
        else ui::footer({{"X", "Abrir"}, {"/\\", "Limpar"}, {"O", "Voltar"}});
    }
};

std::unique_ptr<Screen> makeSaved(bool favorites) { return std::unique_ptr<Screen>(new SavedScreen(favorites)); }

// ---------------------------------------------------------------------------
class SettingsScreen : public Screen {
    ui::ListNav nav_;
    bool reloading_ = false;

    struct Row { std::string label, value; std::function<void()> action; };
    std::vector<Row> rows()
    {
        Settings& s = storage::settings();
        std::vector<Row> r;
        r.push_back({"Usuário", api::session().user, nullptr});
        r.push_back({"Status da conta", g_account.status.empty() ? "-" : g_account.status, nullptr});
        r.push_back({"Vencimento", formatExp(g_account.expDate), nullptr});
        r.push_back({"Endereço MAC", platform::macAddress(), nullptr});
        r.push_back({"Servidor (DNS)", api::session().dns, [this] { nextDns(); }});
        r.push_back({"Formato da TV ao vivo", s.liveFormat == "m3u8" ? "HLS (.m3u8)" : "MPEG-TS (.ts) — recomendado", [] {
                         Settings& st = storage::settings();
                         st.liveFormat = st.liveFormat == "m3u8" ? "ts" : "m3u8";
                         storage::save();
                     }});
        r.push_back({"Alterar senha parental", "****", [] { changePin(); }});
        r.push_back({"Limpar histórico", "", [] { storage::clearHistory(); app::toast("Histórico limpo"); }});
        r.push_back({"Recarregar listas", reloading_ ? "Carregando..." : "", [this] { reloadLists(); }});
        r.push_back({"Sair da conta", "", [] {
                         app::dialog("Sair da conta", "Deseja sair? Favoritos e histórico serão apagados.", {"Sair", "Cancelar"},
                                     [](int x) { if (x == 0) logout(); });
                     }});
        r.push_back({"Versão", APP_VERSION, nullptr});
        return r;
    }

    void nextDns()
    {
        Settings& s = storage::settings();
        if (s.dnsList.size() < 2) { app::toast("Apenas um servidor disponível"); return; }
        size_t i = 0;
        std::string cur = api::normalizeDns(api::session().dns);
        for (size_t k = 0; k < s.dnsList.size(); k++)
            if (api::normalizeDns(s.dnsList[k]) == cur) i = k + 1;
        std::string next = api::normalizeDns(s.dnsList[i % s.dnsList.size()]);
        api::session().dns = next;
        s.dns = next;
        storage::save();
        app::toast("Servidor alterado");
    }

    static void changePin()
    {
        ime::open("Senha parental atual", "", ime::Type::Number, [](bool ok, const std::string& old) {
            if (!ok) return;
            if (old != storage::settings().parentalPin) { app::toast("Senha atual incorreta."); return; }
            ime::open("Nova senha (mínimo 4 dígitos)", "", ime::Type::Number, [](bool ok2, const std::string& nw) {
                if (!ok2) return;
                if (nw.size() < 4) { app::toast("A nova senha deve ter pelo menos 4 dígitos."); return; }
                storage::settings().parentalPin = nw;
                storage::save();
                app::toast("Senha alterada com sucesso!");
            }, 8);
        }, 8);
    }

    void reloadLists()
    {
        if (reloading_) return;
        reloading_ = true;
        catalog::clear();
        auto alive = alive_;
        catalog::loadCategories([this, alive](bool ok) {
            if (*alive) reloading_ = false;
            app::toast(ok ? "Listas atualizadas" : "Falha ao atualizar as listas");
        });
    }

public:
    void input(Btn b) override
    {
        std::vector<Row> r = rows();
        int n = (int)r.size();
        if (b == Btn::Up) nav_.move(-1, n, 11);
        else if (b == Btn::Down) nav_.move(1, n, 11);
        else if (b == Btn::Cross && r[(size_t)nav_.sel].action) r[(size_t)nav_.sel].action();
        else if (b == Btn::Circle) app::pop();
    }

    void render() override
    {
        ui::background(200);
        ui::header("Configurações", APP_NAME " " APP_VERSION);
        std::vector<Row> r = rows();
        int x = 260, w = SCREEN_W - 520, y0 = 170, rh = 72;
        for (int i = 0; i < (int)r.size(); i++) {
            int y = y0 + i * (rh + 6);
            bool f = i == nav_.sel;
            gfx::fillRect(x, y, w, rh, f ? Color{0, 120, 200, 255} : Color{20, 24, 44, 255});
            gfx::text(r[(size_t)i].label, x + 30, y + 20, 28, col::text, f);
            gfx::text(r[(size_t)i].value, x + w - 30, y + 22, 26, f ? col::white : col::dim, false, w / 2, Align::Right);
        }
        ui::footer({{"X", "Selecionar"}, {"O", "Voltar"}});
    }
};

std::unique_ptr<Screen> makeSettings() { return std::unique_ptr<Screen>(new SettingsScreen()); }
