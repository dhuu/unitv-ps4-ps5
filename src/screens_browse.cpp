// Navegação por categorias: TV ao vivo (lista + EPG), Filmes/Séries/Kids/Adultos (grade de pôsteres)
// (MainActivity.kt, MovieCategoryActivity.kt, SeriesCategoryActivity.kt, KidsCategoryActivity.kt)
#include "screens.h"
#include "config.h"
#include "platform.h"
#include "jobs.h"
#include "storage.h"
#include "catalog.h"
#include "util.h"

#include <map>

struct CatRef {
    Kind kind;
    Category cat;
    bool adult;
};

class BrowseScreen : public Screen {
    Kind kind_;
    BrowseFilter filter_;
    std::vector<CatRef> cats_;
    ui::ListNav catNav_;
    int activeCat_ = -1;          // categoria cujo conteúdo está carregado à direita
    uint64_t catChangedAt_ = 0;
    int focus_ = 0;               // 0 = categorias, 1 = conteúdo
    ui::ListNav listNav_;         // canais
    ui::GridNav gridNav_;         // pôsteres

    // EPG
    std::map<int, std::vector<EpgEntry>> epg_;
    std::map<int, bool> epgRequested_;
    int epgFor_ = 0;
    uint64_t selChangedAt_ = 0;

    static const int kCatVisible = 13;
    static const int kChanVisible = 12;
    static const int kGridRows = 2;

public:
    BrowseScreen(Kind kind, BrowseFilter filter) : kind_(kind), filter_(filter)
    {
        gridNav_.cols = 5;
        auto addFrom = [&](Kind k) {
            for (auto& c : catalog::categories(k)) {
                bool adult = util::isAdultCategory(c.name);
                if (filter_ == BrowseFilter::Kids && (!util::isKidsCategory(c.name) || adult)) continue;
                if (filter_ == BrowseFilter::Adult && !adult) continue;
                cats_.push_back({k, c, adult});
            }
        };
        if (filter_ == BrowseFilter::All) {
            addFrom(kind_);
        } else {
            addFrom(Kind::Vod);
            addFrom(Kind::Series);
        }
        if (!cats_.empty()) { activeCat_ = 0; catChangedAt_ = 0; }
    }

    Kind contentKind() const { return activeCat_ >= 0 ? cats_[(size_t)activeCat_].kind : kind_; }

    bool locked() const
    {
        return activeCat_ >= 0 && cats_[(size_t)activeCat_].adult && !app::adultUnlocked();
    }

    const std::vector<Stream>* content(catalog::Load* st)
    {
        if (activeCat_ < 0) { if (st) *st = catalog::Load::Ready; return nullptr; }
        if (locked()) { if (st) *st = catalog::Load::None; return nullptr; }
        const CatRef& c = cats_[(size_t)activeCat_];
        return catalog::streams(c.kind, c.cat.id, st);
    }

    std::string title() const
    {
        if (filter_ == BrowseFilter::Kids) return "Kids";
        if (filter_ == BrowseFilter::Adult) return "Adultos";
        return kind_ == Kind::Live ? "TV ao Vivo" : (kind_ == Kind::Series ? "Séries" : "Filmes");
    }

    void selectContent(const std::vector<Stream>& list, int idx)
    {
        const Stream& s = list[(size_t)idx];
        if (s.kind == Kind::Live) {
            std::vector<PlayItem> pl;
            for (auto& x : list) {
                PlayItem p;
                p.kind = Kind::Live;
                p.id = x.id;
                p.name = x.name;
                p.icon = x.icon;
                pl.push_back(p);
            }
            startPlayback(pl, idx);
        } else {
            openStream(s);
        }
    }

    void toggleFav(const Stream& s)
    {
        SavedItem it;
        it.kind = s.kind;
        it.id = s.id;
        it.name = s.name;
        it.icon = s.icon;
        it.ext = s.ext;
        bool was = storage::isFavorite(s.kind, s.id);
        storage::toggleFavorite(it);
        app::toast(was ? "Removido dos favoritos" : "Adicionado aos favoritos");
    }

