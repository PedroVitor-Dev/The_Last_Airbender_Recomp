#include "gba/apu.h"

#include <algorithm>

#include "gba/gba.h"
#include "gba/profile.h"

namespace gba {

namespace {

constexpr uint32_t kSeqPeriod = 16777216 / 512; // sequenciador de quadro a 512 Hz

constexpr uint8_t kDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, // 12,5%
    {1, 0, 0, 0, 0, 0, 0, 1}, // 25%
    {1, 0, 0, 0, 0, 1, 1, 1}, // 50%
    {0, 1, 1, 1, 1, 1, 1, 0}, // 75%
};

// Registradores de cada canal quadrado: [comprimento/duty/envelope, frequência/controle]
constexpr uint32_t kSqCnt[2] = {0x062, 0x068};
constexpr uint32_t kSqFreq[2] = {0x064, 0x06C};

} // namespace

Apu::Apu(Gba& gba) : gba_(gba) {}

uint16_t Apu::reg(uint32_t off) const { return gba_.io.reg(off); }

int Apu::square_freq(int n) const { return reg(kSqFreq[n]) & 0x7FF; }

std::vector<int16_t> Apu::take_samples() {
    std::vector<int16_t> s;
    s.swap(out_);
    return s;
}

// ─────────────────────────────────────────────────────────── disparos ──

void Apu::trigger_square(int n) {
    Square& s = sq_[n];
    const uint16_t cnt = reg(kSqCnt[n]);
    const uint16_t fr = reg(kSqFreq[n]);
    if (s.length == 0) s.length = 64;
    s.duty = (cnt >> 6) & 3;
    s.volume = cnt >> 12;
    s.env_up = (cnt >> 11) & 1;
    s.env_period = (cnt >> 8) & 7;
    s.env_timer = s.env_period;
    s.length_en = (fr >> 14) & 1;
    s.timer = (2048 - (fr & 0x7FF)) * 16;
    s.on = (cnt & 0xF800) != 0; // DAC ligado
    if (n == 0) {
        const uint16_t sw = reg(0x060);
        s.sweep_shift = sw & 7;
        s.sweep_down = (sw >> 3) & 1;
        s.sweep_period = (sw >> 4) & 7;
        s.sweep_timer = s.sweep_period ? s.sweep_period : 8;
        s.shadow_freq = fr & 0x7FF;
        s.sweep_on = s.sweep_period || s.sweep_shift;
    }
}

void Apu::trigger_wave() {
    const uint16_t fr = reg(0x074);
    if (wave_.length == 0) wave_.length = 256;
    wave_.length_en = (fr >> 14) & 1;
    wave_.timer = (2048 - (fr & 0x7FF)) * 8;
    wave_.pos = 0;
    wave_.on = (reg(0x070) & 0x80) != 0;
}

void Apu::trigger_noise() {
    const uint16_t cnt = reg(0x078), fr = reg(0x07C);
    if (noise_.length == 0) noise_.length = 64;
    noise_.volume = cnt >> 12;
    noise_.env_up = (cnt >> 11) & 1;
    noise_.env_period = (cnt >> 8) & 7;
    noise_.env_timer = noise_.env_period;
    noise_.length_en = (fr >> 14) & 1;
    noise_.lfsr = (fr & 0x08) ? 0x7F : 0x7FFF;
    noise_.timer = 0;
    noise_.on = (cnt & 0xF800) != 0;
}

void Apu::on_write(uint32_t off, uint16_t v, uint64_t now) {
    catch_up(now);
    switch (off) {
    case 0x062: case 0x068: {
        Square& s = sq_[off == 0x062 ? 0 : 1];
        s.length = 64 - (v & 63);
        s.duty = (v >> 6) & 3;
        if (!(v & 0xF800)) s.on = false;
        break;
    }
    case 0x064: case 0x06C: {
        const int n = off == 0x064 ? 0 : 1;
        sq_[n].length_en = (v >> 14) & 1;
        if (v & 0x8000) trigger_square(n);
        break;
    }
    case 0x070:
        if (!(v & 0x80)) wave_.on = false;
        break;
    case 0x072:
        wave_.length = 256 - (v & 0xFF);
        break;
    case 0x074:
        wave_.length_en = (v >> 14) & 1;
        if (v & 0x8000) trigger_wave();
        break;
    case 0x078:
        noise_.length = 64 - (v & 63);
        if (!(v & 0xF800)) noise_.on = false;
        break;
    case 0x07C:
        noise_.length_en = (v >> 14) & 1;
        if (v & 0x8000) trigger_noise();
        break;
    case 0x084:
        if (!(v & 0x80)) {
            sq_[0].on = sq_[1].on = wave_.on = noise_.on = false;
        }
        break;
    default:
        if (off >= 0x090 && off < 0x0A0) {
            // A CPU escreve no banco que NÃO está tocando.
            const int bank = ((reg(0x070) >> 6) & 1) ^ 1;
            const uint32_t i = bank * 16 + (off - 0x090);
            wave_ram_[i] = static_cast<uint8_t>(v);
            wave_ram_[i + 1] = static_cast<uint8_t>(v >> 8);
        }
        break;
    }
}

