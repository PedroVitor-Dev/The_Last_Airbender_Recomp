#pragma once

#include <cstdint>
#include <string>

namespace recomp {

// Gera uma ROM com `count` funções sintéticas (alternando Thumb/ARM) e o config com as
// entradas "func" correspondentes.
bool write_synth_rom(const std::string& rom_path, const std::string& config_path, uint32_t count, uint32_t seed);

} // namespace recomp