    void input(Btn b) override
    {
        int nc = (int)cats_.size();
        if (b == Btn::Circle) {
            if (focus_ == 1) focus_ = 0;
            else app::pop();
            return;
        }
        if (b == Btn::Triangle) { app::push(makeSearch()); return; }

        if (focus_ == 0) {
            switch (b) {
            case Btn::Up: if (catNav_.move(-1, nc, kCatVisible)) catChangedAt_ = platform::ticksMs(); break;
            case Btn::Down: if (catNav_.move(1, nc, kCatVisible)) catChangedAt_ = platform::ticksMs(); break;
            case Btn::L1: if (catNav_.move(-kCatVisible, nc, kCatVisible)) catChangedAt_ = platform::ticksMs(); break;
            case Btn::R1: if (catNav_.move(kCatVisible, nc, kCatVisible)) catChangedAt_ = platform::ticksMs(); break;
            case Btn::Right:
            case Btn::Cross:
                if (nc == 0) break;
                if (activeCat_ != catNav_.sel) setActive(catNav_.sel);
                if (locked()) {
                    app::requirePin([] {});
                    break;
                }
                {
                    catalog::Load st;
                    content(&st);
                    if (st == catalog::Load::Failed) {
                        catalog::retry(cats_[(size_t)activeCat_].kind, cats_[(size_t)activeCat_].cat.id);
                        break;
                    }
                }
                focus_ = 1;
                break;
            default: break;
            }
            return;
        }

        catalog::Load st;
        const std::vector<Stream>* list = content(&st);
        int n = list ? (int)list->size() : 0;
        if (contentKind() == Kind::Live) {
            switch (b) {
            case Btn::Up: if (listNav_.move(-1, n, kChanVisible)) selChangedAt_ = platform::ticksMs(); break;
            case Btn::Down: if (listNav_.move(1, n, kChanVisible)) selChangedAt_ = platform::ticksMs(); break;
            case Btn::L1: if (listNav_.move(-kChanVisible, n, kChanVisible)) selChangedAt_ = platform::ticksMs(); break;
            case Btn::R1: if (listNav_.move(kChanVisible, n, kChanVisible)) selChangedAt_ = platform::ticksMs(); break;
            case Btn::Left: focus_ = 0; break;
            case Btn::Cross: if (n) selectContent(*list, listNav_.sel); break;
            case Btn::Square: if (n) toggleFav((*list)[(size_t)listNav_.sel]); break;
            default: break;
            }
        } else {
            switch (b) {
            case Btn::Up: gridNav_.move(0, -1, n, kGridRows); break;
            case Btn::Down: gridNav_.move(0, 1, n, kGridRows); break;
            case Btn::Left: if (!gridNav_.move(-1, 0, n, kGridRows)) focus_ = 0; break;
            case Btn::Right: gridNav_.move(1, 0, n, kGridRows); break;
            case Btn::L1: gridNav_.move(0, -kGridRows, n, kGridRows); break;
            case Btn::R1: gridNav_.move(0, kGridRows, n, kGridRows); break;
            case Btn::Cross: if (n) selectContent(*list, gridNav_.sel); break;
            case Btn::Square: if (n) toggleFav((*list)[(size_t)gridNav_.sel]); break;
            default: break;
            }
        }
    }

    void setActive(int i)
    {
        app::redraw();
        activeCat_ = i;
        listNav_ = ui::ListNav();
        gridNav_ = ui::GridNav();
        gridNav_.cols = 5;
        selChangedAt_ = platform::ticksMs();
        images::clearQueue();
    }

    void update() override
    {
        // troca o conteúdo quando o usuário para numa categoria (evita requisições a cada passo)
        if (catNav_.sel != activeCat_ && catChangedAt_ && platform::ticksMs() - catChangedAt_ > 450) {
            setActive(catNav_.sel);
            catChangedAt_ = 0;
        }
        // EPG do canal selecionado
        if (contentKind() == Kind::Live) {
            catalog::Load st;
            const std::vector<Stream>* list = content(&st);
            if (list && !list->empty() && platform::ticksMs() - selChangedAt_ > 500) {
                listNav_.clamp((int)list->size(), kChanVisible);
                int id = (*list)[(size_t)listNav_.sel].id;
                epgFor_ = id;
                if (!epgRequested_[id]) {
                    epgRequested_[id] = true;
                    auto out = std::make_shared<std::vector<EpgEntry>>();
                    auto alive = alive_;
                    jobs::run([id, out]() { *out = api::shortEpg(id); },
                              [this, alive, id, out]() { if (*alive) epg_[id] = *out; });
                }
            }
        }
    }

