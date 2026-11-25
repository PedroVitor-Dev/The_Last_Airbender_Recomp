// BIOS do GBA em HLE. Nenhum código da BIOS original é usado: o único código ARM na
// região da BIOS é o stub do vetor de IRQ abaixo, escrito para este projeto.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gba/gba.h"

namespace gba {

void bios_install_stub(Bus& bus) {
    // Vetor de IRQ (0x18). Mesmo contrato da BIOS: salva r0-r3, r12, lr; chama o handler
    // do jogo em [0x03007FFC] (espelho 0x03FFFFFC) em modo ARM; restaura e retorna.
    static const uint32_t irq_stub[] = {
        0xE92D500F, // push {r0-r3, r12, lr}
        0xE3A00301, // mov r0, #0x04000000
        0xE28FE000, // add lr, pc, #0          (lr = endereço do ldmfd)
        0xE510F004, // ldr pc, [r0, #-4]       (handler em 0x03FFFFFC)
        0xE8BD500F, // pop {r0-r3, r12, lr}
        0xE25EF004, // subs pc, lr, #4         (retorna e restaura o CPSR)
    };
    std::memcpy(&bus.bios[0x18], irq_stub, sizeof irq_stub);
}

namespace {

uint32_t& R(Gba& g, int n) { return g.cpu.r[n]; }

int16_t arctan(int32_t i) {
    // Polinômio usado pela BIOS (entrada e saída em ponto fixo 1.14 / 0..0x4000).
    const int32_t a = -((i * i) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return static_cast<int16_t>((i * b) >> 16);
}

uint16_t arctan2(int32_t x, int32_t y) {
    if (!y) return x >= 0 ? 0 : 0x8000;
    if (!x) return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return static_cast<uint16_t>(arctan((y << 14) / x));
        } else if (-x >= y) {
            return static_cast<uint16_t>(arctan((y << 14) / x) + 0x8000);
        }
        return static_cast<uint16_t>(0x4000 - arctan((x << 14) / y));
    }
    if (x <= 0) {
        if (-x > -y) return static_cast<uint16_t>(arctan((y << 14) / x) + 0x8000);
    } else if (x >= -y) {
        return static_cast<uint16_t>(arctan((y << 14) / x) + 0x10000);
    }
    return static_cast<uint16_t>(0xC000 - arctan((x << 14) / y));
}

void cpu_set(Gba& g, bool fast) {
    uint32_t src = R(g, 0), dst = R(g, 1);
    const uint32_t ctrl = R(g, 2);
    uint32_t count = ctrl & 0x1FFFFF;
    const bool fill = (ctrl >> 24) & 1;
    const bool wide = fast || ((ctrl >> 26) & 1);
    if (fast) count = (count + 7) & ~7u;
    Bus& b = g.bus;
    if (wide) {
        src &= ~3u, dst &= ~3u;
        const uint32_t fv = b.read32(src);
        for (uint32_t i = 0; i < count; ++i) {
            b.write32(dst + i * 4, fill ? fv : b.read32(src + i * 4));
        }
    } else {
        src &= ~1u, dst &= ~1u;
        const uint16_t fv = b.read16(src);
        for (uint32_t i = 0; i < count; ++i) {
            b.write16(dst + i * 2, fill ? fv : b.read16(src + i * 2));
        }
    }
}

// Copia o resultado descomprimido para o destino (byte a byte ou em halfwords para VRAM).
void emit(Gba& g, uint32_t dst, const std::vector<uint8_t>& out, bool vram) {
    if (vram) {
        for (size_t i = 0; i + 1 < out.size(); i += 2)
            g.bus.write16(dst + static_cast<uint32_t>(i), static_cast<uint16_t>(out[i] | (out[i + 1] << 8)));
        if (out.size() & 1) g.bus.write16(dst + static_cast<uint32_t>(out.size() - 1), out.back());
    } else {
        for (size_t i = 0; i < out.size(); ++i) g.bus.write8(dst + static_cast<uint32_t>(i), out[i]);
    }
}

void lz77(Gba& g, bool vram) {
    uint32_t src = R(g, 0);
    const uint32_t header = g.bus.read32(src & ~3u);
    src = (src & ~3u) + 4;
    const uint32_t size = header >> 8;
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size) {
        const uint8_t flags = g.bus.read8(src++);
        for (int bit = 7; bit >= 0 && out.size() < size; --bit) {
            if (flags & (1 << bit)) {
                const uint8_t b0 = g.bus.read8(src++), b1 = g.bus.read8(src++);
                const uint32_t disp = (((b0 & 0xF) << 8) | b1) + 1;
                uint32_t len = (b0 >> 4) + 3;
                while (len-- && out.size() < size) {
                    out.push_back(disp <= out.size() ? out[out.size() - disp] : 0);
                }
            } else {
                out.push_back(g.bus.read8(src++));
            }
        }
    }
    emit(g, R(g, 1), out, vram);
}

