// Player em tela cheia (PlayerActivity.kt / TvPlayerActivity.kt)
#include "screens.h"
#include "config.h"
#include "platform.h"
#include "jobs.h"
#include "storage.h"
#include "player.h"
#include "util.h"

void startPlayback(std::vector<PlayItem> list, int index)
{
    if (list.empty()) return;
    const PlayItem& p = list[(size_t)index];
    std::string key = p.progressKey();
    uint64_t pos = key.empty() ? 0 : storage::getProgress(key);
    if (!pos) {
        app::push(makePlayer(std::move(list), index, 0));
        return;
    }
    auto shared = std::make_shared<std::vector<PlayItem>>(std::move(list));
    app::dialog("Continuar assistindo?",
                "Você parou em " + util::formatTime(pos) + ". Deseja continuar de onde parou?", {"Sim", "Não"},
                [shared, index, pos, key](int r) {
                    if (r == 0) {
                        app::push(makePlayer(*shared, index, pos));
                    } else {
                        storage::clearProgress(key);
                        app::push(makePlayer(*shared, index, 0));
                    }
                });
}

class PlayerScreen : public Screen {
    std::vector<PlayItem> list_;
    int idx_;
    uint64_t pendingSeek_;
    uint64_t osdUntil_ = 0;
    uint64_t lastSave_ = 0;
    bool started_ = false;
    bool errorShown_ = false;
    int liveRetries_ = 0;
    int vodRetries_ = 0;
    std::vector<EpgEntry> epg_;
    int epgFor_ = 0;
    uint64_t seekAccum_ = 0;   // alvo de busca acumulado (setas repetidas)
    uint64_t seekCommitAt_ = 0;

    const PlayItem& cur() const { return list_[(size_t)idx_]; }
    bool isLive() const { return cur().kind == Kind::Live; }

public:
    PlayerScreen(std::vector<PlayItem> list, int idx, uint64_t startMs)
        : list_(std::move(list)), idx_(idx), pendingSeek_(startMs) {}

    ~PlayerScreen() override
    {
        saveProgress();
        player::close();
    }

    bool drawsVideo() const override { return true; }

    void showOsd(uint64_t ms = 4500) { osdUntil_ = platform::ticksMs() + ms; }

    void addHistory()
    {
        const PlayItem& p = cur();
        SavedItem it;
        if (p.kind == Kind::Episode) {
            it.kind = Kind::Series;
            it.id = p.seriesId;
            it.name = p.seriesName;
            it.icon = p.seriesCover;
        } else {
            it.kind = p.kind;
            it.id = p.id;
            it.name = p.name;
            it.icon = p.icon;
            it.ext = p.ext;
        }
        storage::addHistory(it);
    }

    void openCurrent()
    {
        errorShown_ = false;
        player::open(cur().url());
        addHistory();
        showOsd();
        lastSave_ = platform::ticksMs();
        if (isLive()) loadEpg();
    }

    void loadEpg()
    {
        int id = cur().id;
        epg_.clear();
        epgFor_ = id;
        auto out = std::make_shared<std::vector<EpgEntry>>();
        auto alive = alive_;
        jobs::run([id, out]() { *out = api::shortEpg(id); },
                  [this, alive, id, out]() { if (*alive && epgFor_ == id) epg_ = *out; });
    }

    void saveProgress()
    {
        if (!player::isOpen() || isLive()) return;
        std::string key = cur().progressKey();
        if (key.empty()) return;
        uint64_t pos = player::positionMs(), dur = player::durationMs();
        if (pos > 0) storage::saveProgress(key, pos, dur);
    }

    void go(int newIdx)
    {
        if (newIdx < 0 || newIdx >= (int)list_.size()) return;
        saveProgress();
        idx_ = newIdx;
        pendingSeek_ = 0;
        if (!isLive()) pendingSeek_ = storage::getProgress(cur().progressKey());
        openCurrent();
    }

    void enter() override
    {
        if (!started_) { started_ = true; openCurrent(); }
    }

