#pragma once
#include <string>

namespace http {

struct Response {
    int status = 0;          // código HTTP (0 = falha de rede)
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

bool init();
// GET síncrono (chamar fora da thread de UI). maxBytes limita o download (0 = sem limite).
Response get(const std::string& url, size_t maxBytes = 0, int timeoutSec = 25);

} // namespace http

#include <atomic>
#include <cstdint>
#include <vector>
#include <mutex>

namespace http {

void contexts(int& httpCtx, int& sslCtx);
// Segue redirecionamentos 30x manualmente e retorna a URL final
std::string resolveRedirects(const std::string& url);

// Download sequencial a partir de um deslocamento (Range: bytes=from-)
struct Stream {
    int tpl = -1, conn = -1, req = -1; // PS4 (sceHttp)
    void* impl = nullptr;               // PS5 (curl)
    std::atomic<bool> aborted{false};   // PS5: pedido de interrupção vindo de outra thread
    uint64_t pos = 0;     // próximo byte que será lido
    uint64_t total = 0;   // tamanho total do arquivo (0 = desconhecido)
    bool rangeOk = false; // servidor aceita Range (permite avançar/voltar)
    bool open = false;
};
bool streamOpen(Stream& st, const std::string& url, uint64_t from);
std::string lastStreamError(); // diagnóstico da última falha de streamOpen
int streamRead(Stream& st, void* buf, uint32_t len); // bytes lidos, 0 = fim, <0 = erro
void streamClose(Stream& st);
// Interrompe uma leitura bloqueada em outra thread (ao fechar o player)
void streamAbort(Stream& st);

// Arquivo remoto com acesso aleatório e cache de blocos.
// Usa blocos em cache e reaproveita a conexão quando a leitura é sequencial.
class RangeFile {
public:
    bool open(const std::string& url);
    int read(uint8_t* buf, uint64_t pos, uint32_t len);
    uint64_t size() const { return size_; }
    void close();
    ~RangeFile() { close(); }

private:
    struct Block {
        uint64_t idx = 0;
        std::vector<uint8_t> data;
        uint64_t used = 0;
    };
    bool fetch(uint64_t idx, Block& out);
    std::string url_;
    uint64_t size_ = 0;
    uint64_t tick_ = 0;
    Stream st_;
    std::vector<Block> cache_;
    std::mutex mtx_;
};

} // namespace http
