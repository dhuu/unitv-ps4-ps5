#pragma once
// Execução em segundo plano (substitui coroutines/Retrofit enqueue do Android).
// work() roda numa thread de trabalho; done() roda depois na thread principal.
#include <functional>

namespace jobs {
void init(int threads = 2);
void run(std::function<void()> work, std::function<void()> done = nullptr);
void post(std::function<void()> fn);   // agenda para a thread principal
int pump();                            // chamar na thread principal a cada frame; retorna quantas rodaram
}
