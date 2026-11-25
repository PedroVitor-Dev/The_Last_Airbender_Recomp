// tlarecomp — recompilador estático de Avatar: The Last Airbender (GBA).
//
//   tlarecomp info    <rom>                       valida a ROM e mostra o header
//   tlarecomp analyze <rom> [-o dir] [--config arq] [--disasm] [--no-scan]
//                                                 descobre funções e gera relatórios

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include "analysis.h"
#include "emitter.h"
#include "core/rom.h"

namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr,
                 "uso:\n"
                 "  tlarecomp info    <rom.gba>\n"
                 "  tlarecomp analyze <rom.gba> [-o <dir>] [--config <arquivo>] [--disasm] [--no-scan] [--emit <dir>]\n"
                 "  tlarecomp emit    <rom.gba> (= analyze --emit generated/recomp)\n"
                 "  tlarecomp dis     <rom.gba> <endereço> <n> arm|thumb [--at <endereço de execução>]\n");
    return 2;
}

std::optional<core::Rom> load_supported(const char* path) {
    std::string err;
    auto rom = core::Rom::load(path, &err);
    if (!rom) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return std::nullopt;
    }
    if (!rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM não suportada (esperado %s / SHA-1 %s)\n", core::kExpectedGameCode,
                     core::kExpectedSha1);
        return std::nullopt;
    }
    return rom;
}

int cmd_info(const char* path) {
    std::string err;
    auto rom = core::Rom::load(path, &err);
    if (!rom) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    const auto& h = rom->header();
    std::printf("Título : %s\n", h.title.c_str());
    std::printf("Código : %s\n", h.game_code.c_str());
    std::printf("Tamanho: %zu bytes\n", rom->size());
    std::printf("Entrada: 0x%08X\n", h.entry_point);
    std::printf("SHA-1  : %s\n", rom->sha1().c_str());
    if (!rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM não suportada\n");
        return 1;
    }
    std::printf("OK: ROM suportada.\n");
    return 0;
}

// tlarecomp dis <rom> <endereço> <n> arm|thumb [--at <endereço de execução>]
// Desmonta n instruções em linha reta (útil para código copiado para a RAM).
int cmd_dis(int argc, char** argv) {
    if (argc < 4) return usage();
    auto rom = load_supported(argv[0]);
    if (!rom) return 1;
    const uint32_t src = static_cast<uint32_t>(std::stoul(argv[1], nullptr, 0));
    const uint32_t count = static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0));
    const bool thumb = !std::strcmp(argv[3], "thumb");
    uint32_t at = src;
    if (argc >= 6 && !std::strcmp(argv[4], "--at")) at = static_cast<uint32_t>(std::stoul(argv[5], nullptr, 0));
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; ++i) {
        core::arm::Instr in;
        if (thumb) in = core::arm::decode_thumb(at + off, rom->read16(src + off), rom->read16(src + off + 2));
        else in = core::arm::decode_arm(at + off, rom->read32(src + off));
        std::printf("%08X:  %08X  %s\n", at + off, in.raw, core::arm::disassemble(in).c_str());
        off += in.size;
    }
    return 0;
}

void write_disasm(const recomp::Analyzer& a, const fs::path& file) {
    std::ofstream out(file);
    for (const auto& [addr, f] : a.functions()) {
        out << "\n; ───── " << (f.thumb ? "thumb" : "arm") << " func_" << std::hex << addr << " ("
            << recomp::reason_name(f.reason) << (f.invalid ? ", INVÁLIDA: " + f.invalid_why : "") << ")\n";
        // Lista as instruções em ordem de endereço, marcando inícios de bloco.
        for (uint32_t pc = f.lo; pc < f.hi;) {
            // Overlays (código na RAM) não têm mapa de bytes: vale o conjunto de instruções.
            const auto kind = a.in_rom(pc) ? a.byte_map()[pc - core::kRomBase]
                              : f.instrs.count(pc) ? (f.thumb ? recomp::ByteKind::Thumb : recomp::ByteKind::Arm)
                                                   : recomp::ByteKind::Unknown;
            const bool ours = f.instrs.count(pc) && kind == (f.thumb ? recomp::ByteKind::Thumb : recomp::ByteKind::Arm);
            if (!ours) {
                if (kind == recomp::ByteKind::Literal) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "  %08X:  .word 0x%08X\n", pc, a.decode_at(pc, false).raw);
                    out << buf;
                    pc += 4;
                } else {
                    pc += f.thumb ? 2 : 4;
                }
                continue;
            }
            const auto in = a.decode_at(pc, f.thumb);
            char raw[16];
            if (!in.thumb) std::snprintf(raw, sizeof raw, "%08X ", in.raw);
            else if (in.size == 4) std::snprintf(raw, sizeof raw, "%04X %04X", in.raw & 0xFFFF, in.raw >> 16);
            else std::snprintf(raw, sizeof raw, "%04X     ", in.raw);
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s %08X:  %s  %s\n", f.blocks.count(pc) ? ">" : " ", pc, raw,
                          core::arm::disassemble(in).c_str());
            out << buf;
            pc += in.size;
        }
    }
}

