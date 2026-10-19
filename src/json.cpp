#include "json.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>

static const Json kNull;

struct JsonParser {
    const char* p;
    const char* end;
    int depth = 0;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    uint32_t read4() {
        if (end - p < 4) return 0xFFFD;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            int h = hex(p[i]);
            if (h < 0) return 0xFFFD;
            v = (v << 4) | (uint32_t)h;
        }
        p += 4;
        return v;
    }

    bool parseString(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { out += c; continue; }
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t cp = read4();
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo = read4();
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: out += e; break;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }

    bool parseValue(Json& v) {
        ws();
        if (p >= end) return false;
        if (++depth > 200) return false;
        bool ok = true;
        char c = *p;
        if (c == '{') {
            p++;
            v.t = Json::Object;
            ws();
            if (p < end && *p == '}') { p++; depth--; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!parseString(key)) { ok = false; break; }
                ws();
                if (p >= end || *p != ':') { ok = false; break; }
                p++;
                Json child;
                if (!parseValue(child)) { ok = false; break; }
                v.keys.push_back(std::move(key));
                v.vals.push_back(std::move(child));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            p++;
            v.t = Json::Array;
            ws();
            if (p < end && *p == ']') { p++; depth--; return true; }
            for (;;) {
                Json child;
                if (!parseValue(child)) { ok = false; break; }
                v.arr.push_back(std::move(child));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; break; }
                ok = false; break;
            }
        } else if (c == '"') {
            v.t = Json::String;
            ok = parseString(v.s);
        } else if (c == 't' && end - p >= 4 && !strncmp(p, "true", 4)) {
            v.t = Json::Bool; v.b = true; p += 4;
        } else if (c == 'f' && end - p >= 5 && !strncmp(p, "false", 5)) {
            v.t = Json::Bool; v.b = false; p += 5;
        } else if (c == 'n' && end - p >= 4 && !strncmp(p, "null", 4)) {
            v.t = Json::Null; p += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            char buf[64];
            size_t i = 0;
            while (p < end && i < sizeof(buf) - 1 &&
                   (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9')))
                buf[i++] = *p++;
            buf[i] = 0;
            v.t = Json::Number;
            v.n = strtod(buf, nullptr);
        } else {
            ok = false;
        }
        depth--;
        return ok;
    }
};

Json Json::parse(const std::string& text, bool* ok)
{
    JsonParser jp;
    jp.p = text.data();
    jp.end = text.data() + text.size();
    // pula BOM UTF-8
    if (jp.end - jp.p >= 3 && (unsigned char)jp.p[0] == 0xEF && (unsigned char)jp.p[1] == 0xBB && (unsigned char)jp.p[2] == 0xBF)
        jp.p += 3;
    Json v;
    bool r = jp.parseValue(v);
    if (ok) *ok = r;
    return v;
}

std::string Json::str(const std::string& def) const
{
    switch (t) {
    case String: return s;
    case Number: {
        char buf[64];
        if (std::fabs(n - (double)(int64_t)n) < 1e-9) snprintf(buf, sizeof(buf), "%lld", (long long)n);
        else snprintf(buf, sizeof(buf), "%g", n);
        return buf;
    }
    case Bool: return b ? "true" : "false";
    default: return def;
    }
}

int64_t Json::num(int64_t def) const
{
    switch (t) {
    case Number: return (int64_t)n;
    case String: {
        if (s.empty()) return def;
        char* e = nullptr;
        double d = strtod(s.c_str(), &e);
        if (e == s.c_str()) return def;
        return (int64_t)d;
    }
    case Bool: return b ? 1 : 0;
    default: return def;
    }
}

double Json::dbl(double def) const
{
    if (t == Number) return n;
    if (t == String && !s.empty()) {
        char* e = nullptr;
        double d = strtod(s.c_str(), &e);
        return e == s.c_str() ? def : d;
    }
    return def;
}

bool Json::boolean(bool def) const
{
    switch (t) {
    case Bool: return b;
    case Number: return n != 0;
    case String: return s == "true" || s == "1" || s == "yes" || s == "Active";
    default: return def;
    }
}

const Json& Json::operator[](size_t i) const
{
    if (t != Array || i >= arr.size()) return kNull;
    return arr[i];
}

const Json& Json::operator[](const std::string& key) const
{
    if (t != Object) return kNull;
    for (size_t i = 0; i < keys.size(); i++)
        if (keys[i] == key) return vals[i];
    return kNull;
}

bool Json::has(const std::string& key) const
{
    if (t != Object) return false;
    for (auto& k : keys) if (k == key) return true;
    return false;
}

const Json& Json::any(std::initializer_list<const char*> names) const
{
    for (const char* nm : names) {
        const Json& v = (*this)[nm];
        if (!v.isNull()) return v;
    }
    return kNull;
}

void Json::push(const Json& v)
{
    if (t != Array) { t = Array; arr.clear(); }
    arr.push_back(v);
}

void Json::set(const std::string& key, const Json& v)
{
    if (t != Object) { t = Object; keys.clear(); vals.clear(); }
    for (size_t i = 0; i < keys.size(); i++)
        if (keys[i] == key) { vals[i] = v; return; }
    keys.push_back(key);
    vals.push_back(v);
}

void Json::remove(const std::string& key)
{
    if (t != Object) return;
    for (size_t i = 0; i < keys.size(); i++)
        if (keys[i] == key) {
            keys.erase(keys.begin() + (long)i);
            vals.erase(vals.begin() + (long)i);
            return;
        }
}

static void dumpString(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out) const
{
    switch (t) {
    case Null: out += "null"; break;
    case Bool: out += b ? "true" : "false"; break;
    case Number: out += str(); break;
    case String: dumpString(out, s); break;
    case Array:
        out += '[';
        for (size_t i = 0; i < arr.size(); i++) { if (i) out += ','; arr[i].dumpTo(out); }
        out += ']';
        break;
    case Object:
        out += '{';
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) out += ',';
            dumpString(out, keys[i]);
            out += ':';
            vals[i].dumpTo(out);
        }
        out += '}';
        break;
    }
}

std::string Json::dump() const
{
    std::string out;
    dumpTo(out);
    return out;
}
