// Captura de crash: grava no log o sinal, a instrução que falhou e endereços da pilha.
// Os endereços podem ser traduzidos com o mapa de símbolos gerado no CI (UnitVPro.map).
#include "crash.h"
#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <pthread.h>

#ifdef UNITV_PS5
#include <csignal>
#include <ucontext.h>
#else
extern "C" {
typedef void (*OrbisExceptionHandler)(int, void*);
int sceKernelInstallExceptionHandler(int signum, OrbisExceptionHandler handler);
int sceKernelRemoveExceptionHandler(int signum);
}
#endif

namespace crash {

static uint64_t g_base = 0; // endereço de execução de crash::install (referência para o mapa)

static void report(int sig, uint64_t rip, uint64_t addr, uint64_t rsp, uint64_t rbp, const uint64_t regs[6])
{
    platform::log("==== CRASH sinal %d (thread %p) ====", sig, (void*)pthread_self());
    platform::log("rip=%016llx addr=%016llx rsp=%016llx rbp=%016llx", (unsigned long long)rip,
                  (unsigned long long)addr, (unsigned long long)rsp, (unsigned long long)rbp);
    platform::log("rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rdi=%016llx rsi=%016llx",
                  (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2],
                  (unsigned long long)regs[3], (unsigned long long)regs[4], (unsigned long long)regs[5]);
    platform::log("referencia crash::install=%016llx (rip relativo: %+lld)", (unsigned long long)g_base,
                  (long long)(rip - g_base));
    // possíveis endereços de retorno na pilha (próximos do código do app)
    const uint64_t* sp = (const uint64_t*)rsp;
    int found = 0;
    for (int i = 0; sp && i < 512 && found < 16; i++) {
        uint64_t v = sp[i];
        if (v > g_base - (64ull << 20) && v < g_base + (64ull << 20)) {
            platform::log("  pilha[%d] = %016llx (relativo %+lld)", i, (unsigned long long)v, (long long)(v - g_base));
            found++;
        }
    }
    platform::log("==== fim do relatorio ====");
}

#ifdef UNITV_PS5
static void handler(int sig, siginfo_t* info, void* ctx)
{
    const mcontext_t& m = ((ucontext_t*)ctx)->uc_mcontext;
    const uint64_t regs[6] = {(uint64_t)m.mc_rax, (uint64_t)m.mc_rbx, (uint64_t)m.mc_rcx,
                              (uint64_t)m.mc_rdx, (uint64_t)m.mc_rdi, (uint64_t)m.mc_rsi};
    report(sig, (uint64_t)m.mc_rip, (uint64_t)(uintptr_t)(info ? info->si_addr : nullptr), (uint64_t)m.mc_rsp,
           (uint64_t)m.mc_rbp, regs);
    // devolve ao sistema: o sinal segue com o tratamento padrão
    signal(sig, SIG_DFL);
    raise(sig);
}
#else

// Contexto entregue pelo kernel (FreeBSD/Orbis): uc_sigmask(16) + 48 bytes, depois o mcontext
struct Mcontext {
    uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
    int32_t trapno;
    uint16_t fs, gs;
    uint64_t addr;
    int32_t flags;
    uint16_t es, ds;
    uint64_t err, rip, cs, rflags, rsp, ss;
};
struct Ucontext {
    uint32_t sigmask[4];
    int32_t pad[12];
    Mcontext mc;
};

static void handler(int sig, void* ctx)
{
    Ucontext* uc = (Ucontext*)ctx;
    const Mcontext& m = uc->mc;
    const uint64_t regs[6] = {m.rax, m.rbx, m.rcx, m.rdx, m.rdi, m.rsi};
    report(sig, m.rip, m.addr, m.rsp, m.rbp, regs);
    // devolve ao sistema: o crash segue normalmente (tela de erro do PS4)
    sceKernelRemoveExceptionHandler(sig);
}
#endif

static void onTerminate()
{
    platform::log("==== CRASH: std::terminate (excecao C++ nao tratada) ====");
    try {
        std::exception_ptr e = std::current_exception();
        if (e) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        platform::log("excecao: %s", ex.what());
    } catch (...) {
        platform::log("excecao desconhecida");
    }
    abort();
}

void install()
{
    g_base = (uint64_t)(void*)&install;
    std::set_terminate(onTerminate);
    const int sigs[] = {4 /*SIGILL*/, 6 /*SIGABRT*/, 8 /*SIGFPE*/, 10 /*SIGBUS*/, 11 /*SIGSEGV*/};
    for (int s : sigs) {
#ifdef UNITV_PS5
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        if (sigaction(s, &sa, nullptr) != 0) platform::log("sigaction %d falhou", s);
#else
        int r = sceKernelInstallExceptionHandler(s, handler);
        if (r < 0) platform::log("exception handler %d: 0x%08X", s, r);
#endif
    }
    platform::log("captura de crash ativa (crash::install=%016llx)", (unsigned long long)g_base);
}

} // namespace crash
