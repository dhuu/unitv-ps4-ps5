// Detalhes de filme e de série (MovieDetailsActivity.kt / SeriesDetailsActivity.kt)
#include "screens.h"
#include "config.h"
#include "platform.h"
#include "jobs.h"
#include "storage.h"
#include "util.h"

static std::string metaLine(std::initializer_list<std::string> parts)
{
    std::string out;
    for (auto& p : parts) {
        if (p.empty() || p == "0" || p == "null") continue;
        if (!out.empty()) out += "   •   ";
        out += p;
    }
    return out;
}

static void toggleFavorite(const Stream& s, const std::string& icon)
{
    SavedItem it;
    it.kind = s.kind;
    it.id = s.id;
    it.name = s.name;
    it.icon = icon.empty() ? s.icon : icon;
    it.ext = s.ext;
    bool was = storage::isFavorite(s.kind, s.id);
    storage::toggleFavorite(it);
    app::toast(was ? "Removido dos favoritos" : "Adicionado aos favoritos");
}

// ---------------------------------------------------------------------------
class MovieDetailsScreen : public Screen {
    Stream s_;
    VodInfo info_;
    bool loading_ = true;
    int btn_ = 0;

public:
    explicit MovieDetailsScreen(const Stream& s) : s_(s)
    {
        auto out = std::make_shared<VodInfo>();
        auto alive = alive_;
        int id = s.id;
        jobs::run([out, id]() { *out = api::vodInfo(id); },
                  [this, out, alive]() {
                      if (!*alive) return;
                      info_ = *out;
                      loading_ = false;
                      if (!info_.ext.empty()) s_.ext = info_.ext;
                  });
    }

    std::vector<std::string> buttons() const
    {
        uint64_t pos = storage::getProgress("vod_" + std::to_string(s_.id));
        std::vector<std::string> b;
        b.push_back(pos ? "Continuar (" + util::formatTime(pos) + ")" : "Assistir");
        if (pos) b.push_back("Do início");
        b.push_back(storage::isFavorite(Kind::Vod, s_.id) ? "Remover favorito" : "Favoritar");
        b.push_back("Sinopse");
        return b;
    }

    void play(bool fromStart)
    {
        PlayItem p;
        p.kind = Kind::Vod;
        p.id = s_.id;
        p.name = s_.name;
        p.icon = s_.icon;
        p.ext = s_.ext;
        uint64_t pos = fromStart ? 0 : storage::getProgress(p.progressKey());
        if (fromStart) storage::clearProgress(p.progressKey());
        app::push(makePlayer({p}, 0, pos));
    }

    void input(Btn b) override
    {
        std::vector<std::string> bs = buttons();
        int n = (int)bs.size();
        if (b == Btn::Left && btn_ > 0) btn_--;
        else if (b == Btn::Right && btn_ < n - 1) btn_++;
        else if (b == Btn::Circle) app::pop();
        else if (b == Btn::Square) toggleFavorite(s_, info_.cover);
        else if (b == Btn::Cross) {
            const std::string& lbl = bs[(size_t)btn_];
            if (btn_ == 0) play(false);
            else if (lbl == "Do início") play(true);
            else if (lbl == "Sinopse") app::message(s_.name, info_.plot.empty() ? "Sinopse indisponível." : info_.plot);
            else toggleFavorite(s_, info_.cover);
        }
        if (btn_ >= (int)buttons().size()) btn_ = (int)buttons().size() - 1;
    }

    void render() override
    {
        std::string backdrop = !info_.backdrop.empty() ? info_.backdrop : (!info_.cover.empty() ? info_.cover : s_.icon);
        ui::backdrop(backdrop, 190);
        ui::header("Filmes", catalogName());

        int px = 120, py = 170, pw = 420, ph = 630;
        ui::poster(info_.cover.empty() ? s_.icon : info_.cover, px, py, pw, ph, s_.name);

        int x = px + pw + 70, w = SCREEN_W - x - 120;
        gfx::textWrapped(info_.name.empty() ? s_.name : info_.name, x, py, w, 52, col::text, 2, true);
        int y = py + 140;
        std::string year = info_.releaseDate.size() >= 4 ? info_.releaseDate.substr(0, 4) : info_.releaseDate;
        std::string rating = info_.rating.empty() ? s_.rating : info_.rating;
        gfx::text(metaLine({year, info_.genre, info_.duration, rating.empty() ? "" : ("Nota " + rating)}), x, y, 26,
                  col::accent2, false, w);
        y += 60;
        if (loading_) {
            ui::spinner(x + 40, y + 40);
        } else {
            y += gfx::textWrapped(info_.plot.empty() ? "Sinopse indisponível." : info_.plot, x, y, w, 28, col::text, 7) + 20;
            if (!info_.director.empty()) { gfx::text("Direção: " + info_.director, x, y, 24, col::dim, false, w); y += 40; }
            if (!info_.cast.empty()) gfx::textWrapped("Elenco: " + info_.cast, x, y, w, 24, col::dim, 2);
        }

        std::vector<std::string> bs = buttons();
        int bx = x, by = py + ph - 80;
        for (size_t i = 0; i < bs.size(); i++) {
            int bw = gfx::textWidth(bs[i], 28, true) + 80;
            ui::button(bx, by, bw, 76, bs[i], (int)i == btn_, i == 0);
            bx += bw + 24;
        }
        ui::footer({{"X", "Selecionar"}, {"[]", "Favorito"}, {"O", "Voltar"}});
    }

