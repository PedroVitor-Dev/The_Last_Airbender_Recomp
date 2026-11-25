#include "gba/gba.h"

namespace gba {

Gba::Gba(const core::Rom& rom) : bus(*this), cpu(*this), io(*this), ppu(*this), rom_(rom.bytes()) {
    bus.rom = &rom_;
    bios_install_stub(bus);
    cpu.reset_after_bios();
}

void Gba::run_until(uint64_t target) {
    uint64_t next_timer = io.next_timer_event(cycles_);
    while (cycles_ < target) {
        if (cpu.halted) {
            if (io.irq_any()) {
                cpu.halted = false;
            } else {
                // Nada a fazer até o próximo evento (fim do trecho ou overflow de timer).
                cycles_ = next_timer < target ? next_timer : target;
                io.update_timers(cycles_);
                next_timer = io.next_timer_event(cycles_);
                continue;
            }
        }
        cycles_ += cpu.step();
        if (cycles_ >= next_timer) {
            io.update_timers(cycles_);
            next_timer = io.next_timer_event(cycles_);
        }
    }
    io.update_timers(cycles_);
}

void Gba::run_frame() {
    for (uint32_t line = 0; line < kLinesPerFrame; ++line) {
        const uint64_t line_start = cycles_;
        uint16_t& stat = io.reg(0x004);
        io.reg(0x006) = static_cast<uint16_t>(line);

        stat &= ~2u; // fim do HBlank
        if (line == kScreenH) {
            stat |= 1; // VBlank
            ppu.latch_affine();
            io.on_vblank();
        } else if (line == kLinesPerFrame - 1) {
            stat &= ~1u;
        }
        const bool match = static_cast<uint32_t>(stat >> 8) == line;
        if (match) {
            stat |= 4;
            if (stat & 0x20) io.raise(IRQ_VCOUNT);
        } else {
            stat &= ~4u;
        }

        run_until(line_start + kHDrawCycles);
        if (line < static_cast<uint32_t>(kScreenH)) ppu.render_line(static_cast<int>(line));
        io.on_hblank(static_cast<int>(line));
        run_until(line_start + kCyclesPerLine);
    }
    ++frame_count;
}

} // namespace gba
