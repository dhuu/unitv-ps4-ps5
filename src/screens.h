#pragma once
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

#include "ui.h"
#include "api.h"

// Item reproduzível (canal, filme ou episódio)
struct PlayItem {
    Kind kind = Kind::Vod;
    int id = 0;               // stream_id / id do episódio
    std::string episodeId;    // episódios: id em texto
    int seriesId = 0;
    std::string name;
    std::string icon;
    std::string ext;
    std::string seriesName;   // episódios: para o histórico
    std::string seriesCover;
    std::string progressKey() const;
    std::string url() const;
};

std::unique_ptr<Screen> makeSplash();
std::unique_ptr<Screen> makeLogin(const std::string& notice = "");
std::unique_ptr<Screen> makeHome();
enum class BrowseFilter { All, Kids, Adult };
std::unique_ptr<Screen> makeBrowse(Kind kind, BrowseFilter filter);
std::unique_ptr<Screen> makeMovieDetails(const Stream& s);
std::unique_ptr<Screen> makeSeriesDetails(const Stream& s);
std::unique_ptr<Screen> makePlayer(std::vector<PlayItem> list, int index, uint64_t startMs);
std::unique_ptr<Screen> makeSearch();
std::unique_ptr<Screen> makeSaved(bool favorites);
std::unique_ptr<Screen> makeSettings();

// Abre o player perguntando se deseja continuar de onde parou (PlaybackProgressStore.showResumePlaybackDialog)
void startPlayback(std::vector<PlayItem> list, int index);

// Login com fallback de DNS (LoginActivity.performLogin). done roda na thread principal.
void attemptLogin(std::vector<std::string> dnsCandidates, const std::string& user, const std::string& pass,
                  std::function<void(bool ok, const std::string& error)> done);
void logout();
// Abre um item salvo/encontrado: canal toca direto, filme/série abre detalhes
void openStream(const Stream& s);
void drawKindBadge(Kind k, int x, int y);

extern LoginResult g_account;