    std::string catalogName() const { return ""; }
};

std::unique_ptr<Screen> makeMovieDetails(const Stream& s)
{
    return std::unique_ptr<Screen>(new MovieDetailsScreen(s));
}

// ---------------------------------------------------------------------------
class SeriesDetailsScreen : public Screen {
    Stream s_;
    SeriesInfo info_;
    bool loading_ = true;
    int season_ = 0;          // índice em info_.seasons
    int focus_ = 1;           // 0 = temporadas, 1 = episódios
    ui::ListNav epNav_;
    static const int kVisible = 6;

public:
    explicit SeriesDetailsScreen(const Stream& s) : s_(s)
    {
        auto out = std::make_shared<SeriesInfo>();
        auto alive = alive_;
        int id = s.id;
        jobs::run([out, id]() { *out = api::seriesInfo(id); },
                  [this, out, alive]() {
                      if (!*alive) return;
                      info_ = std::move(*out);
                      loading_ = false;
                      if (info_.seasons.empty()) focus_ = 0;
                  });
    }

    const std::vector<Episode>* episodes() const
    {
        if (info_.seasons.empty()) return nullptr;
        auto it = info_.episodes.find(info_.seasons[(size_t)season_]);
        return it == info_.episodes.end() ? nullptr : &it->second;
    }

    std::string cover() const { return info_.cover.empty() ? s_.icon : info_.cover; }

    // Lista global ordenada (temporada, episódio) para o "Próximo episódio"
    void play(int seasonIdx, int epIdx)
    {
        std::vector<PlayItem> list;
        int start = 0;
        for (size_t si = 0; si < info_.seasons.size(); si++) {
            auto it = info_.episodes.find(info_.seasons[si]);
            if (it == info_.episodes.end()) continue;
            for (size_t ei = 0; ei < it->second.size(); ei++) {
                const Episode& e = it->second[ei];
                if ((int)si == seasonIdx && (int)ei == epIdx) start = (int)list.size();
                PlayItem p;
                p.kind = Kind::Episode;
                p.id = atoi(e.id.c_str());
                p.episodeId = e.id;
                p.seriesId = s_.id;
                char buf[32];
                snprintf(buf, sizeof(buf), "T%02d E%02d", e.season, e.num);
                p.name = std::string(buf) + (e.title.empty() ? "" : " - " + e.title);
                p.icon = e.image.empty() ? cover() : e.image;
                p.ext = e.ext;
                p.seriesName = info_.name.empty() ? s_.name : info_.name;
                p.seriesCover = cover();
                list.push_back(p);
            }
        }
        if (!list.empty()) startPlayback(list, start);
    }

    void input(Btn b) override
    {
        int ns = (int)info_.seasons.size();
        const std::vector<Episode>* eps = episodes();
        int ne = eps ? (int)eps->size() : 0;
        if (b == Btn::Circle) { app::pop(); return; }
        if (b == Btn::Square) { toggleFavorite(s_, cover()); return; }
        if (b == Btn::Triangle) {
            app::message(s_.name, info_.plot.empty() ? "Sinopse indisponível." : info_.plot);
            return;
        }
        if (focus_ == 0) {
            if (b == Btn::Left && season_ > 0) { season_--; epNav_ = ui::ListNav(); }
            else if (b == Btn::Right && season_ < ns - 1) { season_++; epNav_ = ui::ListNav(); }
            else if ((b == Btn::Down || b == Btn::Cross) && ne) focus_ = 1;
        } else {
            if (b == Btn::Up) { if (epNav_.sel == 0) focus_ = 0; else epNav_.move(-1, ne, kVisible); }
            else if (b == Btn::Down) epNav_.move(1, ne, kVisible);
            else if (b == Btn::L1 && season_ > 0) { season_--; epNav_ = ui::ListNav(); }
            else if (b == Btn::R1 && season_ < ns - 1) { season_++; epNav_ = ui::ListNav(); }
            else if (b == Btn::Cross && ne) play(season_, epNav_.sel);
        }
    }

