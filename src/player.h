#pragma once
// Player interno baseado no FFmpeg (substitui o Media3/ExoPlayer do Android).
// Toca MPEG-TS, HLS, MP4, MKV, AVI... (H.264, HEVC, MPEG-2, AAC, AC3, MP3...) via HTTP/HTTPS.
#include <string>
#include <cstdint>
#include <SDL2/SDL.h>

namespace player {

enum class State { Idle, Opening, Buffering, Playing, Paused, Ended, Error };

bool open(const std::string& url);
void close();
bool isOpen();
State state();
std::string errorText();

uint64_t positionMs();
uint64_t durationMs();      // 0 para ao vivo
bool hasVideoFrame();
void pause();
void resume();
void togglePause();
void seekTo(uint64_t ms);

// Atualiza o frame de vídeo (se houver novo) e desenha na superfície da tela.
void render(SDL_Surface* dst);

} // namespace player
