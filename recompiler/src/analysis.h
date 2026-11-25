#pragma once
// Descoberta de funções por travessia recursiva do código da ROM.
//
// Fontes de entrada:
//   - entry point do cartucho (ARM)
//   - alvos de BL
//   - alvos de BX/LDR PC com valor constante conhecido (literal pool, ADD PC)
//   - ponteiros de código encontrados em literais ou varrendo a ROM (ímpar → Thumb
//     apontando para um prólogo "push {..., lr}")
//
// Saltos indiretos que não conseguimos resolver são registrados; o runtime os
// completará mais tarde (fallback para o interpretador + log).

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/armv4t.h"
#include "core/rom.h"

namespace recomp {

enum class EntryReason : uint8_t { Entry, Call, Jump, Pointer, Scan, Manual };
const char* reason_name(EntryReason r);

struct Function {
    uint32_t addr = 0;
    bool thumb = false;
    EntryReason reason = EntryReason::Call;
    uint32_t lo = 0, hi = 0;          // faixa [lo, hi) coberta pelas instruções
    uint32_t instr_count = 0;
    std::set<uint32_t> blocks;        // inícios de blocos básicos
    std::set<uint32_t> callees;
    std::vector<uint32_t> unresolved; // endereços de saltos indiretos não resolvidos
    std::set<uint32_t> instrs;        // endereços de todas as instruções da função
    std::set<uint32_t> returns;       // instruções classificadas como retorno
    std::map<uint32_t, std::vector<uint32_t>> tables; // salto por tabela → alvos
    // Entradas extras (código encadeado): saltos indiretos para elas viram goto dentro
    // da própria função em vez de chamadas aninhadas.
    std::set<uint32_t> extra_entries;
    bool is_veneer = false;           // stub "bx rN" (_call_via_rN)
    bool invalid = false;             // achou instrução indefinida ou saiu da ROM
    std::string invalid_why;
};

struct ExternalTarget {
    uint32_t target;
    uint32_t from;
};

enum class ByteKind : uint8_t { Unknown, Arm, Thumb, Literal };

// Código da ROM que o jogo copia para a RAM e executa lá (ex.: rotinas ARM na IWRAM).
struct Overlay {
    uint32_t ram;   // endereço de execução
    uint32_t rom;   // origem na ROM
    uint32_t size;
};

class Analyzer {
public:
    explicit Analyzer(const core::Rom& rom);

    void add_entry(uint32_t addr, bool thumb, EntryReason why);
    // Tabela de ponteiros de função na ROM (bit 0 de cada entrada = Thumb).
    void add_pointer_table(uint32_t base, uint32_t count);
    // Endereço que nunca deve virar função (falso positivo da varredura).
    void skip(uint32_t addr) { skipped_.insert(addr & ~1u); }
    // Lê config/functions.txt: "func <addr> arm|thumb", "table <addr> <n>", "skip <addr>",
    // "overlay <ram> <rom> <tamanho>", "entry <função> <endereço>".
    bool load_config(const std::string& path, std::string* error);
    void scan_code_pointers();
    // Depois de run(): aceita ponteiros Thumb para funções folha (sem push {lr}) na região
    // de código, validados por decodificação até um retorno. Devolve quantas achou.
    size_t scan_leaf_pointers();
    void run();

    const std::map<uint32_t, Function>& functions() const { return functions_; }
    const std::vector<ExternalTarget>& external_targets() const { return external_; }
    const std::vector<ByteKind>& byte_map() const { return bytes_; }
    uint32_t mode_conflicts() const { return conflicts_; }

    core::arm::Instr decode_at(uint32_t addr, bool thumb) const;
    bool in_rom(uint32_t addr) const;
    bool in_code(uint32_t addr) const; // ROM ou overlay
    const Overlay* overlay_at(uint32_t addr) const;
    const std::vector<Overlay>& overlays() const { return overlays_; }
    uint32_t code_read32(uint32_t addr) const;
    uint16_t code_read16(uint32_t addr) const;

private:
    void explore(Function& f);
    bool looks_like_thumb_prologue(uint32_t addr) const;
    bool looks_like_leaf(uint32_t addr) const;
    void consider_pointer(uint32_t value, EntryReason why);
    void mark(uint32_t addr, uint32_t size, ByteKind kind);

    const core::Rom& rom_;
    std::map<uint32_t, Function> functions_;
    std::vector<uint32_t> pending_;
    std::vector<ExternalTarget> external_;
    std::vector<ByteKind> bytes_;
    std::set<uint32_t> skipped_;
    std::vector<Overlay> overlays_;
    std::map<uint32_t, std::set<uint32_t>> extra_entries_;
    uint32_t conflicts_ = 0;
};

} // namespace recomp
