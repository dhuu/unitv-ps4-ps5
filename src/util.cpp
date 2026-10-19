#include "util.h"

#include <cstdio>
#include <ctime>

namespace util {

std::string urlEncode(const std::string& s)
{
    static const char* hexd = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hexd[c >> 4];
            out += hexd[c & 15];
        }
    }
    return out;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string trimRight(const std::string& s, char c)
{
    size_t b = s.size();
    while (b > 0 && s[b - 1] == c) b--;
    return s.substr(0, b);
}

bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

uint32_t nextCodepoint(const std::string& s, size_t& i)
{
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return 0xFFFD;
    for (int k = 0; k < extra; k++) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

// Mapeia letras latinas acentuadas para ASCII
static char foldLatin(uint32_t cp)
{
    if (cp < 0x80) return (char)cp;
    static const char* tbl =
        // U+00C0..U+00FF
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF) return tbl[cp - 0xC0];
    return ' ';
}

std::string normalize(const std::string& s)
{
    std::string out;
    bool space = true;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = nextCodepoint(s, i);
        char c = foldLatin(cp);
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) { out += c; space = false; }
        else if (!space) { out += ' '; space = true; }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool containsNormalized(const std::string& haystack, const std::string& needleNormalized)
{
    if (needleNormalized.empty()) return true;
    return normalize(haystack).find(needleNormalized) != std::string::npos;
}

std::string displayCategoryLabel(const std::string& raw)
{
    std::string s = trim(raw);
    // remove BOM e caracteres de decoração no começo
    for (;;) {
        if (startsWith(s, "\xEF\xBB\xBF")) { s = s.substr(3); continue; }
        if (!s.empty() && (s[0] == '>' || s[0] == ' ' || s[0] == '-')) { s = s.substr(1); continue; }
        if (startsWith(s, "\xE2\x96\xB6") || startsWith(s, "\xE2\x96\xBA") || startsWith(s, "\xE2\x9E\xA4") ||
            startsWith(s, "\xE2\x80\xBA")) { s = s.substr(3); continue; }
        if (startsWith(s, "\xC2\xBB")) { s = s.substr(2); continue; }
        break;
    }
    s = trim(s);
    return s.empty() ? trim(raw) : s;
}

static bool hasAnyWord(const std::string& name, std::initializer_list<const char*> kws)
{
    std::string n = " " + normalize(name) + " ";
    for (const char* k : kws) {
        std::string kn = normalize(k);
        if (n.find(kn) != std::string::npos) return true;
    }
    return false;
}

bool isAdultCategory(const std::string& name)
{
    return hasAnyWord(name, {"adulto", "adultos", "xxx", "onlyfans", "erotico"});
}

bool isKidsCategory(const std::string& name)
{
    return hasAnyWord(name, {"infantil", "infantis", "kids", "kidz", "children", "child", "animacao",
                             "animation", "desenho", "cartoon", "crianca", "familia", "family",
                             "pequenos", "baby", "bebe"});
}

std::string formatTime(uint64_t ms)
{
    uint64_t t = ms / 1000;
    unsigned h = (unsigned)(t / 3600), m = (unsigned)((t / 60) % 60), s = (unsigned)(t % 60);
    char buf[32];
    if (h) snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
    else snprintf(buf, sizeof(buf), "%02u:%02u", m, s);
    return buf;
}

std::string formatClock(int64_t unixSec)
{
    time_t t = (time_t)unixSec;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::string base64Decode(const std::string& in)
{
    static int T[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) T[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[(unsigned char)a[i]] = i;
        init = true;
    }
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
    }
    return out;
}

} // namespace util
