#pragma once
// Port de XtreamApiService.kt + PanelApiService.kt + DataRepository.kt
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#include "storage.h"

struct Category {
    std::string id;
    std::string name;
};

struct Stream {
    Kind kind = Kind::Vod;
    int id = 0;
    std::string name;
    std::string icon;
    std::string categoryId;
    std::string ext;      // container_extension (filmes)
    std::string rating;
};

struct PanelConfig {
    bool ok = false;
    std::vector<std::string> dns;
    std::string status, logo, background, message, whatsapp, eventsUrl, newsBanner;
};

struct MacStatus {
    bool ok = false;      // resposta recebida
    bool active = false;
    std::string message, username, password, dns;
};

struct LoginResult {
    bool ok = false;
    std::string status;   // "Active", "Expired"...
    std::string expDate;
    std::string maxConnections;
    std::string error;
};

struct VodInfo {
    bool ok = false;
    std::string plot, cast, director, genre, releaseDate, rating, duration, cover, backdrop, ext, name;
};

struct Episode {
    std::string id;
    int season = 0;
    int num = 0;
    std::string title, ext, image, plot, duration;
};

struct SeriesInfo {
    bool ok = false;
    std::string name, cover, plot, cast, genre, releaseDate, rating, backdrop;
    std::vector<int> seasons;                 // temporadas ordenadas
    std::map<int, std::vector<Episode>> episodes;
};

struct EpgEntry {
    std::string title, description;
    int64_t start = 0, end = 0;
};

namespace api {

// Sessão Xtream ativa
struct Session {
    std::string dns, user, pass;
};
Session& session();

PanelConfig fetchConfig(const std::string& mac);
MacStatus checkMac(const std::string& mac);
LoginResult login(const std::string& dns, const std::string& user, const std::string& pass);

bool categories(Kind kind, std::vector<Category>& out);
bool streams(Kind kind, const std::string& categoryId, std::vector<Stream>& out);
VodInfo vodInfo(int vodId);
SeriesInfo seriesInfo(int seriesId);
std::vector<EpgEntry> shortEpg(int streamId);

std::string normalizeDns(const std::string& dns);
std::string liveUrl(int streamId);
std::string vodUrl(int streamId, const std::string& ext);
std::string episodeUrl(const std::string& episodeId, const std::string& ext);

} // namespace api
