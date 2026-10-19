#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace util {

std::string urlEncode(const std::string& s);
std::string trim(const std::string& s);
std::string trimRight(const std::string& s, char c);
bool startsWith(const std::string& s, const std::string& prefix);
// Normaliza para busca: minúsculas, sem acentos, só letras/dígitos separados por espaço
std::string normalize(const std::string& s);
bool containsNormalized(const std::string& haystack, const std::string& needleNormalized);
// Remove decorações à esquerda ("> ", "▶ ") como CategoryNameUtils.displayCategoryLabel
std::string displayCategoryLabel(const std::string& raw);
bool isAdultCategory(const std::string& name);
bool isKidsCategory(const std::string& name);
std::string formatTime(uint64_t ms);  // 1:02:03 ou 02:03
std::string formatClock(int64_t unixSec); // HH:MM local
// Decodifica um codepoint UTF-8 a partir de i (avança i)
uint32_t nextCodepoint(const std::string& s, size_t& i);
std::string base64Decode(const std::string& in);

} // namespace util