    void renderCategories()
    {
        int x = 40, y = 150, w = 450, rh = 60;
        gfx::fillRect(x, y - 10, w, kCatVisible * rh + 20, {10, 12, 24, 255});
        int n = (int)cats_.size();
        if (n == 0) {
            gfx::textWrapped("Nenhuma categoria disponível.", x + 20, y + 20, w - 40, 26, col::dim);
            return;
        }
        for (int i = catNav_.top; i < n && i < catNav_.top + kCatVisible; i++) {
            int ry = y + (i - catNav_.top) * rh;
            bool sel = i == catNav_.sel;
            bool active = i == activeCat_;
            if (sel) gfx::fillRect(x, ry, w, rh - 4, focus_ == 0 ? col::accent : Color{40, 50, 90, 255});
            else if (active) gfx::fillRect(x, ry, w, rh - 4, {30, 36, 64, 255});
            if (active) gfx::fillRect(x, ry, 6, rh - 4, col::accent2);
            std::string label = cats_[(size_t)i].cat.name;
            if (cats_[(size_t)i].adult) label = "[18+] " + label;
            gfx::text(label, x + 22, ry + 14, 26, sel ? col::white : col::text, sel, w - 40);
        }
        if (n > kCatVisible) {
            int barH = kCatVisible * rh;
            int th = barH * kCatVisible / n;
            if (th < 30) th = 30;
            int ty = y + (barH - th) * catNav_.top / (n - kCatVisible > 0 ? n - kCatVisible : 1);
            gfx::fillRect(x + w + 6, ty, 6, th, col::dim);
        }
    }

    void renderLoading(catalog::Load st, int cx, int cy)
    {
        if (locked()) {
            gfx::text("Conteúdo protegido por senha", cx, cy - 20, 32, col::text, true, 0, Align::Center);
            gfx::text("Pressione X para desbloquear", cx, cy + 30, 26, col::dim, false, 0, Align::Center);
        } else if (st == catalog::Load::Failed) {
            gfx::text("Falha ao carregar. Pressione X na categoria para tentar novamente.", cx, cy, 28, col::danger, false, 0,
                      Align::Center);
        } else if (st == catalog::Load::Loading) {
            ui::spinner(cx, cy);
            gfx::text("Carregando...", cx, cy + 50, 26, col::dim, false, 0, Align::Center);
        } else {
            gfx::text("Nenhum conteúdo nesta categoria", cx, cy, 28, col::dim, false, 0, Align::Center);
        }
    }

    void renderLive(const std::vector<Stream>* list, catalog::Load st)
    {
        int x = 530, y = 150, w = 700, rh = 66;
        gfx::fillRect(x, y - 10, w, kChanVisible * rh + 20, {10, 12, 24, 255});
        if (!list || list->empty()) { renderLoading(st, x + w / 2, y + 300); return; }
        int n = (int)list->size();
        listNav_.clamp(n, kChanVisible);
        for (int i = listNav_.top; i < n && i < listNav_.top + kChanVisible; i++) {
            const Stream& s = (*list)[(size_t)i];
            int ry = y + (i - listNav_.top) * rh;
            bool sel = i == listNav_.sel;
            if (sel) gfx::fillRect(x, ry, w, rh - 4, focus_ == 1 ? col::accent : Color{40, 50, 90, 255});
            gfx::text(std::to_string(i + 1), x + 50, ry + 18, 24, sel ? col::white : col::dim, false, 0, Align::Right);
            ui::poster(s.icon, x + 64, ry + 4, 80, rh - 12, s.name, false);
            std::string nm = s.name;
            if (storage::isFavorite(Kind::Live, s.id)) nm = "* " + nm;
            gfx::text(nm, x + 160, ry + 16, 26, sel ? col::white : col::text, sel, w - 180);
        }

        // painel de pré-visualização com EPG
        int px = 1260, pw = SCREEN_W - px - 40;
        gfx::fillRect(px, y - 10, pw, kChanVisible * rh + 20, {10, 12, 24, 255});
        const Stream& cur = (*list)[(size_t)listNav_.sel];
        ui::poster(cur.icon, px + 30, y + 20, pw - 60, 260, cur.name, false);
        gfx::text(cur.name, px + 30, y + 310, 32, col::text, true, pw - 60);
        auto it = epg_.find(cur.id);
        int ey = y + 370;
        if (epgFor_ == cur.id && it != epg_.end() && !it->second.empty()) {
            for (size_t k = 0; k < it->second.size() && k < 3; k++) {
                const EpgEntry& e = it->second[k];
                std::string when = e.start ? util::formatClock(e.start) + " - " + util::formatClock(e.end) : "";
                gfx::text(k == 0 ? "AGORA" : "A SEGUIR", px + 30, ey, 20, k == 0 ? col::accent2 : col::dim, true);
                gfx::text(when, px + pw - 30, ey, 20, col::dim, false, 0, Align::Right);
                gfx::text(e.title, px + 30, ey + 28, 26, col::text, k == 0, pw - 60);
                if (k == 0 && !e.description.empty()) {
                    int h = gfx::textWrapped(e.description, px + 30, ey + 66, pw - 60, 22, col::dim, 3);
                    ey += h + 10;
                }
                ey += 80;
            }
        } else {
            gfx::text("Sem guia de programação", px + 30, ey, 24, col::dim);
        }
        gfx::text("Pressione X para assistir", px + pw / 2, y + kChanVisible * rh - 30, 24, col::accent, false, 0,
                  Align::Center);
    }