    void seekBy(int64_t deltaMs)
    {
        if (isLive()) return;
        uint64_t base = seekCommitAt_ ? seekAccum_ : player::positionMs();
        int64_t t = (int64_t)base + deltaMs;
        uint64_t dur = player::durationMs();
        if (t < 0) t = 0;
        if (dur && (uint64_t)t > dur - 2000) t = (int64_t)(dur > 2000 ? dur - 2000 : 0);
        seekAccum_ = (uint64_t)t;
        seekCommitAt_ = platform::ticksMs() + 600;
        showOsd();
    }

    void input(Btn b) override
    {
        switch (b) {
        case Btn::Circle:
            app::pop();
            break;
        case Btn::Cross:
            if (isLive()) showOsd();
            else { player::togglePause(); showOsd(); }
            break;
        case Btn::Left: seekBy(-10000); break;
        case Btn::Right: seekBy(10000); break;
        case Btn::L1: if (isLive()) go(idx_ - 1); else seekBy(-60000); break;
        case Btn::R1: if (isLive()) go(idx_ + 1); else seekBy(60000); break;
        case Btn::L2: seekBy(-300000); break;
        case Btn::R2: seekBy(300000); break;
        case Btn::Up:
            if (isLive()) go(idx_ > 0 ? idx_ - 1 : (int)list_.size() - 1);
            else showOsd();
            break;
        case Btn::Down:
            if (isLive()) go(idx_ + 1 < (int)list_.size() ? idx_ + 1 : 0);
            else showOsd();
            break;
        case Btn::Square:
            if (cur().kind == Kind::Episode && idx_ + 1 < (int)list_.size()) {
                storage::clearProgress(cur().progressKey());
                go(idx_ + 1);
            } else {
                showOsd();
            }
            break;
        case Btn::Triangle:
        case Btn::Options:
        case Btn::Touchpad:
            showOsd(8000);
            break;
        default: break;
        }
    }

    void update() override
    {
        uint64_t now = platform::ticksMs();
        player::State st = player::state();
        static uint64_t lastBeat = 0;
        if (now - lastBeat > 5000) {
            lastBeat = now;
            platform::log("tela player: estado=%d pos=%llu ms mem=%d MB", (int)st,
                          (unsigned long long)player::positionMs(), platform::freeMemoryMB());
        }

        if (seekCommitAt_ && now >= seekCommitAt_) {
            player::seekTo(seekAccum_);
            seekCommitAt_ = 0;
        }
        if (pendingSeek_ && st == player::State::Playing && player::hasVideoFrame()) {
            player::seekTo(pendingSeek_);
            pendingSeek_ = 0;
        }
        if (!isLive() && now - lastSave_ > 10000 && st == player::State::Playing) {
            lastSave_ = now;
            saveProgress();
        }

        if (st == player::State::Ended) {
            if (isLive()) {
                if (liveRetries_++ < 3) { openCurrent(); return; }
            } else {
                // "terminou" muito antes do fim (queda de conexão): reabre no mesmo ponto
                uint64_t pos = player::positionMs(), dur = player::durationMs();
                if (dur > 0 && pos + 60000 < dur && vodRetries_ < 5) {
                    vodRetries_++;
                    platform::log("fim inesperado em %llu/%llu ms: reabrindo", (unsigned long long)pos,
                                  (unsigned long long)dur);
                    pendingSeek_ = pos > 5000 ? pos - 5000 : 0;
                    openCurrent();
                    return;
                }
                storage::clearProgress(cur().progressKey());
                if (cur().kind == Kind::Episode && idx_ + 1 < (int)list_.size()) {
                    app::toast("Próximo episódio");
                    go(idx_ + 1);
                    return;
                }
            }
            player::close();
            app::pop();
            return;
        }

        if (st == player::State::Error && !errorShown_) {
            errorShown_ = true;
            std::string msg = player::errorText();
            auto alive = alive_;
            app::dialog("Erro na reprodução", msg, {"Tentar novamente", "Voltar"}, [this, alive](int r) {
                if (!*alive) return;
                if (r == 0) openCurrent();
                else app::pop();
            });
        }
        if (st == player::State::Playing) liveRetries_ = 0;
    }

