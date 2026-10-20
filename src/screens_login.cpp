// Splash + Login (SplashActivity.kt / LoginActivity.kt)
#include "screens.h"
#include "config.h"
#include "platform.h"
#include "jobs.h"
#include "ime.h"
#include "storage.h"
#include "catalog.h"
#include "util.h"
#include "player.h"

#include <memory>

LoginResult g_account;

std::string PlayItem::progressKey() const
{
    if (kind == Kind::Episode) return "series_" + std::to_string(seriesId) + "_" + episodeId;
    if (kind == Kind::Vod) return "vod_" + std::to_string(id);
    return "";
}

std::string PlayItem::url() const
{
    switch (kind) {
    case Kind::Live: return api::liveUrl(id);
    case Kind::Episode: return api::episodeUrl(episodeId, ext);
    default: return api::vodUrl(id, ext);
    }
}

void attemptLogin(std::vector<std::string> cands, const std::string& user, const std::string& pass,
                  std::function<void(bool, const std::string&)> done)
{
    // remove vazios e duplicados mantendo a ordem
    std::vector<std::string> list;
    for (auto& c : cands) {
        std::string n = api::normalizeDns(c);
        if (n.empty()) continue;
        bool dup = false;
        for (auto& l : list) if (l == n) dup = true;
        if (!dup) list.push_back(n);
    }
    if (list.empty()) { done(false, "Nenhum servidor (DNS) configurado no painel."); return; }

    struct Result { bool ok = false; std::string dns, err; LoginResult lr; };
    auto res = std::make_shared<Result>();
    jobs::run(
        [list, user, pass, res]() {
            for (auto& dns : list) {
                LoginResult lr = api::login(dns, user, pass);
                if (lr.ok) { res->ok = true; res->dns = dns; res->lr = lr; return; }
                res->err = lr.error;
                res->lr = lr;
                // conta inválida/expirada: não adianta tentar outros servidores
                if (!lr.status.empty()) return;
            }
        },
        [res, user, pass, done]() {
            if (res->ok) {
                api::Session& s = api::session();
                s.dns = res->dns;
                s.user = user;
                s.pass = pass;
                g_account = res->lr;
                Settings& st = storage::settings();
                st.dns = res->dns;
                st.user = user;
                st.pass = pass;
                st.loggedIn = true;
                storage::save();
                done(true, "");
            } else {
                done(false, res->err.empty() ? "Falha na conexão com todos os servidores." : res->err);
            }
        });
}

void logout()
{
    player::close();
    catalog::clear();
    storage::clearSession();
    api::session() = api::Session();
    g_account = LoginResult();
    app::replaceAll(makeLogin("Você saiu da conta."));
}

static std::vector<std::string> dnsCandidates(const std::string& preferred)
{
    std::vector<std::string> c;
    if (!preferred.empty()) c.push_back(preferred);
    Settings& s = storage::settings();
    if (!s.dns.empty()) c.push_back(s.dns);
    for (auto& d : s.dnsList) c.push_back(d);
    return c;
}

// Após login: carrega categorias e vai para a Home
static void syncAndEnter(std::function<void(const std::string&)> onError)
{
    catalog::loadCategories([onError](bool ok) {
        if (ok) app::replaceAll(makeHome());
        else onError("Não foi possível carregar as listas do servidor.");
    });
}

static void applyConfig(const PanelConfig& c)
{
    if (!c.ok) return;
    if (!c.dns.empty()) {
        storage::settings().dnsList = c.dns;
        storage::save();
    }
    app::Branding& b = app::branding();
    b.logoUrl = c.logo;
    b.backgroundUrl = c.background;
    b.message = c.message;
    b.whatsapp = c.whatsapp;
    b.eventsUrl = c.eventsUrl;
    b.newsBanner = c.newsBanner;
}

