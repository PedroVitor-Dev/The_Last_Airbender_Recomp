#pragma once
// Funções auxiliares (inline) usadas pelo código C++ gerado pelo recompilador.
// Mantêm a mesma semântica do interpretador (runtime/src/cpu.cpp).

#include <bit>
#include <cstdint>
#include <cstring>

#include "gba/gba.h"

namespace rt {

using gba::Cpu;
using gba::Gba;

// ───────────────────────────────────────────────────────────── memória ──

inline uint32_t ld32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t ld16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

inline uint32_t rd32(Gba& g, uint32_t a) {
    switch (a >> 24) {
    case 0x02: return ld32(&g.bus.ewram[a & 0x3FFFC]);
    case 0x03: return ld32(&g.bus.iwram[a & 0x7FFC]);
    default: return g.bus.read32(a);
    }
}
inline uint32_t rd16(Gba& g, uint32_t a) {
    switch (a >> 24) {
    case 0x02: return ld16(&g.bus.ewram[a & 0x3FFFE]);
    case 0x03: return ld16(&g.bus.iwram[a & 0x7FFE]);
    default: return g.bus.read16(a);
    }
}
inline uint32_t rd8(Gba& g, uint32_t a) {
    switch (a >> 24) {
    case 0x02: return g.bus.ewram[a & 0x3FFFF];
    case 0x03: return g.bus.iwram[a & 0x7FFF];
    default: return g.bus.read8(a);
    }
}
inline void wr32(Gba& g, uint32_t a, uint32_t v) {
    switch (a >> 24) {
    case 0x02: std::memcpy(&g.bus.ewram[a & 0x3FFFC], &v, 4); return;
    case 0x03: std::memcpy(&g.bus.iwram[a & 0x7FFC], &v, 4); return;
    default: g.bus.write32(a, v); return;
    }
}
inline void wr16(Gba& g, uint32_t a, uint32_t v) {
    const uint16_t h = static_cast<uint16_t>(v);
    switch (a >> 24) {
    case 0x02: std::memcpy(&g.bus.ewram[a & 0x3FFFE], &h, 2); return;
    case 0x03: std::memcpy(&g.bus.iwram[a & 0x7FFE], &h, 2); return;
    default: g.bus.write16(a, h); return;
    }
}
inline void wr8(Gba& g, uint32_t a, uint32_t v) {
    switch (a >> 24) {
    case 0x02: g.bus.ewram[a & 0x3FFFF] = static_cast<uint8_t>(v); return;
    case 0x03: g.bus.iwram[a & 0x7FFF] = static_cast<uint8_t>(v); return;
    default: g.bus.write8(a, static_cast<uint8_t>(v)); return;
    }
}

// Cargas com as peculiaridades do ARM7 para endereços desalinhados.
inline uint32_t ldr(Gba& g, uint32_t a) { return std::rotr(rd32(g, a & ~3u), static_cast<int>((a & 3) * 8)); }
inline uint32_t ldrh(Gba& g, uint32_t a) { return std::rotr(rd16(g, a & ~1u), static_cast<int>((a & 1) * 8)); }
inline uint32_t ldrsb(Gba& g, uint32_t a) { return static_cast<uint32_t>(int32_t(int8_t(rd8(g, a)))); }
inline uint32_t ldrsh(Gba& g, uint32_t a) {
    return (a & 1) ? static_cast<uint32_t>(int32_t(int8_t(rd8(g, a))))
                   : static_cast<uint32_t>(int32_t(int16_t(rd16(g, a))));
}

// ─────────────────────────────────────────────────────────────── flags ──

inline void nz(Cpu& c, uint32_t r) {
    c.n_ = r >> 31;
    c.z_ = r == 0;
}
inline uint32_t add_wc(Cpu& c, uint32_t a, uint32_t b, bool cin) {
    const uint64_t w = uint64_t(a) + b + cin;
    const uint32_t r = static_cast<uint32_t>(w);
    c.n_ = r >> 31;
    c.z_ = r == 0;
    c.c_ = (w >> 32) != 0;
    c.v_ = ((~(a ^ b) & (a ^ r)) >> 31) != 0;
    return r;
}
inline uint32_t adds(Cpu& c, uint32_t a, uint32_t b) { return add_wc(c, a, b, false); }
inline uint32_t adcs(Cpu& c, uint32_t a, uint32_t b) { return add_wc(c, a, b, c.c_); }
inline uint32_t subs(Cpu& c, uint32_t a, uint32_t b) { return add_wc(c, a, ~b, true); }
inline uint32_t sbcs(Cpu& c, uint32_t a, uint32_t b) { return add_wc(c, a, ~b, c.c_); }

// ─────────────────────────────────── deslocamento por registrador (carry) ──

inline uint32_t lsl_r(uint32_t v, uint32_t amt, bool& carry) {
    if (amt == 0) return v;
    if (amt < 32) { carry = (v >> (32 - amt)) & 1; return v << amt; }
    carry = amt == 32 ? (v & 1) : false;
    return 0;
}
inline uint32_t lsr_r(uint32_t v, uint32_t amt, bool& carry) {
    if (amt == 0) return v;
    if (amt < 32) { carry = (v >> (amt - 1)) & 1; return v >> amt; }
    carry = amt == 32 ? (v >> 31) != 0 : false;
    return 0;
}
inline uint32_t asr_r(uint32_t v, uint32_t amt, bool& carry) {
    if (amt == 0) return v;
    if (amt < 32) {
        carry = (static_cast<int32_t>(v) >> (amt - 1)) & 1;
        return static_cast<uint32_t>(static_cast<int32_t>(v) >> amt);
    }
    carry = (v >> 31) != 0;
    return carry ? 0xFFFFFFFFu : 0;
}
inline uint32_t ror_r(uint32_t v, uint32_t amt, bool& carry) {
    if (amt == 0) return v;
    amt &= 31;
    if (amt) v = std::rotr(v, static_cast<int>(amt));
    carry = (v >> 31) != 0;
    return v;
}

// ───────────────────────────────────────────────────────────────── PSR ──

inline void msr(Cpu& c, bool spsr, uint32_t field_mask, uint32_t v) {
    uint32_t mask = 0;
    if (field_mask & 8) mask |= 0xFF000000;
    if (field_mask & 1) mask |= 0x000000FF;
    if (spsr) {
        if (c.mode_ != gba::MODE_USR && c.mode_ != gba::MODE_SYS) c.spsr_ = (c.spsr_ & ~mask) | (v & mask);
        return;
    }
    if (c.mode_ == gba::MODE_USR) mask &= 0xFF000000;
    const bool t = c.t_;
    c.set_cpsr((c.cpsr() & ~mask) | (v & mask));
    c.t_ = t;
}

} // namespace rt