    void render() override
    {
        std::string backdrop = !info_.backdrop.empty() ? info_.backdrop : cover();
        ui::backdrop(backdrop, 200);
        ui::header("Séries", storage::isFavorite(Kind::Series, s_.id) ? "Nos seus favoritos" : "");

        int px = 100, py = 160, pw = 300, ph = 450;
        ui::poster(cover(), px, py, pw, ph, s_.name);
        gfx::textWrapped(info_.name.empty() ? s_.name : info_.name, px, py + ph + 20, pw, 30, col::text, 3, true);
        std::string year = info_.releaseDate.size() >= 4 ? info_.releaseDate.substr(0, 4) : "";
        gfx::textWrapped(metaLine({year, info_.genre, info_.rating.empty() ? "" : "Nota " + info_.rating}), px,
                         py + ph + 140, pw, 22, col::accent2, 3);

        int x = px + pw + 60, w = SCREEN_W - x - 80;
        if (loading_) {
            ui::spinner(x + w / 2, 500);
            gfx::text("Carregando episódios...", x + w / 2, 560, 26, col::dim, false, 0, Align::Center);
            ui::footer({{"O", "Voltar"}});
            return;
        }
        gfx::textWrapped(info_.plot.empty() ? "Sinopse indisponível." : info_.plot, x, py, w, 26, col::text, 3);

        // abas de temporadas
        int ty = py + 130;
        int tx = x;
        int first = season_ > 6 ? season_ - 6 : 0;
        for (int i = first; i < (int)info_.seasons.size(); i++) {
            std::string label = "Temporada " + std::to_string(info_.seasons[(size_t)i]);
            int bw = gfx::textWidth(label, 24, true) + 40;
            if (tx + bw > x + w) break;
            bool sel = i == season_;
            gfx::fillRect(tx, ty, bw, 56, sel ? (focus_ == 0 ? col::accent : Color{40, 50, 90, 255}) : Color{24, 28, 50, 255});
            gfx::text(label, tx + bw / 2, ty + 14, 24, col::white, sel, 0, Align::Center);
            tx += bw + 12;
        }
        if (info_.seasons.empty())
            gfx::text("Nenhum episódio disponível", x, ty + 14, 26, col::dim);

        // episódios
        const std::vector<Episode>* eps = episodes();
        int ey = ty + 80, rh = 116;
        if (eps) {
            int ne = (int)eps->size();
            epNav_.clamp(ne, kVisible);
            for (int i = epNav_.top; i < ne && i < epNav_.top + kVisible; i++) {
                const Episode& e = (*eps)[(size_t)i];
                int ry = ey + (i - epNav_.top) * rh;
                bool sel = i == epNav_.sel && focus_ == 1;
                gfx::fillRect(x, ry, w, rh - 10, sel ? Color{0, 110, 190, 255} : Color{18, 22, 40, 255});
                ui::poster(e.image.empty() ? cover() : e.image, x + 8, ry + 8, 160, rh - 26, e.title);
                char num[32];
                snprintf(num, sizeof(num), "Episódio %d", e.num);
                gfx::text(num, x + 190, ry + 12, 22, sel ? col::white : col::accent2, true);
                gfx::text(e.title.empty() ? num : e.title, x + 190, ry + 42, 26, col::text, sel, w - 400);
                if (!e.duration.empty()) gfx::text(e.duration, x + w - 24, ry + 14, 22, col::dim, false, 0, Align::Right);
                std::string key = "series_" + std::to_string(s_.id) + "_" + e.id;
                uint64_t pos = storage::getProgress(key);
                if (pos) {
                    gfx::text("Parou em " + util::formatTime(pos), x + 190, ry + 76, 20, col::dim);
                    gfx::fillRect(x + 8, ry + rh - 18, 160, 6, {255, 255, 255, 60});
                    gfx::fillRect(x + 8, ry + rh - 18, 80, 6, col::danger);
                }
            }
        }
        ui::footer({{"X", "Assistir"}, {"L1/R1", "Temporada"}, {"[]", "Favorito"}, {"/\\", "Sinopse"}, {"O", "Voltar"}});
    }
};

std::unique_ptr<Screen> makeSeriesDetails(const Stream& s)
{
    return std::unique_ptr<Screen>(new SeriesDetailsScreen(s));
}