// ---------------------------------------------------------------------------
class SplashScreen : public Screen {
    SDL_Texture* splash_ = nullptr;
    std::string status_ = "Carregando...";
    bool started_ = false;

public:
    void enter() override
    {
        if (started_) return;
        started_ = true;
        splash_ = gfx::loadTextureFile(APP_ASSETS "/images/splash.jpg");
        std::string mac = platform::macAddress();
        auto cfg = std::make_shared<PanelConfig>();
        jobs::run([cfg, mac]() { *cfg = api::fetchConfig(mac); },
                  [this, cfg, alive = alive_]() {
                      if (!*alive) return;
                      applyConfig(*cfg);
                      Settings& s = storage::settings();
                      if (s.loggedIn && !s.user.empty() && !s.pass.empty()) {
                          status_ = "Conectando ao servidor...";
                          attemptLogin(dnsCandidates(s.dns), s.user, s.pass, [this, alive = alive_](bool ok, const std::string& err) {
                              if (!*alive) return;
                              if (!ok) { app::replaceAll(makeLogin(err)); return; }
                              status_ = "Sincronizando conteúdo...";
                              syncAndEnter([](const std::string& e) { app::replaceAll(makeLogin(e)); });
                          });
                      } else {
                          app::replaceAll(makeLogin(cfg->ok ? "" : "Sem conexão com o painel. Verifique a internet."));
                      }
                  });
    }

    ~SplashScreen() override
    {
        if (splash_) SDL_DestroyTexture(splash_);
    }

    void input(Btn) override {}

    void render() override
    {
        if (splash_) gfx::drawTextureCover(splash_, 0, 0, SCREEN_W, SCREEN_H);
        else ui::background(0);
        gfx::fillRect(0, SCREEN_H - 180, SCREEN_W, 180, {0, 0, 0, 120});
        ui::spinner(SCREEN_W / 2, SCREEN_H - 120);
        gfx::text(status_, SCREEN_W / 2, SCREEN_H - 70, 28, col::text, false, 0, Align::Center);
        gfx::text("v" APP_VERSION, SCREEN_W - 40, SCREEN_H - 50, 22, col::dim, false, 0, Align::Right);
    }
};

std::unique_ptr<Screen> makeSplash() { return std::unique_ptr<Screen>(new SplashScreen()); }

// ---------------------------------------------------------------------------
class LoginScreen : public Screen {
    bool macMode_;
    int focus_ = 0;
    bool busy_ = false;
    std::string status_;
    std::string mac_;
    uint64_t nextAutoCheck_ = 0;
    bool checking_ = false;
    bool dnsOpen_ = false;   // lista de DNS aberta
    int dnsSel_ = 0;

    // itens focáveis
    enum Item { ItUser, ItPass, ItDns, ItLogin, ItSwitch, ItCount };
    std::vector<int> items() const
    {
        if (macMode_) return {ItLogin, ItSwitch};
        return {ItUser, ItPass, ItDns, ItLogin, ItSwitch};
    }

    // DNSs disponíveis (obtidos do servidor/painel)
    static std::vector<std::string> availableDns()
    {
        std::vector<std::string> v;
        Settings& s = storage::settings();
        for (auto& d : s.dnsList) {
            std::string n = api::normalizeDns(d);
            if (n.empty()) continue;
            bool dup = false;
            for (auto& x : v) if (x == n) dup = true;
            if (!dup) v.push_back(n);
        }
        if (v.empty() && !s.dns.empty()) v.push_back(api::normalizeDns(s.dns));
        return v;
    }

    // Exibe apenas 5 caracteres após o http://
    static std::string shortDns(const std::string& d)
    {
        if (d.empty()) return "";
        size_t p = d.find("://");
        std::string scheme = p == std::string::npos ? "http://" : d.substr(0, p + 3);
        std::string host = p == std::string::npos ? d : d.substr(p + 3);
        return scheme + host.substr(0, 5);
    }

    void openDnsList()
    {
        std::vector<std::string> v = availableDns();
        if (v.empty()) { status_ = "Nenhum servidor disponível."; return; }
        std::string cur = api::normalizeDns(storage::settings().dns);
        dnsSel_ = 0;
        for (size_t k = 0; k < v.size(); k++) if (v[k] == cur) dnsSel_ = (int)k;
        dnsOpen_ = true;
    }

public:
    explicit LoginScreen(const std::string& notice) : status_(notice)
    {
        macMode_ = storage::settings().loginMode != "USER";
        mac_ = platform::macAddress();
        if (storage::settings().dns.empty()) {
            std::vector<std::string> v = availableDns();
            if (!v.empty()) storage::settings().dns = v[0];
        }
    }

    void enter() override { nextAutoCheck_ = platform::ticksMs() + 5000; }

    void loginUser(const std::string& user, const std::string& pass)
    {
        busy_ = true;
        status_ = "Aguardando conexão com o servidor...";
        auto alive = alive_;
        attemptLogin(dnsCandidates(""), user, pass, [this, alive](bool ok, const std::string& err) {
            if (!*alive) return;
            if (!ok) { busy_ = false; status_ = err; return; }
            status_ = "Sincronizando conteúdo...";
            syncAndEnter([this, alive](const std::string& e) { if (*alive) { busy_ = false; status_ = e; } });
        });
    }