void rl(Gba& g, bool vram) {
    uint32_t src = R(g, 0);
    const uint32_t header = g.bus.read32(src & ~3u);
    src = (src & ~3u) + 4;
    const uint32_t size = header >> 8;
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size) {
        const uint8_t flag = g.bus.read8(src++);
        if (flag & 0x80) {
            const uint8_t v = g.bus.read8(src++);
            for (uint32_t i = 0; i < (flag & 0x7Fu) + 3 && out.size() < size; ++i) out.push_back(v);
        } else {
            for (uint32_t i = 0; i < (flag & 0x7Fu) + 1 && out.size() < size; ++i) out.push_back(g.bus.read8(src++));
        }
    }
    emit(g, R(g, 1), out, vram);
}

void bit_unpack(Gba& g) {
    uint32_t src = R(g, 0), dst = R(g, 1);
    const uint32_t info = R(g, 2);
    const uint16_t len = g.bus.read16(info);
    const uint8_t sw = g.bus.read8(info + 2), dw = g.bus.read8(info + 3);
    const uint32_t offset = g.bus.read32(info + 4);
    const bool zero_too = offset >> 31;
    const uint32_t add = offset & 0x7FFFFFFF;
    uint32_t acc = 0;
    int acc_bits = 0;
    for (uint32_t i = 0; i < len; ++i) {
        const uint8_t byte = g.bus.read8(src++);
        for (int b = 0; b < 8; b += sw) {
            uint32_t v = (byte >> b) & ((1u << sw) - 1);
            if (v || zero_too) v += add;
            acc |= (v & ((dw == 32) ? 0xFFFFFFFFu : ((1u << dw) - 1))) << acc_bits;
            acc_bits += dw;
            if (acc_bits >= 32) {
                g.bus.write32(dst, acc);
                dst += 4;
                acc = 0;
                acc_bits = 0;
            }
        }
    }
}

void register_ram_reset(Gba& g) {
    const uint32_t f = R(g, 0);
    if (f & 0x01) g.bus.ewram.fill(0);
    if (f & 0x02) std::fill(g.bus.iwram.begin(), g.bus.iwram.end() - 0x200, uint8_t(0));
    if (f & 0x04) g.bus.palette.fill(0);
    if (f & 0x08) g.bus.vram.fill(0);
    if (f & 0x10) g.bus.oam.fill(0);
}

} // namespace

bool bios_swi(Gba& g, uint32_t n) {
    switch (n) {
    case 0x00: // SoftReset
        g.cpu.reset_after_bios();
        return true;
    case 0x01: register_ram_reset(g); return true;
    case 0x02: // Halt
    case 0x03: // Stop
        g.cpu.halted = true;
        return true;
    case 0x06: case 0x07: { // Div / DivArm
        int32_t num = static_cast<int32_t>(R(g, 0)), den = static_cast<int32_t>(R(g, 1));
        if (n == 0x07) std::swap(num, den);
        if (den == 0) return true; // a BIOS real trava; evitamos o crash do host
        const int32_t q = (num == INT32_MIN && den == -1) ? INT32_MIN : num / den;
        R(g, 0) = static_cast<uint32_t>(q);
        R(g, 1) = static_cast<uint32_t>(num - q * den);
        R(g, 3) = static_cast<uint32_t>(q < 0 ? -q : q);
        return true;
    }
    case 0x08: R(g, 0) = static_cast<uint32_t>(std::sqrt(static_cast<double>(R(g, 0)))); return true;
    case 0x09: R(g, 0) = static_cast<uint32_t>(static_cast<int32_t>(arctan(static_cast<int16_t>(R(g, 0))))); return true;
    case 0x0A:
        R(g, 0) = arctan2(static_cast<int16_t>(R(g, 0)), static_cast<int16_t>(R(g, 1)));
        return true;
    case 0x0B: cpu_set(g, false); return true;
    case 0x0C: cpu_set(g, true); return true;
    case 0x10: bit_unpack(g); return true;
    case 0x11: lz77(g, false); return true;
    case 0x12: lz77(g, true); return true;
    case 0x14: rl(g, false); return true;
    case 0x15: rl(g, true); return true;
    case 0xAB: return true; // semihosting de debug (printf do desenvolvedor) — ignorado
    default: {
        // Avisa uma vez por número (código lixo executado pode gerar milhares).
        static std::array<bool, 256> warned{};
        if (!warned[n & 0xFF]) {
            warned[n & 0xFF] = true;
            std::fprintf(stderr, "[bios] SWI 0x%02X não suportada (pc=0x%08X)\n", n, g.cpu.r[15]);
        }
        return false;
    }
    }
}

} // namespace gba
