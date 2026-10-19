// UnitV Pro para PS4 (OpenOrbis Toolchain) e PS5 (ps5-payload-sdk) — port do app Android (com.willdev.unitvpro)
#include "platform.h"
#include "http.h"
#include "gfx.h"
#include "input.h"
#include "jobs.h"
#include "storage.h"
#include "ui.h"
#include "screens.h"
#include "player.h"
#include "crash.h"

#include <cstdlib>
#ifndef UNITV_PS5
#include <orbis/SystemService.h>
#endif

static void appMain()
{
    platform::init();
    platform::log("Iniciando UnitV Pro");
#ifdef UNITV_PS5
    platform::notify("UnitV Pro: iniciando...");
#endif
    crash::install();

    if (!gfx::init()) {
        platform::log("Falha ao iniciar gráficos");
#ifdef UNITV_PS5
        // encerra para não segurar memória: um novo envio do payload começa do zero
        platform::notify("UnitV Pro: falha ao iniciar a tela (veja /data/UnitVPro/log.txt)");
        _Exit(1);
#endif
        for (;;) platform::sleepMs(1000);
    }
    if (!http::init()) platform::log("Falha ao iniciar rede");

    input::init();
    jobs::init(3);
    images::init();
    storage::load();

    app::push(makeSplash());
    app::run();

    player::close();
    gfx::shutdown();
#ifdef UNITV_PS5
    // payload: encerrar o processo devolve a tela ao sistema
    platform::log("Encerrando");
    _Exit(0);
#else
    // Encerra o app e volta ao menu do PS4
    sceSystemServiceLoadExec("exit", NULL);
#endif
    for (;;) platform::sleepMs(1000);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    // A thread principal do processo tem pilha limitada: a interface + conversão de vídeo
    // rodam numa thread com pilha de 8 MB.
    if (!platform::startThread(appMain, 8 * 1024 * 1024)) appMain();
    for (;;) platform::sleepMs(1000);
    return 0;
}
