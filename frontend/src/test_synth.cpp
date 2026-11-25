// test_synth — fuzzer de instruções: interpretador × código recompilado sobre a ROM sintética
// gerada por "tlarecomp synth" (sequências aleatórias de instruções terminadas em bx lr).
//
//   test_synth <synth.gba> [--runs K] [--seed S] [--report arq]
//
// Os registradores recebem valores escolhidos para atingir casos-limite (deslocamentos de
// 0/31/32/33, 0x80000000, 0xFFFFFFFF...) e ponteiros (alinhados ou não) para EWRAM/IWRAM.

#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include "core/rom.h"
#include "difftest.h"
#include "gba/gba.h"

extern const gba::RecompEntry tla_recomp_table[];
extern const size_t tla_recomp_count;
extern const gba::RecompOverlay tla_recomp_overlays[];
extern const size_t tla_recomp_overlay_count;

using namespace difftest;

namespace {

constexpr uint32_t kMagicReturn = 0x08000100; // área zerada antes das funções sintéticas
constexpr uint64_t kBudget = 280896;          // 1 quadro: sequências são curtas

uint32_t pick_value(std::mt19937& rng) {
    static const uint32_t edges[] = {0,          1,          2,          7,          8,          31,
                                     32,         33,         63,         64,         255,        256,
                                     0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF, 0x0000FFFF,
                                     0x00008000, 0x000000FF, 0x00000080};
    switch (rng() % 6) {
    case 0: case 1: return edges[rng() % (sizeof edges / sizeof edges[0])];
    case 2: return 0x02000000u + 0x1000u + rng() % 0x3E000u; // EWRAM, qualquer alinhamento
    case 3: return 0x03000000u + 0x400u + rng() % 0x6000u;   // IWRAM, longe da pilha
    case 4: return rng() % 64;
    default: return rng();
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: test_synth <synth.gba> [--runs K] [--seed S] [--report arq]\n");
        return 2;
    }
    int runs = 20;
    uint32_t seed = 1;
    std::string report_path = "build/synth_report.txt";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--runs" && i + 1 < argc) runs = std::stoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
        else {
            std::fprintf(stderr, "opção desconhecida: %s\n", a.c_str());
            return 2;
        }
    }
    std::string err;
    auto rom = core::Rom::load(argv[1], &err);
    if (!rom) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    if (tla_recomp_count == 0) {
        std::fprintf(stderr, "erro: build sem código sintético (rode scripts\\test_synth.ps1)\n");
        return 1;
    }

    // Estado-base: memórias com conteúdo pseudoaleatório para as cargas lerem algo variado.
    auto base = std::make_unique<gba::Gba>(*rom);
    std::mt19937 mem_rng(seed);
    for (auto& b : base->bus.ewram) b = static_cast<uint8_t>(mem_rng());
    for (size_t i = 0; i < 0x7000; ++i) base->bus.iwram[i] = static_cast<uint8_t>(mem_rng());

    Verifier verifier(*rom, tla_recomp_table, tla_recomp_count, tla_recomp_overlays, tla_recomp_overlay_count);
    Tally tally;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < tla_recomp_count; ++i) {
        const uint32_t key = tla_recomp_table[i].addr;
        const uint32_t pc = key & ~1u;
        const bool thumb = key & 1;
        for (int k = 0; k < runs; ++k) {
            std::mt19937 rng(seed * 0x9E3779B9u ^ key ^ (static_cast<uint32_t>(k) << 20));
            std::array<uint32_t, 13> regs{};
            for (auto& r : regs) r = pick_value(rng);
            const uint32_t flags = rng() & 0xF0000000u;
            tally.add(key, verifier.evaluate(*base, pc, thumb, kMagicReturn, kBudget, [&](gba::Gba& g) {
                for (int r = 0; r < 13; ++r) g.cpu.r[r] = regs[r];
                g.cpu.set_cpsr(flags | gba::MODE_SYS);
                g.cpu.r[13] = 0x03007C00;
                g.cpu.r[14] = kMagicReturn | (thumb ? 1u : 0u);
            }));
        }
    }
    tally.print("Fuzz de instruções", tla_recomp_count,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::ofstream rep(report_path);
    tally.dump(rep, "sintético", 25);
    std::printf("Relatório: %s\n", report_path.c_str());
    return tally.failures.empty() ? 0 : 1;
}
