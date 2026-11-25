#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace core {

inline constexpr uint32_t kRomBase = 0x08000000;
inline constexpr const char* kExpectedSha1 = "9d864e9b3ccce4e5e1b1c566afa0d06088e88dfd";
inline constexpr const char* kExpectedGameCode = "BQZE";

struct RomHeader {
    std::string title;      // 0xA0, 12 bytes
    std::string game_code;  // 0xAC, 4 bytes
    std::string maker_code; // 0xB0, 2 bytes
    uint8_t version = 0;    // 0xBC
    uint32_t entry_point = 0;
};

class Rom {
public:
    static std::optional<Rom> load(const std::filesystem::path& path, std::string* error = nullptr);

    const std::vector<uint8_t>& bytes() const { return data_; }
    size_t size() const { return data_.size(); }
    const RomHeader& header() const { return header_; }
    std::string sha1() const;
    bool is_supported() const;

    uint32_t read32(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;

private:
    std::vector<uint8_t> data_;
    RomHeader header_;
};

std::string sha1_hex(const uint8_t* data, size_t size);

} // namespace core