    void checkMac(bool silent)
    {
        if (checking_ || busy_) return;
        checking_ = true;
        if (!silent) { busy_ = true; status_ = "Verificando MAC..."; }
        auto m = std::make_shared<MacStatus>();
        std::string mac = mac_;
        jobs::run([m, mac]() { *m = api::checkMac(mac); },
                  [this, m, silent, alive = alive_]() {
                      if (!*alive) return;
                      checking_ = false;
                      if (m->ok && m->active && !m->username.empty() && !m->password.empty()) {
                          busy_ = true;
                          status_ = "MAC ativo! Conectando...";
                          attemptLogin(dnsCandidates(m->dns), m->username, m->password,
                                       [this, alive = alive_](bool ok, const std::string& err) {
                                           if (!*alive) return;
                                           if (!ok) { busy_ = false; status_ = err; return; }
                                           status_ = "Sincronizando conteúdo...";
                                           syncAndEnter([this, alive](const std::string& e) { if (*alive) { busy_ = false; status_ = e; } });
                                       });
                          return;
                      }
                      if (!silent) {
                          busy_ = false;
                          if (!m->ok) status_ = "Sem resposta do painel. Verifique a internet.";
                          else if (m->active) status_ = "MAC registrado! Aguarde ativação no painel.";
                          else status_ = m->message.empty() ? "MAC solicitado com sucesso. Aguarde a ativação." : m->message;
                      }
                  });
    }

    void activate(int item)
    {
        Settings& s = storage::settings();
        switch (item) {
        case ItUser:
            ime::open("Usuário", s.user, ime::Type::Text, [](bool ok, const std::string& t) {
                if (ok) { storage::settings().user = t; storage::save(); }
            });
            break;
        case ItPass:
            ime::open("Senha", s.pass, ime::Type::Password, [](bool ok, const std::string& t) {
                if (ok) { storage::settings().pass = t; storage::save(); }
            });
            break;
        case ItDns:
            openDnsList();
            break;
        case ItLogin:
            if (macMode_) checkMac(false);
            else if (s.user.empty() || s.pass.empty()) status_ = "Informe usuário e senha.";
            else loginUser(s.user, s.pass);
            break;
        case ItSwitch:
            macMode_ = !macMode_;
            s.loginMode = macMode_ ? "MAC" : "USER";
            storage::save();
            focus_ = 0;
            status_.clear();
            break;
        }
    }

    void input(Btn b) override
    {
        if (busy_) return;
        if (dnsOpen_) {
            std::vector<std::string> v = availableDns();
            int cnt = (int)v.size();
            if (b == Btn::Up && dnsSel_ > 0) dnsSel_--;
            else if (b == Btn::Down && dnsSel_ < cnt - 1) dnsSel_++;
            else if (b == Btn::Cross && cnt > 0) {
                storage::settings().dns = v[(size_t)dnsSel_];
                storage::save();
                dnsOpen_ = false;
            } else if (b == Btn::Circle) dnsOpen_ = false;
            return;
        }
        std::vector<int> its = items();
        int n = (int)its.size();
        if (b == Btn::Up && focus_ > 0) focus_--;
        else if (b == Btn::Down && focus_ < n - 1) focus_++;
        else if (b == Btn::Cross) activate(its[(size_t)focus_]);
        else if (b == Btn::Circle) app::dialog("Sair", "Deseja fechar o " APP_NAME "?", {"Sim", "Não"}, [](int r) {
            if (r == 0) app::quit();
        });
    }

    void update() override
    {
        // LoginActivity: verificação automática do MAC a cada 5 s
        if (macMode_ && !busy_ && platform::ticksMs() >= nextAutoCheck_) {
            nextAutoCheck_ = platform::ticksMs() + 5000;
            checkMac(true);
        }
    }

    void field(int x, int y, int w, const std::string& label, const std::string& value, bool focused, bool secret)
    {
        gfx::text(label, x, y, 24, col::dim);
        gfx::fillRect(x, y + 34, w, 70, focused ? Color{40, 56, 100, 255} : Color{26, 30, 54, 255});
        if (focused) gfx::drawRect(x - 3, y + 31, w + 6, 76, 3, col::accent);
        std::string v = value;
        if (secret && !v.empty()) v = std::string(value.size(), '*');
        if (v.empty()) gfx::text("Pressione X para digitar", x + 20, y + 54, 26, {110, 116, 140, 255}, false, w - 40);
        else gfx::text(v, x + 20, y + 54, 28, col::text, false, w - 40);
    }

