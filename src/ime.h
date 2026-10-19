#pragma once
// Teclado virtual do sistema (sceImeDialog) para digitar usuário, senha, DNS e busca
#include <string>
#include <functional>

namespace ime {

enum class Type { Text, Url, Number, Password, Search };

// Abre o teclado. onDone(ok, texto) é chamado na thread principal quando fechar.
bool open(const std::string& title, const std::string& initial, Type type,
          std::function<void(bool, const std::string&)> onDone, int maxLen = 128);
bool active();
void update(); // chamar a cada frame
// PS5: o SDL pode fechar o teclado antes do app; ele avisa o "OK" com um Enter (input.cpp)
void noteConfirm();

}