void Apu::on_fifo_sample(int ch, int8_t sample, uint64_t now) {
    catch_up(now);
    fifo_out_[ch] = sample;
}

// ──────────────────────────────────────────────────────────── síntese ──

void Apu::frame_sequencer_step() {
    const int step = seq_step_;
    seq_step_ = (seq_step_ + 1) & 7;

    if ((step & 1) == 0) { // comprimento, 256 Hz
        auto tick = [](bool& on, bool en, int& len) {
            if (en && len > 0 && --len == 0) on = false;
        };
        tick(sq_[0].on, sq_[0].length_en, sq_[0].length);
        tick(sq_[1].on, sq_[1].length_en, sq_[1].length);
        tick(wave_.on, wave_.length_en, wave_.length);
        tick(noise_.on, noise_.length_en, noise_.length);
    }
    if (step == 2 || step == 6) { // varredura, 128 Hz
        Square& s = sq_[0];
        if (s.on && s.sweep_on && --s.sweep_timer <= 0) {
            s.sweep_timer = s.sweep_period ? s.sweep_period : 8;
            if (s.sweep_period) {
                const int delta = s.shadow_freq >> s.sweep_shift;
                const int nf = s.sweep_down ? s.shadow_freq - delta : s.shadow_freq + delta;
                if (nf > 2047) {
                    s.on = false;
                } else if (s.sweep_shift) {
                    s.shadow_freq = nf;
                    uint16_t& fr = gba_.io.reg(0x064);
                    fr = static_cast<uint16_t>((fr & ~0x7FF) | nf);
                }
            }
        }
    }
    if (step == 7) { // envelope, 64 Hz
        auto env = [](int& vol, bool up, int period, int& timer) {
            if (!period) return;
            if (--timer <= 0) {
                timer = period;
                if (up && vol < 15) ++vol;
                else if (!up && vol > 0) --vol;
            }
        };
        env(sq_[0].volume, sq_[0].env_up, sq_[0].env_period, sq_[0].env_timer);
        env(sq_[1].volume, sq_[1].env_up, sq_[1].env_period, sq_[1].env_timer);
        env(noise_.volume, noise_.env_up, noise_.env_period, noise_.env_timer);
    }
}

void Apu::advance_channels(uint32_t cycles) {
    if (!cycles) return;
    seq_cycles_ += cycles;
    while (seq_cycles_ >= kSeqPeriod) {
        seq_cycles_ -= kSeqPeriod;
        frame_sequencer_step();
    }
    const int32_t dt = static_cast<int32_t>(cycles);
    for (int n = 0; n < 2; ++n) {
        Square& s = sq_[n];
        if (!s.on) continue;
        const int32_t period = (2048 - square_freq(n)) * 16;
        s.timer -= dt;
        while (s.timer <= 0) {
            s.timer += period;
            s.step = (s.step + 1) & 7;
        }
    }
    if (wave_.on) {
        const int32_t period = (2048 - (reg(0x074) & 0x7FF)) * 8;
        const int samples = (reg(0x070) & 0x20) ? 64 : 32;
        wave_.timer -= dt;
        while (wave_.timer <= 0) {
            wave_.timer += period;
            wave_.pos = (wave_.pos + 1) % samples;
        }
    }
    if (noise_.on) {
        const uint16_t fr = reg(0x07C);
        const int r = fr & 7, shift = (fr >> 4) & 15;
        // 524288 Hz / r / 2^(s+1)  →  ciclos = 32 · r · 2^(s+1)   (r = 0 vale 0,5)
        const int32_t period = (r ? 32 * r : 16) << (shift + 1);
        const bool narrow = fr & 0x08;
        noise_.timer -= dt;
        while (noise_.timer <= 0) {
            noise_.timer += period;
            const uint16_t bit = (noise_.lfsr ^ (noise_.lfsr >> 1)) & 1;
            noise_.lfsr = static_cast<uint16_t>((noise_.lfsr >> 1) | (bit << 14));
            if (narrow) noise_.lfsr = static_cast<uint16_t>((noise_.lfsr & ~0x40) | (bit << 6));
        }
    }
}

