#pragma once
// Parser/serializador JSON compacto e tolerante (equivalente ao Gson "lenient" do app Android).
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() : t(Null) {}
    Json(bool b) : t(Bool), b(b) {}
    Json(int v) : t(Number), n((double)v) {}
    Json(int64_t v) : t(Number), n((double)v) {}
    Json(double v) : t(Number), n(v) {}
    Json(const char* s) : t(String), s(s) {}
    Json(const std::string& s) : t(String), s(s) {}

    static Json array() { Json j; j.t = Array; return j; }
    static Json object() { Json j; j.t = Object; return j; }

    static Json parse(const std::string& text, bool* ok = nullptr);
    std::string dump() const;

    Type type() const { return t; }
    bool isNull() const { return t == Null; }
    bool isArray() const { return t == Array; }
    bool isObject() const { return t == Object; }
    bool isString() const { return t == String; }

    // Acesso tolerante: números como string e vice-versa
    std::string str(const std::string& def = "") const;
    int64_t num(int64_t def = 0) const;
    double dbl(double def = 0) const;
    bool boolean(bool def = false) const;

    size_t size() const { return t == Array ? arr.size() : (t == Object ? keys.size() : 0); }
    const Json& operator[](size_t i) const;
    const Json& operator[](const std::string& key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    bool has(const std::string& key) const;

    // Primeiro campo existente entre vários nomes alternativos
    const Json& any(std::initializer_list<const char*> names) const;

    // Mutação
    void push(const Json& v);
    void set(const std::string& key, const Json& v);
    void remove(const std::string& key);

    const std::vector<Json>& items() const { return arr; }
    const std::vector<std::string>& objectKeys() const { return keys; }

private:
    Type t;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::string> keys;
    std::vector<Json> vals;

    friend struct JsonParser;
    void dumpTo(std::string& out) const;
};
