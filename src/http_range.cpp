// Arquivo remoto com acesso aleatório (cache de blocos sobre http::Stream); igual no PS4 e no PS5
#include "http.h"
#include "platform.h"

#include <algorithm>
#include <cstring>

namespace http {

static const uint64_t kBlock = 512 * 1024;
static const size_t kMaxBlocks = 24; // ~12 MB de cache

bool RangeFile::open(const std::string& url)
{
    std::lock_guard<std::mutex> lk(mtx_);
    url_ = url;
    cache_.clear();
    if (!streamOpen(st_, url_, 0)) return false;
    size_ = st_.total;
    platform::log("RangeFile aberto, tamanho=%llu", (unsigned long long)size_);
    return size_ > 0;
}

void RangeFile::close()
{
    std::lock_guard<std::mutex> lk(mtx_);
    streamClose(st_);
    cache_.clear();
}

bool RangeFile::fetch(uint64_t idx, Block& out)
{
    uint64_t start = idx * kBlock;
    if (start >= size_) return false;
    uint64_t want = std::min<uint64_t>(kBlock, size_ - start);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!st_.open || st_.pos != start) {
            if (!streamOpen(st_, url_, start)) { platform::sleepMs(300); continue; }
        }
        out.idx = idx;
        out.data.resize((size_t)want);
        uint64_t got = 0;
        while (got < want) {
            int n = streamRead(st_, out.data.data() + got, (uint32_t)(want - got));
            if (n <= 0) break;
            got += (uint64_t)n;
        }
        if (got == want) return true;
        streamClose(st_); // conexão caiu: tenta reabrir
    }
    return false;
}

int RangeFile::read(uint8_t* buf, uint64_t pos, uint32_t len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (pos >= size_) return 0;
    if (pos + len > size_) len = (uint32_t)(size_ - pos);
    uint32_t done = 0;
    while (done < len) {
        uint64_t p = pos + done;
        uint64_t idx = p / kBlock;
        Block* b = nullptr;
        for (auto& c : cache_)
            if (c.idx == idx && !c.data.empty()) { b = &c; break; }
        if (!b) {
            Block nb;
            if (!fetch(idx, nb)) return done ? (int)done : -1;
            if (cache_.size() >= kMaxBlocks) {
                size_t old = 0;
                for (size_t i = 1; i < cache_.size(); i++)
                    if (cache_[i].used < cache_[old].used) old = i;
                cache_[old] = std::move(nb);
                b = &cache_[old];
            } else {
                cache_.push_back(std::move(nb));
                b = &cache_.back();
            }
        }
        b->used = ++tick_;
        uint64_t off = p - idx * kBlock;
        uint32_t n = (uint32_t)std::min<uint64_t>(b->data.size() - off, len - done);
        memcpy(buf + done, b->data.data() + off, n);
        done += n;
    }
    return (int)done;
}

} // namespace http
