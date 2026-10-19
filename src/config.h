#pragma once

// URL base do painel (mesma do app Android: DataRepository.kt -> PANEL_URL)
#define PANEL_URL "https://willgame.top/iptv/unitv-pro/"

#define APP_NAME "UnitV Pro"
// UNITV_PS5 é definido pelo CMakeLists.txt do build de PS5 (payload .elf)
#ifdef UNITV_PS5
#define APP_VERSION "22.0 PS5"
#else
#define APP_VERSION "22.0 PS4"
#endif

#define SCREEN_W 1920
#define SCREEN_H 1080
