#pragma once
// Controle DualShock 4 via SDL joystick (com auto-repetição das direções)
#include <vector>

enum class Btn {
    None, Up, Down, Left, Right,
    Cross,     // confirmar (OK)
    Circle,    // voltar
    Square,
    Triangle,
    L1, R1, L2, R2,
    Options,
    Touchpad,
};

namespace input {
void init();
// Processa eventos SDL e retorna os botões pressionados neste frame
std::vector<Btn> poll();
}