    void render() override
    {
        ui::background(170);
        int px = 560, pw = 800, py = 140, ph = 800;
        gfx::fillRect(px, py, pw, ph, {14, 18, 34, 255});
        gfx::fillRect(px, py, pw, 6, col::accent);
        SDL_Texture* lg = ui::logo(160);
        if (lg) gfx::drawTextureFit(lg, px + pw / 2 - 80, py + 30, 160, 160);
        gfx::text(macMode_ ? "Login por MAC" : "Login com usuário e senha", px + pw / 2, py + 205, 34, col::text, true, 0,
                  Align::Center);

        std::vector<int> its = items();
        auto focused = [&](int it) { return its[(size_t)focus_] == it && !busy_; };
        Settings& s = storage::settings();
        int y = py + 270;
        if (macMode_) {
            gfx::text("Endereço MAC deste PS4:", px + pw / 2, y, 26, col::dim, false, 0, Align::Center);
            gfx::text(mac_, px + pw / 2, y + 40, 54, col::accent2, true, 0, Align::Center);
            gfx::textWrapped("Cadastre este MAC no painel. O acesso é liberado automaticamente após a ativação.",
                             px + 70, y + 125, pw - 140, 24, col::dim, 3);
            y += 250;
        } else {
            field(px + 70, y, pw - 140, "Usuário", s.user, focused(ItUser), false);
            field(px + 70, y + 120, pw - 140, "Senha", s.pass, focused(ItPass), true);
            std::string dnsLabel = shortDns(s.dns);
            size_t dn = availableDns().size();
            std::string dnsTitle = dn > 1 ? "Servidor (DNS) — X para escolher (" + std::to_string(dn) + " disponíveis)" : "Servidor (DNS)";
            field(px + 70, y + 240, pw - 140, dnsTitle, dnsLabel, focused(ItDns), false);
            y += 370;
        }
        ui::button(px + 70, y, pw - 140, 72, macMode_ ? "Entrar / Verificar MAC" : "Entrar", focused(ItLogin), true);
        ui::button(px + 70, y + 92, pw - 140, 64, macMode_ ? "Usar usuário e senha" : "Usar login por MAC",
                   focused(ItSwitch));

        if (busy_) ui::spinner(px + pw / 2, py + ph - 70, 20);
        if (!status_.empty())
            gfx::text(status_, px + pw / 2, py + ph + 24, 26, busy_ ? col::text : col::accent2, false, 1500, Align::Center);
        gfx::text("v" APP_VERSION, SCREEN_W - 40, SCREEN_H - 50, 22, col::dim, false, 0, Align::Right);
        ui::footer({{"X", "Selecionar"}, {"O", dnsOpen_ ? "Fechar lista" : "Sair"}});

        if (dnsOpen_) {
            std::vector<std::string> v = availableDns();
            int cnt = (int)v.size();
            const int rowH = 70, maxRows = 7;
            int rows = cnt < maxRows ? cnt : maxRows;
            int first = dnsSel_ - rows + 1;
            if (first < 0) first = 0;
            int lw = 600, lh = rows * rowH + 90;
            int lx = SCREEN_W / 2 - lw / 2, ly = SCREEN_H / 2 - lh / 2;
            gfx::fillRect(0, 0, SCREEN_W, SCREEN_H, {0, 0, 0, 170});
            gfx::fillRect(lx, ly, lw, lh, {14, 18, 34, 255});
            gfx::fillRect(lx, ly, lw, 6, col::accent);
            gfx::text("Selecione o servidor (DNS)", lx + lw / 2, ly + 20, 28, col::text, true, 0, Align::Center);
            for (int r = 0; r < rows; r++) {
                int k = first + r;
                bool sel = k == dnsSel_;
                int ry = ly + 75 + r * rowH;
                gfx::fillRect(lx + 20, ry, lw - 40, rowH - 8, sel ? Color{40, 56, 100, 255} : Color{26, 30, 54, 255});
                if (sel) gfx::drawRect(lx + 17, ry - 3, lw - 34, rowH - 2, 3, col::accent);
                gfx::text(shortDns(v[(size_t)k]), lx + 44, ry + 14, 28, col::text);
            }
        }
    }
};

std::unique_ptr<Screen> makeLogin(const std::string& notice)
{
    return std::unique_ptr<Screen>(new LoginScreen(notice));
}