void Apu::mix_sample() {
    const uint16_t cnt_l = reg(0x080), cnt_h = reg(0x082);
    uint16_t& cnt_x = gba_.io.reg(0x084);
    cnt_x = static_cast<uint16_t>((cnt_x & ~0xF) | (sq_[0].on ? 1 : 0) | (sq_[1].on ? 2 : 0) |
                                  (wave_.on ? 4 : 0) | (noise_.on ? 8 : 0));
    if (!(cnt_x & 0x80)) {
        out_.push_back(0);
        out_.push_back(0);
        return;
    }

    // Saída de cada canal PSG, bipolar em ±15.
    int ch[4] = {0, 0, 0, 0};
    for (int n = 0; n < 2; ++n) {
        const Square& s = sq_[n];
        if (s.on) ch[n] = kDuty[s.duty][s.step] ? s.volume : -s.volume;
    }
    if (wave_.on) {
        const uint16_t c = reg(0x070), w = reg(0x072);
        const int bank = (c >> 6) & 1;
        const int pos = (c & 0x20) ? (bank * 32 + wave_.pos) % 64 : bank * 32 + wave_.pos;
        const uint8_t byte = wave_ram_[pos / 2];
        const int nib = (pos & 1) ? (byte & 15) : (byte >> 4);
        int v = (nib - 8) * 2;
        if (w & 0x8000) v = v * 3 / 4;
        else switch ((w >> 13) & 3) {
            case 0: v = 0; break;
            case 2: v /= 2; break;
            case 3: v /= 4; break;
            default: break;
            }
        ch[2] = v;
    }
    if (noise_.on) ch[3] = (noise_.lfsr & 1) ? -noise_.volume : noise_.volume;

    int psg_l = 0, psg_r = 0;
    for (int i = 0; i < 4; ++i) {
        if (cnt_l & (0x100 << i)) psg_r += ch[i];
        if (cnt_l & (0x1000 << i)) psg_l += ch[i];
    }
    psg_r *= (cnt_l & 7) + 1;
    psg_l *= ((cnt_l >> 4) & 7) + 1;
    const int psg_shift = 3 - std::min(2, cnt_h & 3); // 25% / 50% / 100%
    psg_l >>= psg_shift;
    psg_r >>= psg_shift;

    const int a = fifo_out_[0] * ((cnt_h & 0x04) ? 4 : 2);
    const int b = fifo_out_[1] * ((cnt_h & 0x08) ? 4 : 2);
    int l = psg_l, r = psg_r;
    if (cnt_h & 0x0100) r += a;
    if (cnt_h & 0x0200) l += a;
    if (cnt_h & 0x1000) r += b;
    if (cnt_h & 0x2000) l += b;
    l = std::clamp(l, -512, 511);
    r = std::clamp(r, -512, 511);

    // Passa-baixa de 1 polo (~9 kHz) para suavizar os degraus do PCM.
    lp_l_ += ((l * 64) - lp_l_) * 11 / 16;
    lp_r_ += ((r * 64) - lp_r_) * 11 / 16;
    out_.push_back(static_cast<int16_t>(std::clamp(lp_l_, -32768, 32767)));
    out_.push_back(static_cast<int16_t>(std::clamp(lp_r_, -32768, 32767)));
}

void Apu::catch_up(uint64_t now) {
    prof::Scope scope(prof::counters.apu_ns);
    for (;;) {
        const uint64_t t = (sample_num_ * kCpuHz + kSampleRate - 1) / kSampleRate;
        if (t > now) break;
        advance_channels(static_cast<uint32_t>(t - last_cycle_));
        last_cycle_ = t;
        mix_sample();
        ++sample_num_;
    }
    if (now > last_cycle_) {
        advance_channels(static_cast<uint32_t>(now - last_cycle_));
        last_cycle_ = now;
    }
}

} // namespace gba
