#pragma once
// APU do GBA: dois canais Direct Sound (FIFO A/B, PCM 8 bits disparado por timer) e os
// quatro canais PSG herdados do Game Boy. Gera estéreo int16 a 48 kHz, no tempo exato
// das mudanças (catch_up antes de cada evento que altera o som).

#include <array>
#include <cstdint>
#include <vector>

namespace gba {

class Gba;

class Apu {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint64_t kCpuHz = 16777216;

    explicit Apu(Gba& gba);

    // Gera amostras até o ciclo `now`.
    void catch_up(uint64_t now);
    // Novo valor de saída de um canal Direct Sound (0 = A, 1 = B).
    void on_fifo_sample(int ch, int8_t sample, uint64_t now);
    // Escrita em registrador de som (0x060–0x09F); chamado depois de gravar o valor.
    void on_write(uint32_t off, uint16_t v, uint64_t now);

    // Amostras estéreo intercaladas acumuladas desde a última chamada.
    std::vector<int16_t> take_samples();

    void save(class StateWriter& w) const;
    void load(class StateReader& r);
    const std::vector<int16_t>& pending() const { return out_; }

private:
    struct Square {
        bool on = false;
        int duty = 0, step = 0;
        int32_t timer = 0;
        int volume = 0, env_period = 0, env_timer = 0;
        bool env_up = false;
        int length = 0;
        bool length_en = false;
        // varredura (só canal 1)
        int sweep_period = 0, sweep_shift = 0, sweep_timer = 0;
        bool sweep_down = false, sweep_on = false;
        int shadow_freq = 0;
    };
    struct Wave {
        bool on = false;
        int pos = 0;
        int32_t timer = 0;
        int length = 0;
        bool length_en = false;
    };
    struct Noise {
        bool on = false;
        int32_t timer = 0;
        uint16_t lfsr = 0x7FFF;
        int volume = 0, env_period = 0, env_timer = 0;
        bool env_up = false;
        int length = 0;
        bool length_en = false;
    };

    uint16_t reg(uint32_t off) const;
    void trigger_square(int n);
    void trigger_wave();
    void trigger_noise();
    void frame_sequencer_step();
    void advance_channels(uint32_t cycles);
    void mix_sample();
    int square_freq(int n) const;

    Gba& gba_;
    std::vector<int16_t> out_;
    uint64_t sample_num_ = 0;  // próxima amostra, em unidades de 1/kSampleRate segundo
    uint64_t last_cycle_ = 0;
    uint32_t seq_cycles_ = 0;
    int seq_step_ = 0;

    std::array<Square, 2> sq_{};
    Wave wave_{};
    Noise noise_{};
    std::array<uint8_t, 32> wave_ram_{}; // 2 bancos × 16 bytes (32 amostras de 4 bits cada)
    std::array<int8_t, 2> fifo_out_{};

    // Filtro passa-baixa simples (o GBA tem um filtro analógico na saída).
    int32_t lp_l_ = 0, lp_r_ = 0;
};

} // namespace gba
