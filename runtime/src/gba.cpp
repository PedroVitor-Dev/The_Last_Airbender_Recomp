// Agendador por eventos + execução híbrida (código recompilado ⇄ interpretador).
//
// O tempo avança em `cycles`. `next_event` é o próximo instante em que algo do
// hardware precisa acontecer (fase da scanline ou overflow de timer). Tanto o
// interpretador quanto o código recompilado chamam sync() quando cycles ≥ next_event.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "gba/gba.h"

namespace gba {

Gba::Gba(const core::Rom& rom) : bus(*this), cpu(*this), io(*this), ppu(*this), apu(*this), rom_(rom.bytes()) {
    bus.rom = &rom_;
    bios_install_stub(bus);
    cpu.reset_after_bios();
    begin_line();
    next_event = video_event_;
}

void Gba::set_recomp(const RecompEntry* table, size_t count, const RecompOverlay* overlays,
                     size_t overlay_count) {
    recomp_.clear();
    overlays_.assign(overlays, overlays + overlay_count);
    recomp_bitmap_.assign(rom_.size() / 16 + 1, 0);
    for (size_t i = 0; i < count; ++i) {
        recomp_[table[i].addr] = table[i].fn;
        const uint32_t a = table[i].addr & ~1u;
        if (a >= 0x08000000 && a - 0x08000000 < rom_.size()) {
            const uint32_t h = (a - 0x08000000) >> 1;
            recomp_bitmap_[h >> 3] |= static_cast<uint8_t>(1 << (h & 7));
        }
    }
}

RecompFn Gba::find(uint32_t addr, bool thumb) const {
    if (recomp_.empty()) return nullptr;
    if (addr >= 0x08000000 && addr - 0x08000000 < rom_.size()) {
        const uint32_t h = (addr - 0x08000000) >> 1;
        if (!(recomp_bitmap_[h >> 3] & (1 << (h & 7)))) return nullptr;
    } else {
        return find_overlay(addr, thumb);
    }
    auto it = recomp_.find(addr | (thumb ? 1u : 0u));
    return it == recomp_.end() ? nullptr : it->second;
}

// ─────────────────────────────────────────────────────────────── vídeo ──

void Gba::begin_line() {
    uint16_t& stat = io.reg(0x004);
    io.reg(0x006) = static_cast<uint16_t>(line_);
    stat &= ~2u;
    if (line_ == static_cast<uint32_t>(kScreenH)) {
        stat |= 1;
        ppu.latch_affine();
        io.on_vblank();
    } else if (line_ == kLinesPerFrame - 1) {
        stat &= ~1u;
    }
    if (static_cast<uint32_t>(stat >> 8) == line_) {
        stat |= 4;
        if (stat & 0x20) io.raise(IRQ_VCOUNT);
    } else {
        stat &= ~4u;
    }
}

void Gba::advance_video() {
    if (!in_hblank_) {
        if (line_ < static_cast<uint32_t>(kScreenH)) ppu.render_line(static_cast<int>(line_));
        io.on_hblank(static_cast<int>(line_));
        in_hblank_ = true;
        video_event_ = line_start_ + kCyclesPerLine;
    } else {
        in_hblank_ = false;
        line_start_ += kCyclesPerLine;
        line_ = (line_ + 1) % kLinesPerFrame;
        if (line_ == 0) {
            apu.catch_up(line_start_);
            ++frame_count;
            frame_done_ = true;
        }
        begin_line();
        video_event_ = line_start_ + kHDrawCycles;
    }
}

void Gba::sync() {
    for (;;) {
        io.update_timers(cycles);
        if (cycles < video_event_) break;
        advance_video();
        if (frame_done_) {
            frame_done_ = false;
            if (on_frame_ && !on_frame_()) throw StopRequested{};
        }
    }
    next_event = std::min(video_event_, io.next_timer_event(cycles));
}

// ─────────────────────────────────────────────────────── HALT e IRQ ──

void Gba::halt_wait() {
    const uint64_t irqs = cpu.irq_count;
    while (!io.irq_any() && cpu.irq_count == irqs) {
        if (cycles < next_event) cycles = next_event;
        sync();
    }
    cpu.halted = false;
}

// Equivalente em C++ do stub do vetor de IRQ (bios.cpp), para o caminho recompilado.
void Gba::service_irq() {
    Cpu& c = cpu;
    const uint32_t resume = c.r[15];
    const uint32_t old = c.cpsr();
    c.switch_mode(MODE_IRQ);
    c.spsr_ = old;
    c.i_ = true;
    c.t_ = false;
    c.r[14] = resume + 4;
    ++c.irq_count;

    uint32_t sp = c.r[13] - 24; // push {r0-r3, r12, lr}
    const uint32_t saved[6] = {c.r[0], c.r[1], c.r[2], c.r[3], c.r[12], c.r[14]};
    for (int i = 0; i < 6; ++i) bus.write32(sp + i * 4, saved[i]);
    c.r[13] = sp;

    constexpr uint32_t kStubReturn = 0x28; // endereço após o "ldr pc" no stub
    c.r[0] = 0x04000000;
    c.r[14] = kStubReturn;
    const uint32_t handler = bus.read32(0x03FFFFFC);
    call_function(handler & ~3u, false);
    if (c.r[15] != kStubReturn) resume_until(kStubReturn);

    sp = c.r[13]; // pop {r0-r3, r12, lr}
    const int regs[6] = {0, 1, 2, 3, 12, 14};
    for (int i = 0; i < 6; ++i) c.r[regs[i]] = bus.read32(sp + i * 4);
    c.r[13] = sp + 24;
    const uint32_t ret = c.r[14] - 4; // subs pc, lr, #4
    c.set_cpsr(c.spsr_);
    c.r[15] = ret & (c.t_ ? ~1u : ~3u);
}

void Gba::sync_at(uint32_t pc) {
    cpu.r[15] = pc;
    if (cycles >= next_event) sync();
    if (cpu.halted) halt_wait();
    if (!cpu.i_ && io.irq_ready()) {
        service_irq();
        if (cpu.r[15] != pc) resume_until(pc);
    }
}

void Gba::swi(uint32_t number, uint32_t next_pc) {
    cpu.r[15] = next_pc;
    if (!bios_swi(*this, number)) std::fprintf(stderr, "[bios] SWI 0x%02X não suportada\n", number);
    if (cpu.halted) halt_wait();
    if (!cpu.i_ && io.irq_ready()) {
        service_irq();
        if (cpu.r[15] != next_pc) resume_until(next_pc);
    }
}

// ───────────────────────────────────────────────────────── despacho ──

void Gba::call_function(uint32_t addr, bool thumb) {
    addr &= thumb ? ~1u : ~3u;
    cpu.t_ = thumb;
    cpu.r[15] = addr;
    if (RecompFn fn = find(addr, thumb)) {
        ++recomp_calls;
        fn(*this);
        return;
    }
    const uint32_t key = addr | (thumb ? 1u : 0u);
    ++fallback_entries[key];
    const uint64_t before = cpu.instr_count;
    run_hybrid_until(cpu.r[14] & ~1u);
    fallback_instrs[key] += cpu.instr_count - before;
}

void Gba::jump_indirect(uint32_t target) { call_function(target, target & 1); }

void Gba::resume_until(uint32_t ret) {
    // Retorno inesperado ou fall-through para código não analisado: conta como fallback.
    const uint32_t key = cpu.r[15] | (cpu.t_ ? 1u : 0u);
    ++fallback_entries[key];
    static const bool trace = std::getenv("TLA_TRACE_RESUME") != nullptr;
    static int traced = 0;
    if (trace && traced < 20) {
        ++traced;
        std::fprintf(stderr, "[resume] pc=0x%08X (%s) esperado=0x%08X lr=0x%08X sp=0x%08X modo=0x%02X quadro=%llu\n",
                     cpu.r[15], cpu.t_ ? "thumb" : "arm", ret, cpu.r[14], cpu.r[13], cpu.mode_,
                     static_cast<unsigned long long>(frame_count));
    }
    const uint64_t before = cpu.instr_count;
    run_hybrid_until(ret);
    fallback_instrs[key] += cpu.instr_count - before;
}

RecompFn Gba::find_overlay(uint32_t addr, bool thumb) const {
    for (const auto& o : overlays_) {
        if (addr < o.ram || addr >= o.ram + o.size) continue;
        // A cópia na RAM ainda é idêntica à ROM? (o jogo pode sobrescrever a região)
        const uint8_t* ram = nullptr;
        if ((o.ram >> 24) == 0x03) ram = &bus.iwram[o.ram & 0x7FFF];
        else if ((o.ram >> 24) == 0x02) ram = &bus.ewram[o.ram & 0x3FFFF];
        if (!ram || std::memcmp(ram, rom_.data() + (o.rom - 0x08000000), o.size) != 0) return nullptr;
        auto it = recomp_.find(addr | (thumb ? 1u : 0u));
        return it == recomp_.end() ? nullptr : it->second;
    }
    return nullptr;
}

void Gba::run_hybrid_until(uint32_t ret) {
    uint64_t* region = nullptr; // contador do trecho interpretado atual (estatística)
    for (;;) {
        if (cpu.r[15] == ret) return;
        if (RecompFn fn = find(cpu.r[15], cpu.t_)) {
            ++recomp_calls;
            region = nullptr;
            fn(*this);
            continue;
        }
        if (cpu.halted) {
            halt_wait();
            continue;
        }
        if (!region) region = &interp_regions[cpu.r[15] | (cpu.t_ ? 1u : 0u)];
        ++*region;
        cycles += cpu.step();
        if (cycles >= next_event) sync();
    }
}

void Gba::unimplemented(uint32_t pc, const char* what) {
    char msg[160];
    std::snprintf(msg, sizeof msg, "código recompilado: '%s' não suportado em 0x%08X", what, pc);
    throw std::runtime_error(msg);
}

void Gba::run(const std::function<bool()>& on_frame) {
    on_frame_ = on_frame;
    try {
        run_hybrid_until(0xFFFFFFFF); // nunca retorna: só sai por StopRequested
    } catch (const StopRequested&) {
    }
    on_frame_ = nullptr;
}

} // namespace gba
