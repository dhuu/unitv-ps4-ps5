# UnitV Pro - PS5 (payload .elf, ps5-payload-sdk + PacBrew)
# Build: /opt/ps5-payload-sdk/bin/prospero-cmake -S . -B build-ps5 && cmake --build build-ps5
# O PS4 continua sendo compilado pelo Makefile (OpenOrbis).
cmake_minimum_required(VERSION 3.16)

project(UnitVPro CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

# Imagens e fontes vão dentro do .elf (extraídas para /data/UnitVPro/assets na primeira execução)
file(GLOB_RECURSE UNITV_ASSETS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/*)
set(UNITV_EMBED ${CMAKE_BINARY_DIR}/assets_embed.cpp)
add_custom_command(
    OUTPUT ${UNITV_EMBED}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py ${CMAKE_SOURCE_DIR}/assets ${UNITV_EMBED}
    DEPENDS ${UNITV_ASSETS} ${CMAKE_SOURCE_DIR}/scripts/embed_assets.py
    COMMENT "Embutindo assets")

file(GLOB UNITV_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_executable(UnitVPro ${UNITV_SOURCES} ${UNITV_EMBED})
set_target_properties(UnitVPro PROPERTIES OUTPUT_NAME "UnitVPro-PS5.elf")

target_compile_definitions(UnitVPro PRIVATE UNITV_PS5)
target_compile_options(UnitVPro PRIVATE -Wall -Wno-unused-function)
target_include_directories(UnitVPro PRIVATE src third_party)
# threads do FFmpeg com pilha de 1 MB (ver __wrap_pthread_create em src/player.cpp)
target_link_options(UnitVPro PRIVATE -Wl,--wrap=pthread_create -Wl,--strip-debug)

if(TARGET SDL2::SDL2)
    set(UNITV_SDL SDL2::SDL2)
else()
    set(UNITV_SDL SDL2::SDL2-static)
endif()

target_link_libraries(UnitVPro PRIVATE
    ${UNITV_SDL}
    PkgConfig::FFMPEG
    PkgConfig::FREETYPE2
    png16 # o freetype2.pc do SDK não lista a libpng, que o FreeType usa
    z
    PkgConfig::CURL
    SceAudioOut
    SceImeDialog
    SceUserService
    SceNet
    pthread
    m
)
