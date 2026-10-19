#pragma once
// Cache em memória do catálogo Xtream (equivalente ao DataHolder.kt + DataRepository.kt)
#include <string>
#include <vector>
#include <functional>

#include "api.h"

namespace catalog {

enum class Load { None, Loading, Ready, Failed };

const std::vector<Category>& categories(Kind k);
bool categoriesLoaded();
// Carrega categorias de ao vivo, filmes e séries (em sequência, como o preload do Android)
void loadCategories(std::function<void(bool ok)> done);

// Streams de uma categoria (carregamento sob demanda). Retorna nullptr enquanto carrega.
const std::vector<Stream>* streams(Kind k, const std::string& categoryId, Load* state = nullptr);
void retry(Kind k, const std::string& categoryId);

// Lista completa (para busca). Retorna nullptr enquanto carrega.
const std::vector<Stream>* all(Kind k, Load* state = nullptr);

std::string categoryName(Kind k, const std::string& id);
void clear();

} // namespace catalog
