// tlarecomp synth — gera uma "ROM sintética" para testar o emissor instrução a instrução.
//
// Cada função é uma sequência aleatória de instruções ARM ou Thumb válidas, sem desvios
// nem escritas no PC, terminada em "bx lr". A ROM passa pelo mesmo analyze/emit do jogo e o
// test_synth compara interpretador × recompilado em cada função.

#include "synth.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

#include "core/armv4t.h"

namespace recomp {

namespace {

using core::arm::Instr;
using core::arm::Op;

bool acceptable(const Instr& in) {
    switch (in.op) {
    case Op::B: case Op::BL: case Op::BX: case Op::SWI: case Op::UNDEFINED: case Op::COPROC:
    case Op::THUMB_BL_HI: case Op::THUMB_BL_LO:
        return false;
    case Op::MSR:
        // Só flags: trocar de modo no meio de um teste não acrescenta nada aqui.
        return in.psr_mask == 8 && !in.spsr;
    case Op::MRS:
        return !in.spsr && in.rd < 13;
    case Op::LDM:
        // Sem lista vazia e sem banco de usuário (o emissor rejeita); LDM não pode tocar
        // sp/lr/pc (a função precisa voltar).
        return in.reg_list != 0 && !in.user_bank && !(in.reg_list & 0xE000) && in.rn < 13;
    case Op::STM:
        return in.reg_list != 0 && !in.user_bank && in.rn < 13;
    default:
        break;
    }
    if (in.writes_pc()) return false;
    // Casos IMPREVISÍVEIS no ARM7 (PC como Rs, PC em multiplicação): sem comportamento
    // definido no hardware, não há o que comparar.
    if (in.is_data_processing() && in.op2.shift_by_reg && in.op2.rs == 15) return false;
    if ((in.op >= Op::MUL && in.op <= Op::SMLAL) && (in.rm == 15 || in.rs == 15 || in.rn == 15 || in.rd == 15))
        return false;
    // Não mexer no sp nem no lr: a função precisa conseguir voltar.
    if (in.is_data_processing() && !in.is_compare() && (in.rd == 13 || in.rd == 14)) return false;
    if ((in.op == Op::LDR || in.op == Op::LDRB || in.op == Op::LDRH || in.op == Op::LDRSB || in.op == Op::LDRSH) &&
        (in.rd >= 13))
        return false;
    if ((in.op == Op::MUL || in.op == Op::MLA) && in.rd >= 13) return false;
    if (in.op >= Op::UMULL && in.op <= Op::SMLAL && (in.rd >= 13 || in.rn >= 13 || in.rd == in.rn)) return false;
    if ((in.op == Op::SWP || in.op == Op::SWPB) && (in.rd >= 13 || in.rn == 15 || in.rm == 15)) return false;
    // Escrita de volta na base sp/lr/pc também quebraria o retorno.
    if (in.writeback && in.rn >= 13) return false;
    return true;
}

} // namespace

bool write_synth_rom(const std::string& rom_path, const std::string& config_path, uint32_t count, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> rom(0x200, 0);
    std::string cfg = "# Gerado por tlarecomp synth — funções sintéticas para test_synth\n";

    auto put16 = [&](uint16_t v) { rom.push_back(uint8_t(v)); rom.push_back(uint8_t(v >> 8)); };
    auto put32 = [&](uint32_t v) { put16(uint16_t(v)); put16(uint16_t(v >> 16)); };

    for (uint32_t f = 0; f < count; ++f) {
        const bool thumb = (f & 1) == 0;
        while (rom.size() % 4) rom.push_back(0);
        const uint32_t addr = 0x08000000u + static_cast<uint32_t>(rom.size());
        const int n = 1 + static_cast<int>(rng() % 12);
        for (int i = 0; i < n;) {
            const uint32_t pc = 0x08000000u + static_cast<uint32_t>(rom.size());
            if (thumb) {
                const uint16_t h = static_cast<uint16_t>(rng());
                if (h == 0) continue; // 0x0000 encerra blocos na análise (padding)
                const Instr in = core::arm::decode_thumb(pc, h, 0);
                if (in.size != 2 || !acceptable(in)) continue;
                put16(h);
            } else {
                const uint32_t w = rng();
                const Instr in = core::arm::decode_arm(pc, w);
                if (in.cond == core::arm::Cond::NV || !acceptable(in)) continue;
                put32(w);
            }
            ++i;
        }
        if (thumb) put16(0x4770);      // bx lr
        else put32(0xE12FFF1E);        // bx lr
        char line[64];
        std::snprintf(line, sizeof line, "func 0x%08X %s\n", addr, thumb ? "thumb" : "arm");
        cfg += line;
    }
    while (rom.size() < 0x10000) rom.push_back(0);

    std::ofstream(rom_path, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()),
                                                   static_cast<std::streamsize>(rom.size()));
    std::ofstream(config_path) << cfg;
    return true;
}

} // namespace recomp