    void renderOsd()
    {
        uint64_t now = platform::ticksMs();
        player::State st = player::state();
        bool show = now < osdUntil_ || st == player::State::Paused || seekCommitAt_;

        if (st == player::State::Opening || st == player::State::Buffering || (st == player::State::Playing && !player::hasVideoFrame())) {
            ui::spinner(SCREEN_W / 2, SCREEN_H / 2, 34);
            gfx::text(st == player::State::Buffering ? "Carregando..." : "Conectando...", SCREEN_W / 2, SCREEN_H / 2 + 60,
                      26, col::text, false, 0, Align::Center);
        }
        if (!show) return;

        const PlayItem& p = cur();
        gfx::fillGradientV(0, 0, SCREEN_W, 200, {0, 0, 0, 210}, {0, 0, 0, 0});
        if (isLive()) {
            ui::poster(p.icon, 60, 40, 140, 100, p.name, false);
            gfx::text(std::to_string(idx_ + 1) + "  " + p.name, 230, 50, 40, col::text, true, 1400);
            if (epgFor_ == p.id && !epg_.empty()) {
                const EpgEntry& e = epg_[0];
                std::string when = e.start ? util::formatClock(e.start) + " - " + util::formatClock(e.end) + "  " : "";
                gfx::text("Agora: " + when + e.title, 232, 104, 26, col::accent2, false, 1400);
                if (epg_.size() > 1) gfx::text("A seguir: " + epg_[1].title, 232, 140, 22, col::dim, false, 1400);
            }
        } else {
            gfx::text(p.kind == Kind::Episode ? p.seriesName : p.name, 80, 50, 40, col::text, true, 1600);
            if (p.kind == Kind::Episode) gfx::text(p.name, 82, 104, 28, col::dim, false, 1600);
        }
        gfx::text(util::formatClock(platform::unixTime()), SCREEN_W - 60, 50, 36, col::text, true, 0, Align::Right);

        int by = SCREEN_H - 200;
        gfx::fillGradientV(0, by - 40, SCREEN_W, 240, {0, 0, 0, 0}, {0, 0, 0, 220});
        if (!isLive()) {
            uint64_t pos = seekCommitAt_ ? seekAccum_ : player::positionMs();
            uint64_t dur = player::durationMs();
            int bx = 80, bw = SCREEN_W - 160;
            gfx::fillRect(bx, by + 40, bw, 10, {255, 255, 255, 70});
            if (dur) {
                int fw = (int)((double)pos / (double)dur * bw);
                if (fw > bw) fw = bw;
                gfx::fillRect(bx, by + 40, fw, 10, col::danger);
                gfx::fillRect(bx + fw - 8, by + 37, 16, 16, col::white);
            }
            gfx::text(util::formatTime(pos), bx, by + 66, 26, col::text);
            gfx::text(dur ? util::formatTime(dur) : "--:--", bx + bw, by + 66, 26, col::text, false, 0, Align::Right);
            const char* stTxt = st == player::State::Paused ? "PAUSADO" : "";
            gfx::text(stTxt, SCREEN_W / 2, by + 66, 28, col::accent2, true, 0, Align::Center);
            std::vector<std::pair<std::string, std::string>> hints = {
                {"X", st == player::State::Paused ? "Continuar" : "Pausar"}, {"<- ->", "10s"}, {"L1/R1", "1 min"}};
            if (p.kind == Kind::Episode && idx_ + 1 < (int)list_.size()) hints.push_back({"[]", "Próximo episódio"});
            hints.push_back({"O", "Sair"});
            ui::footer(hints);
        } else {
            ui::footer({{"Cima/Baixo", "Trocar canal"}, {"X", "Info"}, {"O", "Sair"}});
        }
    }

    void render() override
    {
        player::render(gfx::screen());
        renderOsd();
    }
};

std::unique_ptr<Screen> makePlayer(std::vector<PlayItem> list, int index, uint64_t startMs)
{
    return std::unique_ptr<Screen>(new PlayerScreen(std::move(list), index, startMs));
}
