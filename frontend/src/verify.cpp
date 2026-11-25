// tlaverify — teste diferencial: código recompilado × interpretador, função a função.
//
//   tlaverify <rom> [--frames N] [--per-function K] [--fuzz K] [--seed S]
//                   [--press TECLA@ini-fim ...] [--report arq] [--only 0xADDR]
//
// Fase 1 (replay): o jogo roda no interpretador com o roteiro de teclas. Nas primeiras K
// chamadas de cada função recompilada, o estado completo é copiado para duas instâncias de
// rascunho que executam a função até ela retornar — uma interpretada, outra recompilada.
//
// Fase 2 (fuzz, opcional): a partir do estado final do jogo, TODAS as funções da tabela são
// chamadas diretamente, K vezes cada, com registradores sorteados (zeros, valores pequenos,
// ponteiros válidos para EWRAM/IWRAM/ROM, valores reais) e um endereço de retorno mágico.
// Entradas sem sentido para o jogo ainda precisam produzir o mesmo resultado nos dois.
//
// Nos dois casos: IRQs mascaradas, vigia de ciclos, e registradores, flags e memórias
// (EWRAM, IWRAM, VRAM, paleta, OAM) precisam ficar idênticos. Divergências em execuções que
// acessaram IO são marcadas como inconclusivas (timing difere entre os modos).

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/rom.h"
#include "difftest.h"
#include "gba/gba.h"

extern const gba::RecompEntry tla_recomp_table[];
extern const size_t tla_recomp_count;
extern const gba::RecompOverlay tla_recomp_overlays[];
extern const size_t tla_recomp_overlay_count;

using namespace difftest;

