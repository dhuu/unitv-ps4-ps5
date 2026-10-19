#include "api.h"
#include "config.h"
#include "http.h"
#include "json.h"
#include "util.h"
#include "platform.h"

#include <algorithm>

namespace api {

static Session g_session;
Session& session() { return g_session; }

std::string normalizeDns(const std::string& dnsIn)
{
    std::string dns = util::trim(dnsIn);
    if (dns.empty()) return dns;
    if (dns.find("://") == std::string::npos) dns = "http://" + dns;
    return util::trimRight(dns, '/');
}

// GET com retry para 429/503 (equivalente ao apiRetryInterceptor do NetworkManager.kt)
static http::Response getWithRetry(const std::string& url)
{
    http::Response r;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = http::get(url);
        if (r.status == 429 || r.status == 503) {
            platform::sleepMs(1000u << attempt);
            continue;
        }
        if (r.status == 0 && attempt < 1) {
            platform::sleepMs(800);
            continue;
        }
        break;
    }
    return r;
}

static bool getJson(const std::string& url, Json& out)
{
    http::Response r = getWithRetry(url);
    if (!r.ok()) {
        platform::log("GET falhou (%d %s): %s", r.status, r.error.c_str(), url.c_str());
        return false;
    }
    bool ok = false;
    out = Json::parse(r.body, &ok);
    return ok;
}

static std::string playerApi(const std::string& dns, const std::string& user, const std::string& pass,
                             const std::string& extra = "")
{
    return normalizeDns(dns) + "/player_api.php?username=" + util::urlEncode(user) +
           "&password=" + util::urlEncode(pass) + extra;
}

static std::string sessApi(const std::string& extra)
{
    return playerApi(g_session.dns, g_session.user, g_session.pass, extra);
}

PanelConfig fetchConfig(const std::string& mac)
{
    PanelConfig c;
    Json j;
    if (!getJson(std::string(PANEL_URL) + "api.php?action=get_config&mac=" + util::urlEncode(mac), j)) return c;
    if (!j.isObject()) return c;
    c.ok = true;
    const Json& dns = j["dns"];
    if (dns.isArray()) {
        for (auto& d : dns.items()) {
            std::string s = util::trim(d.str());
            if (!s.empty()) c.dns.push_back(s);
        }
    } else if (dns.isString() && !dns.str().empty()) {
        c.dns.push_back(dns.str());
    }
    c.status = j["status"].str();
    c.logo = j["app_logo"].str();
    c.background = j["app_background"].str();
    c.message = j.any({"message_global", "global_message", "messageGlobal", "alert", "alerta",
                       "mensagem", "mensagem_global", "aviso", "notice"}).str();
    c.whatsapp = j["support_whatsapp"].str();
    c.eventsUrl = j["events_url"].str();
    c.newsBanner = j["news_banner_url"].str();
    return c;
}

MacStatus checkMac(const std::string& mac)
{
    MacStatus m;
    Json j;
    if (!getJson(std::string(PANEL_URL) + "api.php?action=check_mac&mac=" + util::urlEncode(mac), j)) return m;
    m.ok = true;
    m.active = j["active"].boolean(false);
    m.message = j["message"].str();
    m.username = j["username"].str();
    m.password = j["password"].str();
    m.dns = j["dns"].str();
    return m;
}

LoginResult login(const std::string& dns, const std::string& user, const std::string& pass)
{
    LoginResult r;
    http::Response resp = getWithRetry(playerApi(dns, user, pass));
    if (!resp.ok()) {
        r.error = resp.status ? ("HTTP " + std::to_string(resp.status)) : resp.error;
        return r;
    }
    bool ok = false;
    Json j = Json::parse(resp.body, &ok);
    if (!ok) { r.error = "Resposta inválida do servidor"; return r; }
    const Json& ui = j["user_info"];
    r.status = ui["status"].str();
    r.expDate = ui["exp_date"].str();
    r.maxConnections = ui["max_connections"].str();
    r.ok = (r.status == "Active") || (ui["auth"].num(0) == 1 && r.status.empty());
    if (!r.ok && r.error.empty()) r.error = r.status.empty() ? "Usuário ou senha inválidos" : ("Conta: " + r.status);
    return r;
}

static const char* actionFor(Kind k, bool cats)
{
    switch (k) {
    case Kind::Live: return cats ? "get_live_categories" : "get_live_streams";
    case Kind::Series: return cats ? "get_series_categories" : "get_series";
    default: return cats ? "get_vod_categories" : "get_vod_streams";
    }
}

bool categories(Kind kind, std::vector<Category>& out)
{
    Json j;
    if (!getJson(sessApi(std::string("&action=") + actionFor(kind, true)), j) || !j.isArray()) return false;
    out.clear();
    out.reserve(j.size());
    for (auto& c : j.items()) {
        Category cat;
        cat.id = c["category_id"].str();
        cat.name = util::displayCategoryLabel(c["category_name"].str());
        if (!cat.id.empty()) out.push_back(std::move(cat));
    }
    return true;
}

