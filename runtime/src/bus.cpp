#include <cstring>

#include "gba/gba.h"

namespace gba {

// ───────────────────────────────────────────────────────────── EEPROM ──
// Protocolo serial da EEPROM (via DMA3, 1 bit por halfword, MSB primeiro):
//   leitura : "11" + endereço + "0"  → depois 4 bits lixo + 64 bits de dados
//   escrita : "10" + endereço + 64 bits + "0"  → depois lê 1 (pronto)

void Eeprom::set_addr_bits(int bits) { addr_bits_ = bits; }

void Eeprom::write_bit(uint16_t v) {
    const uint32_t bit = v & 1;
    buffer_ = (buffer_ << 1) | bit;
    ++bits_;
    switch (state_) {
    case State::Idle:
        state_ = State::Command;
        buffer_ = bit;
        bits_ = 1;
        break;
    case State::Command:
        if (bits_ == 2) {
            cmd_ = static_cast<uint32_t>(buffer_ & 3);
            state_ = State::Address;
            buffer_ = 0;
            bits_ = 0;
        }
        break;
    case State::Address:
        if (bits_ == static_cast<uint32_t>(addr_bits_)) {
            addr_ = static_cast<uint32_t>(buffer_) & 0x3FF;
            buffer_ = 0;
            bits_ = 0;
            state_ = cmd_ == 3 ? State::End : State::WriteData;
        }
        break;
    case State::WriteData:
        if (bits_ == 64) {
            const uint64_t d = buffer_;
            const size_t base = (addr_ * 8u) % data_.size();
            for (int i = 0; i < 8; ++i) data_[base + i] = static_cast<uint8_t>(d >> (56 - 8 * i));
            dirty_ = true;
            buffer_ = 0;
            bits_ = 0;
            state_ = State::End;
        }
        break;
    case State::End:
        if (cmd_ == 3) {
            reading_ = true;
            read_pos_ = 0;
        }
        state_ = State::Idle;
        buffer_ = 0;
        bits_ = 0;
        break;
    default:
        state_ = State::Idle;
        break;
    }
}

uint16_t Eeprom::read_bit() {
    if (!reading_) return 1; // pronto
    const int pos = read_pos_++;
    if (pos < 4) return 0;
    if (pos >= 68) {
        reading_ = false;
        return 1;
    }
    const int bit = pos - 4;
    const size_t base = (addr_ * 8u) % data_.size();
    const uint8_t byte = data_[base + bit / 8];
    if (pos == 67) reading_ = false;
    return (byte >> (7 - bit % 8)) & 1;
}

// ──────────────────────────────────────────────────────────────── Bus ──

namespace {
template <typename T> T load(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof v); return v; }
template <typename T> void store(uint8_t* p, T v) { std::memcpy(p, &v, sizeof v); }
} // namespace

Bus::Bus(Gba& gba) : gba_(gba) {}

uint32_t Bus::access_cycles(uint32_t a, bool wide) {
    switch (a >> 24) {
    case 0x02: return wide ? 6 : 3;
    case 0x05: case 0x06: case 0x07: return wide ? 2 : 1;
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: return wide ? 6 : 3;
    case 0x0E: case 0x0F: return 5;
    default: return 1;
    }
}

#define BUS_READ(T, IO_READ)                                                                     \
    switch (a >> 24) {                                                                           \
    case 0x00: return a < 0x4000 ? load<T>(&bios[a & (0x4000 - sizeof(T))]) : T(0);             \
    case 0x02: return load<T>(&ewram[a & (0x40000 - sizeof(T))]);                                \
    case 0x03: return load<T>(&iwram[a & (0x8000 - sizeof(T))]);                                 \
    case 0x04: ++io_accesses; return (a & 0xFFFFFF) < 0x400 ? IO_READ : T(0);                    \
    case 0x05: return load<T>(&palette[a & (0x400 - sizeof(T))]);                               \
    case 0x06: return load<T>(&vram[vram_offset(a) & ~(uint32_t)(sizeof(T) - 1)]);               \
    case 0x07: return load<T>(&oam[a & (0x400 - sizeof(T))]);                                    \
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {                          \
        const uint32_t off = (a & 0x1FFFFFF) & ~(uint32_t)(sizeof(T) - 1);                       \
        if (off + sizeof(T) <= rom->size()) return load<T>(rom->data() + off);                   \
        if ((a >> 24) == 0x0D) return T(eeprom.read_bit());                                      \
        return T((a >> 1) & 0xFFFF);                                                             \
    }                                                                                            \
    case 0x0E: case 0x0F: return T(~T(0));                                                       \
    default: return T(0);                                                                        \
    }

uint8_t Bus::read8(uint32_t a) { BUS_READ(uint8_t, gba_.io.read8(a & 0x3FF)) }
uint16_t Bus::read16(uint32_t a) { BUS_READ(uint16_t, gba_.io.read16(a & 0x3FE)) }
uint32_t Bus::read32(uint32_t a) {
    BUS_READ(uint32_t, uint32_t(gba_.io.read16(a & 0x3FC)) | (uint32_t(gba_.io.read16((a & 0x3FC) + 2)) << 16))
}

#undef BUS_READ

void Bus::write8(uint32_t a, uint8_t v) {
    switch (a >> 24) {
    case 0x02: ewram[a & 0x3FFFF] = v; break;
    case 0x03: iwram[a & 0x7FFF] = v; break;
    case 0x04: ++io_accesses; if ((a & 0xFFFFFF) < 0x400) gba_.io.write8(a & 0x3FF, v); break;
    case 0x05: store<uint16_t>(&palette[a & 0x3FE], static_cast<uint16_t>(v * 0x101)); break;
    case 0x06: {
        // Escrita de byte em VRAM duplica no halfword (só na área de BG); OBJ ignora.
        const uint32_t o = vram_offset(a);
        if (o < 0x10000) store<uint16_t>(&vram[o & ~1u], static_cast<uint16_t>(v * 0x101));
        break;
    }
    default: break; // OAM ignora byte; ROM/SRAM não graváveis aqui
    }
}

void Bus::write16(uint32_t a, uint16_t v) {
    switch (a >> 24) {
    case 0x02: store(&ewram[a & 0x3FFFE], v); break;
    case 0x03: store(&iwram[a & 0x7FFE], v); break;
    case 0x04: ++io_accesses; if ((a & 0xFFFFFF) < 0x400) gba_.io.write16(a & 0x3FE, v); break;
    case 0x05: store(&palette[a & 0x3FE], v); break;
    case 0x06: store(&vram[vram_offset(a) & ~1u], v); break;
    case 0x07: store(&oam[a & 0x3FE], v); break;
    case 0x0D: eeprom.write_bit(v); break;
    default: break;
    }
}

void Bus::write32(uint32_t a, uint32_t v) {
    switch (a >> 24) {
    case 0x02: store(&ewram[a & 0x3FFFC], v); break;
    case 0x03: store(&iwram[a & 0x7FFC], v); break;
    case 0x04:
        ++io_accesses;
        if ((a & 0xFFFFFF) < 0x400) {
            gba_.io.write16(a & 0x3FC, static_cast<uint16_t>(v));
            gba_.io.write16((a & 0x3FC) + 2, static_cast<uint16_t>(v >> 16));
        }
        break;
    case 0x05: store(&palette[a & 0x3FC], v); break;
    case 0x06: store(&vram[vram_offset(a) & ~3u], v); break;
    case 0x07: store(&oam[a & 0x3FC], v); break;
    default: break;
    }
}

} // namespace gba
