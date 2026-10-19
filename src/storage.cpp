#include "storage.h"
#include "json.h"
#include "platform.h"

#include <algorithm>
#include <mutex>

namespace storage {

static Settings g_settings;
static std::vector<SavedItem> g_favs;
static std::vector<SavedItem> g_hist;
static Json g_progress = Json::object();
static std::recursive_mutex g_mtx;

static const char* kPath = APP_DATA_DIR "/settings.json";

static SavedItem itemFromJson(const Json& j)
{
    SavedItem it;
    it.kind = (Kind)j["kind"].num(1);
    it.id = (int)j["id"].num(0);
    it.name = j["name"].str();
    it.icon = j["icon"].str();
    it.ext = j["ext"].str();
    it.ts = j["ts"].num(0);
    return it;
}

static Json itemToJson(const SavedItem& it)
{
    Json j = Json::object();
    j.set("kind", (int)it.kind);
    j.set("id", it.id);
    j.set("name", it.name);
    j.set("icon", it.icon);
    j.set("ext", it.ext);
    j.set("ts", (int64_t)it.ts);
    return j;
}

Settings& settings() { return g_settings; }

void load()
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    std::string text;
    if (!platform::readFile(kPath, text)) return;
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok || !j.isObject()) return;

    Settings& s = g_settings;
    s.loginMode = j["loginMode"].str("MAC");
    s.dns = j["dns"].str();
    s.user = j["user"].str();
    s.pass = j["pass"].str();
    s.parentalPin = j["parentalPin"].str("0000");
    s.liveFormat = j["liveFormat"].str("ts");
    s.loggedIn = j["loggedIn"].boolean(false);
    s.dnsList.clear();
    for (auto& d : j["dnsList"].items()) s.dnsList.push_back(d.str());

    g_favs.clear();
    for (auto& f : j["favorites"].items()) g_favs.push_back(itemFromJson(f));
    g_hist.clear();
    for (auto& h : j["history"].items()) g_hist.push_back(itemFromJson(h));
    if (j["progress"].isObject()) g_progress = j["progress"];
}

void save()
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    Settings& s = g_settings;
    Json j = Json::object();
    j.set("loginMode", s.loginMode);
    j.set("dns", s.dns);
    j.set("user", s.user);
    j.set("pass", s.pass);
    j.set("parentalPin", s.parentalPin);
    j.set("liveFormat", s.liveFormat);
    j.set("loggedIn", s.loggedIn);
    Json dl = Json::array();
    for (auto& d : s.dnsList) dl.push(d);
    j.set("dnsList", dl);
    Json fa = Json::array();
    for (auto& f : g_favs) fa.push(itemToJson(f));
    j.set("favorites", fa);
    Json hi = Json::array();
    for (auto& h : g_hist) hi.push(itemToJson(h));
    j.set("history", hi);
    j.set("progress", g_progress);
    platform::writeFile(kPath, j.dump());
}

bool isFavorite(Kind k, int id)
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    for (auto& f : g_favs) if (f.kind == k && f.id == id) return true;
    return false;
}

void toggleFavorite(const SavedItem& it)
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    for (size_t i = 0; i < g_favs.size(); i++) {
        if (g_favs[i].kind == it.kind && g_favs[i].id == it.id) {
            g_favs.erase(g_favs.begin() + (long)i);
            save();
            return;
        }
    }
    SavedItem n = it;
    n.ts = platform::unixTime();
    g_favs.insert(g_favs.begin(), n);
    save();
}

std::vector<SavedItem> favorites()
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    return g_favs;
}

void addHistory(const SavedItem& it)
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_hist.erase(std::remove_if(g_hist.begin(), g_hist.end(),
                                [&](const SavedItem& h) { return h.kind == it.kind && h.id == it.id; }),
                 g_hist.end());
    SavedItem n = it;
    n.ts = platform::unixTime();
    g_hist.insert(g_hist.begin(), n);
    if (g_hist.size() > 60) g_hist.resize(60);
    save();
}

std::vector<SavedItem> history()
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    return g_hist;
}

void clearHistory()
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_hist.clear();
    save();
}

// Regras do PlaybackProgressStore.kt: ignora trechos curtos (<30s) e o final (>95%)
void saveProgress(const std::string& key, uint64_t posMs, uint64_t durMs)
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (posMs < 30000 || (durMs > 0 && posMs > durMs * 95 / 100)) {
        g_progress.remove(key);
    } else {
        Json p = Json::object();
        p.set("pos", (int64_t)posMs);
        p.set("dur", (int64_t)durMs);
        p.set("ts", (int64_t)platform::unixTime());
        g_progress.set(key, p);
    }
    save();
}

uint64_t getProgress(const std::string& key)
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    return (uint64_t)g_progress[key]["pos"].num(0);
}

void clearProgress(const std::string& key)
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_progress.remove(key);
    save();
}

void clearSession()
{
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_settings.user.clear();
    g_settings.pass.clear();
    g_settings.loggedIn = false;
    g_hist.clear();
    g_favs.clear();
    g_progress = Json::object();
    save();
}

} // namespace storage