    void renderGrid(const std::vector<Stream>* list, catalog::Load st)
    {
        int x0 = 540, y0 = 150;
        if (!list || list->empty()) { renderLoading(st, x0 + 680, y0 + 330); return; }
        int n = (int)list->size();
        gridNav_.clamp(n, kGridRows);
        int pw = 230, ph = 330, gapX = 40, gapY = 76;
        for (int r = 0; r < kGridRows + 1; r++) {   // +1 linha parcialmente visível para pré-carregar
            for (int c = 0; c < gridNav_.cols; c++) {
                int i = (gridNav_.topRow + r) * gridNav_.cols + c;
                if (i >= n) break;
                const Stream& s = (*list)[(size_t)i];
                int x = x0 + c * (pw + gapX), y = y0 + r * (ph + gapY);
                if (r == kGridRows) { images::get(s.icon, pw, ph); continue; } // só pré-carrega (mesmo tamanho do poster)
                ui::poster(s.icon, x, y, pw, ph, s.name);
                if (!s.rating.empty() && s.rating != "0") {
                    gfx::fillRect(x + pw - 70, y + 8, 62, 30, {0, 0, 0, 190});
                    gfx::text(s.rating.substr(0, 3), x + pw - 39, y + 12, 20, col::accent2, true, 0, Align::Center);
                }
                if (storage::isFavorite(s.kind, s.id)) {
                    gfx::fillRect(x + 8, y + 8, 34, 30, {0, 0, 0, 190});
                    gfx::text("*", x + 25, y + 10, 26, col::accent2, true, 0, Align::Center);
                }
                bool sel = i == gridNav_.sel;
                gfx::text(s.name, x, y + ph + 12, 22, sel && focus_ == 1 ? col::text : col::dim, sel, pw);
                if (sel && focus_ == 1) ui::focusFrame(x, y, pw, ph);
            }
        }
        gfx::text(std::to_string(gridNav_.sel + 1) + " / " + std::to_string(n), SCREEN_W - 60, 110, 24, col::dim, false, 0,
                  Align::Right);
    }

    void render() override
    {
        ui::background(200);
        std::string sub = activeCat_ >= 0 ? cats_[(size_t)activeCat_].cat.name : "";
        ui::header(title(), sub);
        renderCategories();
        catalog::Load st = catalog::Load::None;
        const std::vector<Stream>* list = content(&st);
        if (contentKind() == Kind::Live) renderLive(list, st);
        else renderGrid(list, st);
        ui::footer({{"X", contentKind() == Kind::Live ? "Assistir" : "Abrir"}, {"[]", "Favorito"}, {"/\\", "Buscar"},
                    {"L1/R1", "Página"}, {"O", "Voltar"}});
    }
};

std::unique_ptr<Screen> makeBrowse(Kind kind, BrowseFilter filter)
{
    return std::unique_ptr<Screen>(new BrowseScreen(kind, filter));
}
