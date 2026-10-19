#pragma once
#include <string>
#include <cstdint>
#include <functional>

// Caminhos dos arquivos do app e de dados graváveis
#define APP_DATA_DIR "/data/UnitVPro"
#ifdef UNITV_PS5
// O payload do PS5 é um .elf só: as imagens e fontes vão embutidas e são extraídas aqui no início
#define APP_ASSETS "/data/UnitVPro/assets"
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
void log(const char* fmt, ...);
// Aviso na tela do sistema (PS5: notificação do console; PS4: só grava no log)
void notify(const char* fmt, ...);
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);
void ensureDir(const std::string& path);
// Cria uma thread (desanexada) com pilha do tamanho pedido.
// A pilha padrão da libkernel do PS4 é pequena demais para rede/decodificação.
bool startThread(std::function<void()> fn, size_t stackBytes = 512 * 1024);

} // namespace platform
