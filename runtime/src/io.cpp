#include "gba/gba.h"

namespace gba {

namespace {
constexpr uint32_t REG_DISPSTAT = 0x004, REG_VCOUNT = 0x006;
constexpr uint32_t REG_SOUNDCNT_H = 0x082;
constexpr uint32_t REG_FIFO_A = 0x0A0, REG_FIFO_B = 0x0A4;
constexpr uint32_t REG_KEYINPUT = 0x130, REG_KEYCNT = 0x132;
constexpr uint32_t REG_IE = 0x200, REG_IF = 0x202, REG_IME = 0x208, REG_HALTCNT = 0x300;
constexpr int kPrescalerShift[4] = {0, 6, 8, 10};
} // namespace

Io::Io(Gba& gba) : gba_(gba) {
    reg(REG_KEYINPUT) = 0x3FF;
    reg(0x088) = 0x200; // SOUNDBIAS
    for (int bg = 2; bg < 4; ++bg) {
        reg(0x20 + (bg - 2) * 0x10) = 0x100; // PA = 1.0
        reg(0x26 + (bg - 2) * 0x10) = 0x100; // PD = 1.0
    }
}

void Io::raise(uint16_t irq) {
    reg(REG_IF) |= irq;
}

bool Io::irq_ready() const {
    return (regs_[REG_IME >> 1] & 1) && (regs_[REG_IE >> 1] & regs_[REG_IF >> 1] & 0x3FFF);
}

bool Io::irq_any() const { return regs_[REG_IE >> 1] & regs_[REG_IF >> 1] & 0x3FFF; }

void Io::set_keys(uint16_t pressed) {
    keys_pressed_ = pressed & 0x3FF;
    reg(REG_KEYINPUT) = static_cast<uint16_t>(~keys_pressed_ & 0x3FF);
    const uint16_t cnt = reg(REG_KEYCNT);
    if (cnt & 0x4000) {
        const uint16_t want = cnt & 0x3FF;
        const bool hit = (cnt & 0x8000) ? (keys_pressed_ & want) == want : (keys_pressed_ & want) != 0;
        if (hit) raise(IRQ_KEYPAD);
    }
}

// ─────────────────────────────────────────────────────────── leitura ──

uint16_t Io::read16(uint32_t off) {
    off &= 0x3FE;
    if (off >= 0x100 && off < 0x110 && !(off & 2)) {
        update_timers(gba_.now());
        return timers[(off - 0x100) / 4].counter;
    }
    return regs_[off >> 1];
}

uint8_t Io::read8(uint32_t off) {
    const uint16_t v = read16(off & ~1u);
    return static_cast<uint8_t>((off & 1) ? v >> 8 : v);
}

// ─────────────────────────────────────────────────────────── escrita ──

void Io::write8(uint32_t off, uint8_t v) {
    off &= 0x3FF;
    if (off == REG_HALTCNT + 1) {
        gba_.cpu.halted = true; // bit 7 = STOP; tratamos igual a HALT
        return;
    }
    if (off == REG_IF || off == REG_IF + 1) {
        reg(REG_IF) &= static_cast<uint16_t>(~(uint32_t(v) << ((off & 1) * 8)));
        return;
    }
    if (off >= REG_FIFO_A && off < REG_FIFO_B + 4) {
        auto& f = fifo[off >= REG_FIFO_B ? 1 : 0];
        if (f.size() < 32) f.push_back(static_cast<int8_t>(v));
        return;
    }
    const uint32_t aligned = off & ~1u;
    uint16_t cur = regs_[aligned >> 1];
    cur = (off & 1) ? static_cast<uint16_t>((cur & 0x00FF) | (v << 8)) : static_cast<uint16_t>((cur & 0xFF00) | v);
    write16(aligned, cur);
}

void Io::write16(uint32_t off, uint16_t v) {
    off &= 0x3FE;
    switch (off) {
    case REG_DISPSTAT:
        reg(off) = static_cast<uint16_t>((reg(off) & 0x0007) | (v & 0xFFF8));
        return;
    case REG_VCOUNT:
    case REG_KEYINPUT:
        return; // somente leitura
    case REG_IF:
        reg(REG_IF) &= static_cast<uint16_t>(~v);
        return;
    case REG_HALTCNT:
        reg(off) = v;
        if (v & 0xFF00) gba_.cpu.halted = true;
        return;
    case REG_FIFO_A: case REG_FIFO_A + 2: case REG_FIFO_B: case REG_FIFO_B + 2: {
        auto& f = fifo[off >= REG_FIFO_B ? 1 : 0];
        if (f.size() <= 30) {
            f.push_back(static_cast<int8_t>(v));
            f.push_back(static_cast<int8_t>(v >> 8));
        }
        return;
    }
    case REG_SOUNDCNT_H:
        reg(off) = v;
        if (v & 0x0800) fifo[0].clear();
        if (v & 0x8000) fifo[1].clear();
        return;
    default: break;
    }

    // Pontos de referência dos BGs afins
    if (off >= 0x028 && off <= 0x02E) { reg(off) = v; gba_.ppu.on_affine_ref_write(2); return; }
    if (off >= 0x038 && off <= 0x03E) { reg(off) = v; gba_.ppu.on_affine_ref_write(3); return; }

    // DMA
    if (off >= 0x0B0 && off < 0x0E0) {
        const int ch = static_cast<int>((off - 0x0B0) / 12);
        const uint32_t r = (off - 0x0B0) % 12;
        DmaChannel& d = dma[ch];
        reg(off) = v;
        switch (r) {
        case 0: d.sad = (d.sad & 0xFFFF0000) | v; break;
        case 2: d.sad = (d.sad & 0x0000FFFF) | (uint32_t(v) << 16); break;
        case 4: d.dad = (d.dad & 0xFFFF0000) | v; break;
        case 6: d.dad = (d.dad & 0x0000FFFF) | (uint32_t(v) << 16); break;
        case 8: d.cnt_l = v; break;
        default: dma_write_control(ch, v); break;
        }
        return;
    }

    // Timers
    if (off >= 0x100 && off < 0x110) {
        update_timers(gba_.now());
        Timer& t = timers[(off - 0x100) / 4];
        if (!(off & 2)) {
            t.reload = v;
        } else {
            const bool was = t.control & 0x80;
            t.control = v;
            if (!was && (v & 0x80)) {
                t.counter = t.reload;
                t.sub = 0;
            }
            t.last_update = gba_.now();
        }
        reg(off) = v;
        return;
    }

    reg(off) = v;
}

// ─────────────────────────────────────────────────────────────── DMA ──

void Io::dma_write_control(int ch, uint16_t v) {
    DmaChannel& d = dma[ch];
    const bool was = d.cnt_h & 0x8000;
    d.cnt_h = v;
    if (!was && (v & 0x8000)) {
        d.src = d.sad & (ch == 0 ? 0x07FFFFFF : 0x0FFFFFFF);
        d.dst = d.dad & (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
        d.count = d.cnt_l & (ch == 3 ? 0xFFFF : 0x3FFF);
        if (d.count == 0) d.count = ch == 3 ? 0x10000 : 0x4000;
        if (((v >> 12) & 3) == 0) run_dma(ch);
    }
}

void Io::run_dma(int ch) {
    DmaChannel& d = dma[ch];
    const uint32_t timing = (d.cnt_h >> 12) & 3;
    const bool fifo_mode = timing == 3 && (ch == 1 || ch == 2);
    bool wide = (d.cnt_h >> 10) & 1;
    uint32_t count = d.count;
    const uint32_t dst_ctrl = fifo_mode ? 2 : (d.cnt_h >> 5) & 3;
    const uint32_t src_ctrl = (d.cnt_h >> 7) & 3;
    if (fifo_mode) {
        wide = true;
        count = 4;
    }
    // EEPROM: o tamanho do pedido revela a largura do endereço (6 ou 14 bits).
    if (ch == 3 && (d.dst >> 24) == 0x0D) {
        if (count == 9 || count == 73) gba_.bus.eeprom.set_addr_bits(6);
        else if (count == 17 || count == 81) gba_.bus.eeprom.set_addr_bits(14);
    }
    const int step = wide ? 4 : 2;
    auto advance = [step](uint32_t& a, uint32_t ctrl) {
        if (ctrl == 0 || ctrl == 3) a += step;
        else if (ctrl == 1) a -= step;
    };
    Bus& bus = gba_.bus;
    for (uint32_t i = 0; i < count; ++i) {
        if (wide) bus.write32(d.dst & ~3u, bus.read32(d.src & ~3u));
        else bus.write16(d.dst & ~1u, bus.read16(d.src & ~1u));
        advance(d.src, src_ctrl);
        advance(d.dst, dst_ctrl);
    }
    if (d.cnt_h & 0x4000) raise(static_cast<uint16_t>(IRQ_DMA0 << ch));
    if ((d.cnt_h & 0x0200) && timing != 0) {
        d.count = d.cnt_l & (ch == 3 ? 0xFFFF : 0x3FFF);
        if (d.count == 0) d.count = ch == 3 ? 0x10000 : 0x4000;
        if (dst_ctrl == 3) d.dst = d.dad & (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
    } else {
        d.cnt_h &= 0x7FFF;
        reg(0x0BA + ch * 12) = d.cnt_h;
    }
}

void Io::trigger_dma(int timing) {
    for (int ch = 0; ch < 4; ++ch) {
        const DmaChannel& d = dma[ch];
        if ((d.cnt_h & 0x8000) && static_cast<int>((d.cnt_h >> 12) & 3) == timing) run_dma(ch);
    }
}

// ──────────────────────────────────────────────── vídeo (agendador) ──

void Io::on_hblank(int line) {
    reg(REG_DISPSTAT) |= 2;
    if (reg(REG_DISPSTAT) & 0x10) raise(IRQ_HBLANK);
    if (line < kScreenH) trigger_dma(2);
}

void Io::on_vblank() {
    if (reg(REG_DISPSTAT) & 0x08) raise(IRQ_VBLANK);
    trigger_dma(1);
}

// ──────────────────────────────────────────────────────────── timers ──

void Io::fifo_tick(int timer) {
    const uint16_t sc = reg(REG_SOUNDCNT_H);
    for (int ch = 0; ch < 2; ++ch) {
        const int sel = (sc >> (ch ? 14 : 10)) & 1;
        if (sel != timer) continue;
        auto& f = fifo[ch];
        if (!f.empty()) f.erase(f.begin());
        if (f.size() <= 16) {
            // Pede mais amostras ao canal de DMA (1 ou 2) configurado para este FIFO.
            for (int c = 1; c <= 2; ++c) {
                const DmaChannel& d = dma[c];
                const uint32_t target = ch ? 0x040000A4 : 0x040000A0;
                if ((d.cnt_h & 0x8000) && ((d.cnt_h >> 12) & 3) == 3 && (d.dad & 0x0FFFFFFF) == target) {
                    run_dma(c);
                    break;
                }
            }
        }
    }
}

void Io::timer_overflow(int t, uint32_t times) {
    if (timers[t].control & 0x40) raise(static_cast<uint16_t>(IRQ_TIMER0 << t));
    if (t < 2) {
        const uint32_t n = times > 64 ? 64 : times; // limite defensivo
        for (uint32_t i = 0; i < n; ++i) fifo_tick(t);
    }
    if (t < 3) {
        Timer& nx = timers[t + 1];
        if ((nx.control & 0x80) && (nx.control & 0x04)) {
            const uint32_t period = 0x10000u - nx.reload;
            uint32_t total = uint32_t(nx.counter) + times;
            if (total >= 0x10000) {
                const uint32_t over = total - 0x10000;
                const uint32_t n = 1 + over / period;
                nx.counter = static_cast<uint16_t>(nx.reload + over % period);
                timer_overflow(t + 1, n);
            } else {
                nx.counter = static_cast<uint16_t>(total);
            }
        }
    }
}

void Io::update_timers(uint64_t now) {
    for (int i = 0; i < 4; ++i) {
        Timer& t = timers[i];
        const uint64_t elapsed = now - t.last_update;
        t.last_update = now;
        if (!(t.control & 0x80) || (i > 0 && (t.control & 0x04))) continue;
        const int shift = kPrescalerShift[t.control & 3];
        const uint64_t acc = t.sub + elapsed;
        const uint64_t ticks = acc >> shift;
        t.sub = static_cast<uint32_t>(acc & ((1u << shift) - 1));
        if (!ticks) continue;
        const uint64_t period = 0x10000u - t.reload;
        const uint64_t total = t.counter + ticks;
        if (total >= 0x10000) {
            const uint64_t over = total - 0x10000;
            const uint64_t n = 1 + over / period;
            t.counter = static_cast<uint16_t>(t.reload + over % period);
            timer_overflow(i, static_cast<uint32_t>(n));
        } else {
            t.counter = static_cast<uint16_t>(total);
        }
    }
}

uint64_t Io::next_timer_event(uint64_t now) const {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 4; ++i) {
        const Timer& t = timers[i];
        if (!(t.control & 0x80) || (i > 0 && (t.control & 0x04))) continue;
        const int shift = kPrescalerShift[t.control & 3];
        const uint64_t ticks_left = 0x10000u - t.counter;
        const uint64_t cycles = (ticks_left << shift) - t.sub;
        const uint64_t at = t.last_update + cycles;
        if (at < best) best = at;
    }
    return best < now ? now : best;
}

} // namespace gba
