#include "core/rom.h"

#include <array>
#include <cstdio>
#include <fstream>

namespace core {

namespace {

std::string read_fixed_string(const std::vector<uint8_t>& d, size_t off, size_t len) {
    std::string s;
    for (size_t i = 0; i < len && off + i < d.size(); ++i) {
        if (d[off + i] == 0) break;
        s.push_back(static_cast<char>(d[off + i]));
    }
    return s;
}

uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

} // namespace

std::string sha1_hex(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t bit_len = static_cast<uint64_t>(size) * 8;

    auto process = [&h](const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };

    size_t full = size / 64;
    for (size_t i = 0; i < full; ++i) process(data + i * 64);

    std::array<uint8_t, 128> tail{};
    size_t rem = size % 64;
    for (size_t i = 0; i < rem; ++i) tail[i] = data[full * 64 + i];
    tail[rem] = 0x80;
    size_t tail_len = (rem < 56) ? 64 : 128;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    process(tail.data());
    if (tail_len == 128) process(tail.data() + 64);

    char out[41];
    for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 40);
}

std::optional<Rom> Rom::load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (error) *error = "não foi possível abrir " + path.string();
        return std::nullopt;
    }
    auto size = static_cast<size_t>(f.tellg());
    if (size < 0xC0 || size > 32 * 1024 * 1024) {
        if (error) *error = "tamanho de ROM inválido";
        return std::nullopt;
    }
    Rom rom;
    rom.data_.resize(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(rom.data_.data()), static_cast<std::streamsize>(size));

    rom.header_.title = read_fixed_string(rom.data_, 0xA0, 12);
    rom.header_.game_code = read_fixed_string(rom.data_, 0xAC, 4);
    rom.header_.maker_code = read_fixed_string(rom.data_, 0xB0, 2);
    rom.header_.version = rom.data_[0xBC];
    uint32_t branch = rom.read32(kRomBase);
    rom.header_.entry_point = kRomBase + 8 + ((branch & 0x00FFFFFF) << 2);
    return rom;
}

std::string Rom::sha1() const { return sha1_hex(data_.data(), data_.size()); }

bool Rom::is_supported() const {
    return header_.game_code == kExpectedGameCode && sha1() == kExpectedSha1;
}

uint32_t Rom::read32(uint32_t addr) const {
    uint32_t off = (addr - kRomBase) & ~3u;
    if (off + 4 > data_.size()) return 0;
    return uint32_t(data_[off]) | (uint32_t(data_[off + 1]) << 8) |
           (uint32_t(data_[off + 2]) << 16) | (uint32_t(data_[off + 3]) << 24);
}

uint16_t Rom::read16(uint32_t addr) const {
    uint32_t off = (addr - kRomBase) & ~1u;
    if (off + 2 > data_.size()) return 0;
    return static_cast<uint16_t>(data_[off] | (data_[off + 1] << 8));
}

} // namespace core
