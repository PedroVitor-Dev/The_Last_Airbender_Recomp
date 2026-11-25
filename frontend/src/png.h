#pragma once
// Gravador PNG mínimo (deflate "stored", sem compressão) — sem dependências externas.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace png {

inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// pixels em 0xAARRGGBB
inline bool write_rgb(const std::string& path, const uint32_t* pixels, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const uint32_t p = pixels[y * w + x];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + len == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(len));
        z.push_back(static_cast<uint8_t>(len >> 8));
        z.push_back(static_cast<uint8_t>(~len));
        z.push_back(static_cast<uint8_t>(~len >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    const uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back(static_cast<uint8_t>(adler >> (i * 8)));

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [](uint32_t v, uint8_t* o) { o[0] = uint8_t(v >> 24); o[1] = uint8_t(v >> 16); o[2] = uint8_t(v >> 8); o[3] = uint8_t(v); };
    auto chunk = [&](const char* tag, const std::vector<uint8_t>& data) {
        uint8_t hdr[8];
        be32(static_cast<uint32_t>(data.size()), hdr);
        std::memcpy(hdr + 4, tag, 4);
        std::fwrite(hdr, 1, 8, f);
        if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
        uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(tag), 4);
        crc = crc32(data.data(), data.size(), crc);
        uint8_t c[4];
        be32(crc, c);
        std::fwrite(c, 1, 4, f);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr(13);
    be32(static_cast<uint32_t>(w), &ihdr[0]);
    be32(static_cast<uint32_t>(h), &ihdr[4]);
    ihdr[8] = 8;  // bits
    ihdr[9] = 2;  // RGB
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(f);
    return true;
}

} // namespace png