bool streams(Kind kind, const std::string& categoryId, std::vector<Stream>& out)
{
    std::string extra = std::string("&action=") + actionFor(kind, false);
    if (!categoryId.empty()) extra += "&category_id=" + util::urlEncode(categoryId);
    Json j;
    if (!getJson(sessApi(extra), j) || !j.isArray()) return false;
    out.clear();
    out.reserve(j.size());
    for (auto& s : j.items()) {
        Stream st;
        st.kind = kind;
        st.name = s["name"].str();
        if (kind == Kind::Series) {
            st.id = (int)s["series_id"].num(0);
            st.icon = s["cover"].str();
        } else {
            st.id = (int)s["stream_id"].num(0);
            st.icon = s["stream_icon"].str();
        }
        st.categoryId = s["category_id"].str();
        st.ext = s["container_extension"].str();
        st.rating = s["rating"].str();
        if (st.id) out.push_back(std::move(st));
    }
    return true;
}

static std::string firstOf(const Json& v)
{
    if (v.isArray()) return v.size() ? v[(size_t)0].str() : "";
    return v.str();
}

VodInfo vodInfo(int vodId)
{
    VodInfo v;
    Json j;
    if (!getJson(sessApi("&action=get_vod_info&vod_id=" + std::to_string(vodId)), j)) return v;
    const Json& info = j["info"];
    v.ok = true;
    v.plot = info.any({"plot", "description"}).str();
    v.cast = info.any({"cast", "actors"}).str();
    v.director = info["director"].str();
    v.genre = info["genre"].str();
    v.releaseDate = info.any({"releaseDate", "releasedate", "release_date"}).str();
    v.rating = info["rating"].str();
    v.duration = info["duration"].str();
    v.cover = info.any({"movie_image", "cover_big"}).str();
    v.backdrop = firstOf(info["backdrop_path"]);
    v.ext = j["movie_data"]["container_extension"].str();
    v.name = j["movie_data"]["name"].str();
    return v;
}

SeriesInfo seriesInfo(int seriesId)
{
    SeriesInfo s;
    Json j;
    if (!getJson(sessApi("&action=get_series_info&series_id=" + std::to_string(seriesId)), j)) return s;
    const Json& info = j["info"];
    s.ok = true;
    s.name = info["name"].str();
    s.cover = info["cover"].str();
    s.plot = info["plot"].str();
    s.cast = info["cast"].str();
    s.genre = info["genre"].str();
    s.releaseDate = info.any({"releaseDate", "release_date"}).str();
    s.rating = info["rating"].str();
    s.backdrop = firstOf(info["backdrop_path"]);

    // "episodes" pode vir como objeto {"1":[...]} ou como array de arrays
    const Json& eps = j["episodes"];
    auto addList = [&](const Json& list, int fallbackSeason) {
        for (auto& e : list.items()) {
            Episode ep;
            ep.id = e["id"].str();
            ep.season = (int)e["season"].num(fallbackSeason);
            ep.num = (int)e["episode_num"].num(0);
            ep.title = e["title"].str();
            ep.ext = e["container_extension"].str("mp4");
            ep.image = e["info"]["movie_image"].str();
            ep.plot = e["info"]["plot"].str();
            ep.duration = e["info"]["duration"].str();
            if (!ep.id.empty()) s.episodes[ep.season].push_back(std::move(ep));
        }
    };
    if (eps.isObject()) {
        for (auto& key : eps.objectKeys()) addList(eps[key], atoi(key.c_str()));
    } else if (eps.isArray()) {
        for (size_t i = 0; i < eps.size(); i++) addList(eps[i], (int)i + 1);
    }
    for (auto& kv : s.episodes) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const Episode& a, const Episode& b) { return a.num < b.num; });
        s.seasons.push_back(kv.first);
    }
    return s;
}

// Títulos/descrições do get_short_epg vêm em Base64
static std::string maybeB64(const std::string& s)
{
    if (s.empty()) return s;
    for (char c : s) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '=' || c == '\n' || c == '\r';
        if (!ok) return s;
    }
    std::string d = util::base64Decode(s);
    return d.empty() ? s : d;
}

std::vector<EpgEntry> shortEpg(int streamId)
{
    std::vector<EpgEntry> out;
    Json j;
    if (!getJson(sessApi("&action=get_short_epg&limit=4&stream_id=" + std::to_string(streamId)), j)) return out;
    for (auto& e : j["epg_listings"].items()) {
        EpgEntry en;
        en.title = maybeB64(e["title"].str());
        en.description = maybeB64(e["description"].str());
        en.start = e["start_timestamp"].num(0);
        en.end = e["stop_timestamp"].num(0);
        out.push_back(std::move(en));
    }
    return out;
}

std::string liveUrl(int streamId)
{
    const std::string& fmt = storage::settings().liveFormat;
    return normalizeDns(g_session.dns) + "/live/" + g_session.user + "/" + g_session.pass + "/" +
           std::to_string(streamId) + "." + (fmt.empty() ? "m3u8" : fmt);
}

std::string vodUrl(int streamId, const std::string& ext)
{
    return normalizeDns(g_session.dns) + "/movie/" + g_session.user + "/" + g_session.pass + "/" +
           std::to_string(streamId) + "." + (ext.empty() ? "mp4" : ext);
}

std::string episodeUrl(const std::string& episodeId, const std::string& ext)
{
    return normalizeDns(g_session.dns) + "/series/" + g_session.user + "/" + g_session.pass + "/" +
           episodeId + "." + (ext.empty() ? "mp4" : ext);
}

} // namespace api
