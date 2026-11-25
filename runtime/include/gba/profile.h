#pragma once
// Perfil leve por subsistema (ligado com tlagba --profile). Desligado, custa um teste
// de bool por medição.

#include <chrono>
#include <cstdint>

namespace gba::prof {

struct Counters {
    uint64_t ppu_ns = 0;   // render_line
    uint64_t bg_ns = 0;    //   BGs
    uint64_t obj_ns = 0;   //   sprites
    uint64_t mix_ns = 0;   //   composição
    uint64_t apu_ns = 0;   // Apu::catch_up (síntese/mixagem)
    uint64_t sync_ns = 0;  // sync() inteiro (inclui PPU e IRQ de vídeo)
    uint64_t syncs = 0;
    // Composição: linhas com janelas / efeito de cor / sprite semitransparente
    uint64_t lines = 0, lines_win = 0, lines_blend = 0, lines_semi = 0;
};

inline bool enabled = false;
inline Counters counters;

class Scope {
public:
    explicit Scope(uint64_t& acc) : acc_(enabled ? &acc : nullptr) {
        if (acc_) t0_ = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (acc_)
            *acc_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count());
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    uint64_t* acc_;
    std::chrono::steady_clock::time_point t0_;
};

} // namespace gba::prof
