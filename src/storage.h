#pragma once
// Persistência local (substitui SharedPreferences do Android): /data/WillProTV/settings.json
#include <string>
#include <vector>
#include <cstdint>

enum class Kind { Live = 0, Vod = 1, Series = 2, Episode = 3 };

struct SavedItem {
    Kind kind = Kind::Vod;
    int id = 0;
    std::string name;
    std::string icon;
    std::string ext;
    int64_t ts = 0;
};

struct Settings {
    std::string loginMode = "MAC";   // "MAC" ou "USER"
    std::string dns;                 // DNS atual (servidor Xtream)
    std::vector<std::string> dnsList;
    std::string user, pass;
    std::string parentalPin = "0000";
    std::string liveFormat = "ts";   // "ts" (MPEG-TS, como no Android) ou "m3u8" (HLS)
    bool loggedIn = false;
};

namespace storage {

void load();
void save();
Settings& settings();

// Favoritos (FavoriteManager.kt)
bool isFavorite(Kind k, int id);
void toggleFavorite(const SavedItem& it);
std::vector<SavedItem> favorites();

// Histórico / continuar assistindo (HistoryManager.kt)
void addHistory(const SavedItem& it);
std::vector<SavedItem> history();
void clearHistory();

// Progresso de reprodução (PlaybackProgressStore.kt): chave vod_{id} / series_{serie}_{ep}
void saveProgress(const std::string& key, uint64_t posMs, uint64_t durMs);
uint64_t getProgress(const std::string& key);
void clearProgress(const std::string& key);

void clearSession(); // logout: limpa credenciais, histórico e progresso

} // namespace storage