int cmd_analyze(int argc, char** argv, bool emit) {
    const char* rom_path = nullptr;
    fs::path out_dir = "generated/analysis";
    fs::path emit_dir = emit ? "generated/recomp" : "";
    std::string config = "config/functions.txt";
    bool disasm = false, scan = true;
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--config") && i + 1 < argc) config = argv[++i];
        else if (!std::strcmp(argv[i], "--disasm")) disasm = true;
        else if (!std::strcmp(argv[i], "--emit") && i + 1 < argc) emit_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--no-scan")) scan = false;
        else if (!rom_path) rom_path = argv[i];
        else return usage();
    }
    if (!rom_path) return usage();
    auto rom = load_supported(rom_path);
    if (!rom) return 1;

    recomp::Analyzer a(*rom);
    std::string err;
    if (!a.load_config(config, &err)) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    a.add_entry(rom->header().entry_point, false, recomp::EntryReason::Entry);
    if (scan) a.scan_code_pointers();
    a.run();
    if (scan) {
        size_t leaves = 0;
        for (size_t n; (n = a.scan_leaf_pointers()) > 0;) {
            leaves += n;
            a.run();
        }
        std::printf("Funções folha : %zu achadas por ponteiro (2ª passada)\n", leaves);
    }
    // Ponto fixo: remover uma pseudo-função pode liberar outra que só ela referenciava.
    size_t pruned = 0;
    for (size_t n; (n = a.prune_bl_jumps()) > 0;) pruned += n;
    std::printf("bl como salto : %zu pseudo-funções removidas\n", pruned);

    fs::create_directories(out_dir);

    // functions.csv
    size_t n_arm = 0, n_thumb = 0, n_invalid = 0, n_unres = 0, n_veneer = 0;
    std::map<std::string, size_t> by_reason;
    {
        std::ofstream csv(out_dir / "functions.csv");
        csv << "addr,mode,reason,size,instrs,blocks,callees,unresolved,flags\n";
        for (const auto& [addr, f] : a.functions()) {
            (f.thumb ? n_thumb : n_arm)++;
            by_reason[recomp::reason_name(f.reason)]++;
            if (f.invalid) n_invalid++;
            if (f.is_veneer) n_veneer++;
            n_unres += f.unresolved.size();
            char buf[200];
            std::snprintf(buf, sizeof buf, "0x%08X,%s,%s,%u,%u,%zu,%zu,%zu,%s%s\n", addr, f.thumb ? "thumb" : "arm",
                          recomp::reason_name(f.reason), f.hi - f.lo, f.instr_count, f.blocks.size(),
                          f.callees.size(), f.unresolved.size(), f.is_veneer ? "veneer " : "",
                          f.invalid ? ("invalid:" + f.invalid_why).c_str() : "");
            csv << buf;
        }
    }
    // unresolved.txt / external.txt
    {
        std::ofstream u(out_dir / "unresolved.txt");
        for (const auto& [addr, f] : a.functions())
            for (uint32_t pc : f.unresolved) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "%08X  (func_%08X)  %s\n", pc, addr,
                              core::arm::disassemble(a.decode_at(pc, f.thumb)).c_str());
                u << buf;
            }
        std::ofstream e(out_dir / "external.txt");
        for (const auto& x : a.external_targets()) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%08X  <- %08X\n", x.target, x.from);
            e << buf;
        }
    }
    if (disasm) write_disasm(a, out_dir / "disasm.txt");

    // Cobertura
    size_t arm_b = 0, thumb_b = 0, lit_b = 0;
    for (auto k : a.byte_map()) {
        if (k == recomp::ByteKind::Arm) arm_b++;
        else if (k == recomp::ByteKind::Thumb) thumb_b++;
        else if (k == recomp::ByteKind::Literal) lit_b++;
    }
    const double total = static_cast<double>(rom->size());
    std::printf("Funções       : %zu (ARM %zu, Thumb %zu, veneers %zu, inválidas %zu)\n", a.functions().size(), n_arm,
                n_thumb, n_veneer, n_invalid);
    std::printf("Por origem    :");
    for (const auto& [k, v] : by_reason) std::printf(" %s=%zu", k.c_str(), v);
    std::printf("\nCódigo ARM    : %zu bytes (%.2f%%)\n", arm_b, arm_b * 100 / total);
    std::printf("Código Thumb  : %zu bytes (%.2f%%)\n", thumb_b, thumb_b * 100 / total);
    std::printf("Literais      : %zu bytes\n", lit_b);
    std::printf("Saltos indir. : %zu não resolvidos\n", n_unres);
    std::printf("Alvos externos: %zu (fora da ROM, ex.: IWRAM)\n", a.external_targets().size());
    std::printf("Conflitos     : %u bytes com modo divergente\n", a.mode_conflicts());
    std::printf("Relatórios em : %s\n", out_dir.string().c_str());

    if (!emit_dir.empty()) {
        const auto st = recomp::emit_cpp(a, *rom, emit_dir);
        std::printf("C++ gerado    : %zu funções, %zu instruções, %zu arquivos em %s (%zu não suportadas)\n",
                    st.functions, st.instructions, st.files, emit_dir.string().c_str(), st.unimplemented);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    if (!std::strcmp(argv[1], "info")) return cmd_info(argv[2]);
    if (!std::strcmp(argv[1], "analyze")) return cmd_analyze(argc - 2, argv + 2, false);
    if (!std::strcmp(argv[1], "emit")) return cmd_analyze(argc - 2, argv + 2, true);
    if (!std::strcmp(argv[1], "dis")) return cmd_dis(argc - 2, argv + 2);
    return usage();
}