namespace {

constexpr uint64_t kReplayBudget = 16 * 280896; // ~16 quadros
constexpr uint64_t kFuzzBudget = 4 * 280896;    // ~4 quadros
constexpr uint32_t kMagicReturn = 0x08000004;   // endereço de retorno do fuzz (sem código)

struct Press {
    uint16_t key;
    uint64_t from, to;
};

uint16_t key_from_name(const std::string& n) {
    static const std::pair<const char*, uint16_t> keys[] = {
        {"A", gba::KEY_A}, {"B", gba::KEY_B}, {"SELECT", gba::KEY_SELECT}, {"START", gba::KEY_START},
        {"RIGHT", gba::KEY_RIGHT}, {"LEFT", gba::KEY_LEFT}, {"UP", gba::KEY_UP}, {"DOWN", gba::KEY_DOWN},
        {"R", gba::KEY_R}, {"L", gba::KEY_L}};
    for (const auto& [name, k] : keys)
        if (n == name) return k;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: tlaverify <rom> [--frames N] [--per-function K] [--fuzz K] [--seed S] "
                             "[--press TECLA@ini-fim] [--report arq] [--only 0xADDR]\n");
        return 2;
    }
    uint64_t frames = 3000;
    int per_function = 2, fuzz = 0;
    uint32_t seed = 1, only = 0;
    std::string report_path = "build/verify_report.txt";
    std::vector<Press> script;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::stoull(argv[++i]);
        else if (a == "--per-function" && i + 1 < argc) per_function = std::stoi(argv[++i]);
        else if (a == "--fuzz" && i + 1 < argc) fuzz = std::stoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--only" && i + 1 < argc) only = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
        else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            const std::string s = argv[++i];
            const auto at = s.find('@'), dash = s.find('-', at);
            Press p{key_from_name(s.substr(0, at)), std::stoull(s.substr(at + 1, dash - at - 1)), 0};
            p.to = dash == std::string::npos ? p.from + 5 : std::stoull(s.substr(dash + 1));
            script.push_back(p);
        } else {
            std::fprintf(stderr, "opção desconhecida: %s\n", a.c_str());
            return 2;
        }
    }

    std::string err;
    auto rom = core::Rom::load(argv[1], &err);
    if (!rom || !rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM inválida ou não suportada %s\n", err.c_str());
        return 1;
    }
    if (tla_recomp_count == 0) {
        std::fprintf(stderr, "erro: build sem código recompilado (rode tlarecomp emit e recompile)\n");
        return 1;
    }

    auto main_g = std::make_unique<gba::Gba>(*rom); // jogo de referência (interpretador)
    Verifier verifier(*rom, tla_recomp_table, tla_recomp_count, tla_recomp_overlays, tla_recomp_overlay_count);

    // ── Fase 1: replay de chamadas reais ──
    std::unordered_map<uint32_t, int> entries;
    for (size_t i = 0; i < tla_recomp_count; ++i) entries[tla_recomp_table[i].addr] = 0;
    Tally replay;
    bool testing = false;
    uint32_t last_pc = 0;

    main_g->cpu.on_exec = [&](uint32_t pc, bool thumb) {
        if (testing) return;
        const uint32_t prev = last_pc;
        last_pc = pc;
        // Só entradas recém-chamadas: lr aponta para logo depois da instrução anterior.
        if ((main_g->cpu.r[14] & ~1u) != prev + 4) return;
        const uint32_t key = pc | (thumb ? 1u : 0u);
        if (only && (key & ~1u) != only) return;
        auto it = entries.find(key);
        if (it == entries.end() || it->second >= per_function) return;
        ++it->second;
        testing = true;
        const uint32_t ret = main_g->cpu.r[14] & ~1u;
        replay.add(key, verifier.evaluate(*main_g, pc, thumb, ret, kReplayBudget, [](gba::Gba&) {}));
        testing = false;
    };

    auto keys_for = [&](uint64_t f) {
        uint16_t k = 0;
        for (const auto& p : script)
            if (f >= p.from && f <= p.to) k |= p.key;
        return k;
    };
    auto t0 = std::chrono::steady_clock::now();
    main_g->set_keys(keys_for(0));
    main_g->run([&] {
        const uint64_t f = main_g->frame_count;
        if (f % 1000 == 0)
            std::printf("  quadro %llu: %zu funções testadas, %zu divergências\n", static_cast<unsigned long long>(f),
                        replay.funcs.size(), replay.failures.size());
        main_g->set_keys(keys_for(f));
        return f < frames;
    });
    main_g->cpu.on_exec = nullptr;
    replay.print("Replay", tla_recomp_count, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    // ── Fase 2: fuzz de todas as funções ──
    Tally fz;
    if (fuzz > 0) {
        t0 = std::chrono::steady_clock::now();
        const uint32_t base_sp = main_g->cpu.r[13] - 0x200; // abaixo da pilha atual, ainda na IWRAM
        for (size_t i = 0; i < tla_recomp_count; ++i) {
            const uint32_t key = tla_recomp_table[i].addr;
            const uint32_t pc = key & ~1u;
            const bool thumb = key & 1;
            if (only && pc != only) continue;
            for (int k = 0; k < fuzz; ++k) {
                std::mt19937 rng(seed * 0x9E3779B9u ^ key ^ (static_cast<uint32_t>(k) << 24));
                std::array<uint32_t, 16> regs{};
                for (int r = 0; r < 13; ++r) {
                    switch (rng() % 8) {
                    case 0: regs[r] = 0; break;
                    case 1: regs[r] = rng() % 4; break;
                    case 2: regs[r] = rng() % 256; break;
                    case 3: regs[r] = 0x02000000u + (rng() % 0x3FF00u & ~3u); break;
                    case 4: regs[r] = 0x03000000u + (rng() % 0x7000u & ~3u); break;
                    case 5: regs[r] = 0x08000000u + (rng() % 0x700000u & ~3u); break;
                    case 6: regs[r] = main_g->cpu.r[r]; break;
                    default: regs[r] = rng(); break;
                    }
                }
                const uint32_t flags = rng() & 0xF0000000u;
                const uint32_t ret = kMagicReturn;
                const Result res = verifier.evaluate(*main_g, pc, thumb, ret, kFuzzBudget, [&](gba::Gba& g) {
                    for (int r = 0; r < 13; ++r) g.cpu.r[r] = regs[r];
                    g.cpu.set_cpsr(flags | gba::MODE_SYS);
                    g.cpu.r[13] = base_sp;
                    g.cpu.r[14] = ret | (thumb ? 1u : 0u);
                });
                fz.add(key, res);
            }
            if ((i + 1) % 500 == 0)
                std::printf("  fuzz: %zu/%zu funções, %zu divergências\n", i + 1, tla_recomp_count, fz.failures.size());
        }
        fz.print("Fuzz", tla_recomp_count, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

    std::ofstream rep(report_path);
    replay.dump(rep, "replay", 10);
    if (fuzz > 0) fz.dump(rep, "fuzz", 25);
    std::printf("Relatório: %s\n", report_path.c_str());
    return replay.failures.empty() && fz.failures.empty() ? 0 : 1;
}
