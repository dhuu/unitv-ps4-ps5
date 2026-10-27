#pragma once
#include <string>
#include <cstdint>
#include <functional>

// Caminhos dos arquivos do app e de dados graváveis
#define APP_DATA_DIR "/data/WillProTV"
#ifdef WILLPRO_PS5
// O payload do PS5 é um .elf só: as imagens e fontes vão embutidas e são extraídas aqui no início
#define APP_ASSETS "/data/WillProTV/assets"
// O app roda como "big app" (aberto pelo websrv) a partir de BIGAPP_DIR/eboot.elf, que o
// bootstrap grava aqui junto com sce_sys/icon0.png (os dois vão embutidos no .elf enviado)
#define BIGAPP_DIR APP_DATA_DIR
#else
#define APP_ASSETS "/app0/assets"
#endif

namespace platform {

bool init();                    // carrega módulos do sistema (rede, http, ssl, áudio, ime...)
int32_t userId();               // usuário atual do console
std::string macAddress();       // MAC da interface de rede no formato AA:BB:CC:DD:EE:FF
uint64_t ticksMs();             // relógio monotônico em ms
int64_t unixTime();             // segundos desde epoch (UTC)
void sleepMs(uint32_t ms);
int freeMemoryMB();             // memória flexível livre (MB), -1 se indisponível
// Maior bloco livre de memória direta (MB), -1 se indisponível. É dela que sai a tela
// (o SDL do PS5 reserva 64 MB no SDL_GetWindowSurface); não tem relação com a flexível.
int freeDirectMemoryMB();
void setLogFile(const char* path); // antes do primeiro log (padrão: APP_DATA_DIR/log.txt)
void log(const char* fmt, ...);
// Aviso na tela do sistema (PS5: notificação do console; PS4: só grava no log)
void notify(const char* fmt, ...);
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);
void ensureDir(const std::string& path);
// Cria uma thread (desanexada) com pilha do tamanho pedido.
// A pilha padrão da libkernel do PS4 é pequena demais para rede/decodificação.
bool startThread(std::function<void()> fn, size_t stackBytes = 512 * 1024);

#ifdef WILLPRO_PS5
// Cria o ícone do WillPro-TV na tela inicial, se ainda não existir
void installShortcut();
// Enviado como payload (elfldr/autoload), o .elf não tem memória de vídeo: sobe o websrv embutido e
// reabre o app como "big app" a partir de BIGAPP_DIR. Retorna true se este processo deve encerrar.
bool bootstrapAsApp(int argc, char* argv[]);
#endif

} // namespace platform
