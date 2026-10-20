# WillPro-TV — PS4 e PS5

Versão para PlayStation 4 (homebrew) do app Android **WillPro-TV** (`com.willdev.unitvpro`).
É um port em C++ que usa o [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain),
o SDL2 para desenhar a interface e um **player interno baseado no FFmpeg** (compilado no próprio CI por `scripts/build-ffmpeg.sh`).

> **Requisito:** o PS4 precisa estar desbloqueado (GoldHEN ou outro HEN) para instalar pacotes `.pkg` fake-signed.

## Funcionalidades (port do app Android)

| Android | PS4 |
|---|---|
| `SplashActivity` + `get_config` do painel (DNS, logo, fundo, mensagem global) | `SplashScreen` |
| `LoginActivity`: login por **MAC** (com verificação automática a cada 5 s) e por **usuário/senha** | `LoginScreen` (usa o MAC de rede do PS4) |
| Fallback de DNS entre os servidores do painel (`DnsManager`) | `attemptLogin` |
| TV ao vivo com categorias, lista de canais e EPG (`get_short_epg`) | `BrowseScreen` + zapping no player (Cima/Baixo, L1/R1) |
| Filmes / Séries / Kids / Adultos (com senha parental, padrão `0000`) | `BrowseScreen` com grade de pôsteres |
| Detalhes do filme (`get_vod_info`) e da série (`get_series_info`) com temporadas/episódios | `MovieDetailsScreen`, `SeriesDetailsScreen` |
| Continuar de onde parou (Sim/Não) e próximo episódio | `startPlayback`, `PlayerScreen` |
| Favoritos, histórico, busca | `SavedScreen`, `SearchScreen` |
| Configurações: trocar DNS, senha parental, limpar histórico, recarregar listas, sair | `SettingsScreen` |

A URL do painel fica em [`src/config.h`](src/config.h) (`PANEL_URL`), igual ao `DataRepository.kt` do Android.

## Controles

| Botão | Ação |
|---|---|
| **X** | Selecionar / Play-Pause |
| **O** | Voltar |
| **Quadrado** | Favoritar · próximo episódio (no player) |
| **Triângulo** | Buscar · sinopse |
| **L1 / R1** | Página anterior/próxima · trocar canal · ±1 min |
| **← / →** (player) | ±10 s |
| **L2 / R2** (player) | ±5 min |
| **OPTIONS** | Ajustes |

## Compilar no GitHub (recomendado)

1. Crie um repositório no GitHub e envie **o conteúdo desta pasta** (`WillProTV-PS4`) para a raiz dele.
2. O workflow [`.github/workflows/build-ps4.yml`](.github/workflows/build-ps4.yml) roda sozinho a cada push:
   baixa o OpenOrbis Toolchain, compila e gera o `.pkg`.
3. Baixe o pacote em **Actions → (execução) → Artifacts → `WillProTV-PS4-pkg`**.
4. Para gerar uma *Release* com o `.pkg` anexado, crie uma tag: `git tag v1.0 && git push --tags`.

## Compilar localmente (Linux/WSL)

```bash
# 1) Toolchain
mkdir -p ~/oo && curl -sL https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz | tar xz -C ~/oo
export OO_PS4_TOOLCHAIN=~/oo/OpenOrbis/PS4Toolchain
export DOTNET_ROLL_FORWARD=Major     # PkgTool.Core precisa de .NET

# 2) Módulos do sistema
mkdir -p sce_module sce_sys/about
cp $OO_PS4_TOOLCHAIN/samples/hello_world/sce_module/*.prx sce_module/
cp $OO_PS4_TOOLCHAIN/samples/hello_world/sce_sys/about/right.sprx sce_sys/about/

# 3) Build (gera IV0000-WLTV00001_00-WILLPROTV0000000.pkg)
make CCX=clang++-18 LD=ld.lld-18
```

## Instalar no PS4

1. Copie o `.pkg` para um pendrive (FAT32/exFAT) ou use o *Remote Package Installer* / FTP do GoldHEN.
2. No PS4: **Debug Settings → Game → Package Installer** (ou o instalador do GoldHEN).
3. O app aparece na tela inicial como **WillPro-TV** (`WLTV00001`).

Os dados (login, favoritos, histórico, progresso) ficam em `/data/WillProTV/settings.json`.

## PS5 (payload .elf)

O mesmo código gera um payload nativo do PS5 (`WillProTV-PS5.elf`) com o
[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) e as bibliotecas do
[PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) (SDL2, FFmpeg, FreeType, curl).
As diferenças ficam em blocos `#ifdef WILLPRO_PS5`; o `.pkg` do PS4 não roda no PS5.

- **Compilar no GitHub:** o workflow [`.github/workflows/build-ps5.yml`](.github/workflows/build-ps5.yml)
  roda junto com o do PS4. Baixe em **Actions → (execução) → Artifacts → `WillProTV-PS5-elf`**.
- **Compilar localmente (Linux/WSL):**
  ```bash
  curl -sL https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.41.1/ps5-payload-dev.tar.gz | sudo tar xz -C /
  /opt/ps5-payload-sdk/bin/prospero-cmake -S . -B build-ps5 && cmake --build build-ps5
  ```
- **Rodar:** com o PS5 desbloqueado e um *ELF loader* ativo (elfldr do etaHEN/kstuff na porta 9021),
  envie o arquivo: `socat -t 9999999 - TCP:IP_DO_PS5:9021 < WillProTV-PS5.elf`
  (ou pelo seu enviador de payloads). Imagens e fontes vão dentro do `.elf` e são extraídas para
  `/data/WillProTV/assets` na primeira execução. Login, favoritos e o `log.txt` ficam em `/data/WillProTV`,
  como no PS4.

## Limitações conhecidas

- A decodificação de vídeo é por software (FFmpeg, 6 threads). 720p/1080p H.264 devem rodar; HEVC 4K ou
  1080p60 podem descartar quadros.
- A conversão de cor e a interface usam a CPU (renderizador por software do SDL).
- O banner TMDB, a WebView de eventos e o player "mini" das telas de detalhes do Android não foram portados.

## Estrutura

```
src/
  main.cpp            ponto de entrada
  platform.*          módulos do sistema, MAC, arquivos
  http.*              cliente HTTP/HTTPS (sceHttp no PS4, curl no PS5: http_curl.cpp)
  json.*              parser JSON tolerante
  api.*               API Xtream (player_api.php) + painel (api.php)
  catalog.*           cache de categorias/streams
  storage.*           configurações, favoritos, histórico e progresso
  gfx.*               SDL2 + FreeType + carregamento assíncrono de imagens (stb_image)
  input.*             controle DualShock 4
  ime.*               teclado virtual do sistema
  player.*            player interno (FFmpeg + swscale + AudioOut)
  ui.h / app.cpp      pilha de telas, diálogos, componentes
  screens_*.cpp       telas
assets/               fontes (DejaVu) e imagens
sce_sys/              ícone (icon0.png) e fundo (pic1.png) do PS4
third_party/          stb_image.h
scripts/build-ffmpeg.sh  compila o FFmpeg (LGPL) para o PS4
scripts/embed_assets.py  embute assets/ no .elf do PS5
CMakeLists.txt        build do PS5 (o PS4 usa o Makefile)
```
