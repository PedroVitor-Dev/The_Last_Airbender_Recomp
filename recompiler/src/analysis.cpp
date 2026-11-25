#include "analysis.h"

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace recomp {

using core::arm::Cond;
using core::arm::Instr;
using core::arm::Op;

namespace {

constexpr uint32_t kMaxFunctionInstrs = 200000; // proteção contra travessias descontroladas

bool is_rom_code_ptr(uint32_t v, size_t rom_size) {
    return v >= core::kRomBase && v < core::kRomBase + rom_size;
}

} // namespace

const char* reason_name(EntryReason r) {
    switch (r) {
    case EntryReason::Entry: return "entry";
    case EntryReason::Call: return "call";
    case EntryReason::Jump: return "jump";
    case EntryReason::Pointer: return "ptr";
    case EntryReason::Scan: return "scan";
    default: return "manual";
    }
}

Analyzer::Analyzer(const core::Rom& rom) : rom_(rom), bytes_(rom.size(), ByteKind::Unknown) {}

bool Analyzer::in_rom(uint32_t addr) const {
    return addr >= core::kRomBase && addr < core::kRomBase + rom_.size();
}

const Overlay* Analyzer::overlay_at(uint32_t addr) const {
    for (const auto& o : overlays_)
        if (addr >= o.ram && addr < o.ram + o.size) return &o;
    return nullptr;
}

bool Analyzer::in_code(uint32_t addr) const { return in_rom(addr) || overlay_at(addr) != nullptr; }

uint32_t Analyzer::code_read32(uint32_t addr) const {
    if (const Overlay* o = overlay_at(addr)) return rom_.read32(o->rom + (addr - o->ram));
    return rom_.read32(addr);
}

uint16_t Analyzer::code_read16(uint32_t addr) const {
    if (const Overlay* o = overlay_at(addr)) return rom_.read16(o->rom + (addr - o->ram));
    return rom_.read16(addr);
}

Instr Analyzer::decode_at(uint32_t addr, bool thumb) const {
    if (thumb) {
        uint16_t next = in_code(addr + 2) ? code_read16(addr + 2) : 0;
        return core::arm::decode_thumb(addr, code_read16(addr), next);
    }
    return core::arm::decode_arm(addr, code_read32(addr));
}

void Analyzer::add_entry(uint32_t addr, bool thumb, EntryReason why) {
    addr &= thumb ? ~1u : ~3u;
    if (!in_code(addr) || skipped_.count(addr)) return;
    auto [it, inserted] = functions_.try_emplace(addr);
    if (!inserted) return;
    it->second.addr = addr;
    it->second.thumb = thumb;
    it->second.reason = why;
    pending_.push_back(addr);
}

void Analyzer::mark(uint32_t addr, uint32_t size, ByteKind kind) {
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t off = addr + i - core::kRomBase;
        if (off >= bytes_.size()) return;
        ByteKind& b = bytes_[off];
        if (b == ByteKind::Unknown) b = kind;
        else if (b != kind) { ++conflicts_; return; }
    }
}

bool Analyzer::looks_like_thumb_prologue(uint32_t addr) const {
    if (!in_rom(addr)) return false;
    return (rom_.read16(addr) & 0xFF00) == 0xB500; // push {..., lr}
}

void Analyzer::consider_pointer(uint32_t value, EntryReason why) {
    // Só ponteiros Thumb (bit 0 = 1) para um "push {..., lr}" — critério conservador.
    if (!(value & 1) || !is_rom_code_ptr(value, rom_.size())) return;
    if (looks_like_thumb_prologue(value & ~1u)) add_entry(value, true, why);
}

bool Analyzer::looks_like_leaf(uint32_t addr) const {
    // Decodifica linearmente até achar um retorno; rejeita dados e instruções inválidas.
    for (int i = 0; i < 64; ++i) {
        const uint32_t pc = addr + i * 2;
        if (!in_rom(pc + 1)) return false;
        const ByteKind k = bytes_[pc - core::kRomBase];
        if (k == ByteKind::Literal || k == ByteKind::Arm) return false;
        if (rom_.read16(pc) == 0) return false;
        const Instr in = decode_at(pc, true);
        if (in.op == Op::UNDEFINED || in.op == Op::COPROC || in.op == Op::THUMB_BL_LO) return false;
        if (in.op == Op::BX) return true;
        if (in.op == Op::LDM && (in.reg_list & 0x8000)) return true;
        if (in.op == Op::MOV && in.rd == 15) return true;
        if (in.op == Op::B && in.cond == Cond::AL) return true;
        if (in.op == Op::BL) ++i; // ocupa 4 bytes
    }
    return false;
}

// Epílogo compartilhado alcançado por "bl" usado como salto longo (o b do Thumb só
// alcança ±2 KB): só movimenta registradores, desempilha e retorna — sem push, sem
// gravações em memória e sem chamadas.
bool Analyzer::is_shared_epilogue(uint32_t addr) const {
    bool popped = false;
    for (int i = 0; i < 16; ++i) {
        const uint32_t pc = addr + i * 2;
        if (!in_code(pc)) return false;
        const Instr in = decode_at(pc, true);
        if (in.op == Op::LDM && in.rn == 13 && in.writeback && in.up) {
            if (in.reg_list & 0x8000) return true; // pop {..., pc}
            popped = true;
            continue;
        }
        if (in.op == Op::BX) return popped;
        if (in.is_data_processing() && !in.is_compare() && in.rd != 15) continue;
        return false;
    }
    return false;
}

size_t Analyzer::prune_bl_jumps() {
    // Para cada alvo de BL: todas as referências vêm de dentro de uma função que já
    // contém o alvo no próprio corpo?
    std::map<uint32_t, bool> only_internal; // alvo → só referências internas até agora
    for (const auto& [addr, f] : functions_) {
        for (uint32_t pc : f.instrs) {
            const Instr in = decode_at(pc, f.thumb);
            if (in.op != Op::BL) continue;
            if (in.target == f.addr) continue; // autorreferência da própria pseudo-função
            const bool internal = f.instrs.count(in.target) != 0;
            auto [it, inserted] = only_internal.try_emplace(in.target, internal);
            if (!inserted) it->second = it->second && internal;
        }
    }
    size_t removed = 0;
    for (const auto& [target, internal] : only_internal) {
        auto it = functions_.find(target);
        if (!internal || it == functions_.end() || it->second.reason != EntryReason::Call) continue;
        functions_.erase(it);
        ++removed;
    }
    return removed;
}

size_t Analyzer::scan_leaf_pointers() {
    // Região de código: até o fim da última função já descoberta na ROM.
    uint32_t code_end = core::kRomBase;
    for (const auto& [addr, f] : functions_)
        if (in_rom(addr) && !f.invalid && f.hi > code_end) code_end = f.hi;
    size_t added = 0;
    for (uint32_t off = 0; off + 4 <= rom_.size(); off += 4) {
        const uint32_t v = rom_.read32(core::kRomBase + off);
        if (!(v & 1) || v < core::kRomBase || v >= code_end) continue;
        const uint32_t a = v & ~1u;
        if (functions_.count(a) || skipped_.count(a)) continue;
        // Já é código de outra função (meio de função): não é entrada.
        if (bytes_[a - core::kRomBase] == ByteKind::Thumb) continue;
        if (!looks_like_leaf(a)) continue;
        add_entry(v, true, EntryReason::Scan);
        ++added;
    }
    return added;
}

void Analyzer::add_pointer_table(uint32_t base, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t a = base + i * 4;
        if (!in_rom(a)) return;
        mark(a, 4, ByteKind::Literal);
        const uint32_t v = rom_.read32(a);
        add_entry(v, v & 1, EntryReason::Manual);
    }
}

bool Analyzer::load_config(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) return true; // arquivo opcional
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (n == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3); // BOM UTF-8
        if (auto c = line.find('#'); c != std::string::npos) line.resize(c);
        std::istringstream ss(line);
        std::string kind, a, b;
        if (!(ss >> kind)) continue;
        ss >> a >> b;
        auto num = [](const std::string& s) { return static_cast<uint32_t>(std::stoul(s, nullptr, 0)); };
        try {
            if (kind == "func" && (b == "arm" || b == "thumb")) add_entry(num(a), b == "thumb", EntryReason::Manual);
            else if (kind == "table" && !b.empty()) add_pointer_table(num(a), num(b));
            else if (kind == "skip" && !a.empty()) skip(num(a));
            else if (kind == "entry" && !b.empty()) extra_entries_[num(a) & ~1u].insert(num(b) & ~1u);
            else if (kind == "overlay" && !b.empty()) {
                std::string size;
                if (!(ss >> size)) throw std::invalid_argument(kind);
                overlays_.push_back({num(a), num(b), num(size)});
            }
            else throw std::invalid_argument(kind);
        } catch (const std::exception&) {
            if (error) *error = path + ":" + std::to_string(n) + ": linha inválida";
            return false;
        }
    }
    return true;
}

void Analyzer::scan_code_pointers() {
    for (uint32_t off = 0; off + 4 <= rom_.size(); off += 4) {
        const uint32_t v = rom_.read32(core::kRomBase + off);
        consider_pointer(v, EntryReason::Scan);
    }
}

void Analyzer::run() {
    while (!pending_.empty()) {
        uint32_t addr = pending_.back();
        pending_.pop_back();
        explore(functions_.at(addr));
    }
}

namespace {

// O que sabemos sobre cada registrador ao longo de um bloco.
struct RegState {
    enum class Tag : uint8_t { None, Popped, Times4, Index4, TablePtr, TableEntry };
    std::optional<uint32_t> value;
    Tag tag = Tag::None;
    uint32_t count = 0; // nº de entradas da tabela (Index4/TablePtr/TableEntry)
    uint32_t base = 0;  // endereço da tabela (TablePtr/TableEntry)
    uint8_t src = 0;    // Times4: registrador multiplicado por 4 (ainda sem limite)
};

struct State {
    std::array<RegState, 16> r{};
    int cmp_reg = -1;   // último "cmp rX, #N" — limite de switch
    uint32_t cmp_n = 0;
};

constexpr uint32_t kIrqVector = 0x03007FFC;
constexpr uint32_t kMaxTableEntries = 1024;
constexpr uint32_t kMaxTableDistance = 0x10000; // alvos de switch ficam perto da função

bool plain_reg(const core::arm::Operand& o) {
    return !o.is_imm && !o.shift_by_reg && o.shift == core::arm::ShiftType::LSL && o.shift_amount == 0;
}

} // namespace

void Analyzer::explore(Function& f) {
    using Tag = RegState::Tag;

    std::vector<std::pair<uint32_t, State>> work{{f.addr, State{}}};
    if (auto ex = extra_entries_.find(f.addr); ex != extra_entries_.end()) {
        f.extra_entries = ex->second;
        for (uint32_t e : ex->second) work.push_back({e, State{}});
    }
    std::set<uint32_t> visited;
    f.lo = f.hi = f.addr;

    // Lê uma tabela de salto; retorna false (sem efeitos) se algum alvo for implausível.
    auto resolve_table = [&](uint32_t at, uint32_t base, uint32_t count, const State& st) {
        if (count == 0 || count > kMaxTableEntries || !in_rom(base) || !in_rom(base + count * 4 - 1)) return false;
        std::vector<uint32_t> targets;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t t = rom_.read32(base + i * 4) & (f.thumb ? ~1u : ~3u);
            const uint32_t dist = t > f.addr ? t - f.addr : f.addr - t;
            if (!in_rom(t) || dist > kMaxTableDistance) return false;
            targets.push_back(t);
        }
        mark(base, count * 4, ByteKind::Literal);
        f.tables[at] = targets;
        State clean;
        clean.r = st.r;
        for (uint32_t t : targets) work.push_back({t, clean});
        return true;
    };

    while (!work.empty()) {
        auto [pc, st] = work.back();
        work.pop_back();
        if (visited.count(pc)) continue;
        f.blocks.insert(pc);

        bool prev_sets_lr = false; // "mov lr, pc" (ARM) antes de um BX ⇒ chamada
        for (;;) {
            if (!in_code(pc)) {
                f.invalid = true;
                f.invalid_why = "saiu da ROM";
                break;
            }
            // Pool de literais já identificado: chegamos aqui por fall-through depois de uma
            // chamada que não retorna (ex.: abort). Não decodificar dados como código.
            if (in_rom(pc) && bytes_[pc - core::kRomBase] == ByteKind::Literal) break;
            if (!visited.insert(pc).second) break; // fluxo convergiu num bloco já visto
            if (++f.instr_count > kMaxFunctionInstrs) {
                f.invalid = true;
                f.invalid_why = "função grande demais";
                return;
            }

            // 0x0000 ("lsls r0, r0, #0") nunca é emitido pelo compilador: é padding depois
            // de uma chamada que não retorna.
            if (f.thumb && code_read16(pc) == 0) break;

            const Instr in = decode_at(pc, f.thumb);
            if (in.op == Op::UNDEFINED || in.op == Op::COPROC) {
                f.invalid = true;
                f.invalid_why = "instrução indefinida";
                break;
            }
            mark(pc, in.size, f.thumb ? ByteKind::Thumb : ByteKind::Arm);
            f.instrs.insert(pc);
            if (pc < f.lo) f.lo = pc;
            if (pc + in.size > f.hi) f.hi = pc + in.size;

            const bool always = in.cond == Cond::AL;
            const uint32_t next = pc + in.size;
            bool ends_block = false;
            auto& R = st.r;

            // ── efeito no registrador de destino ──
            RegState out;
            bool rd_written = false;
            if (in.op == Op::LDR && in.rn == 15 && in.op2.is_imm && in.pre_index && !in.writeback) {
                const uint32_t ea = in.up ? in.pc_value + in.op2.imm : in.pc_value - in.op2.imm;
                if (in_code(ea)) {
                    mark(ea, 4, ByteKind::Literal);
                    out.value = code_read32(ea);
                    consider_pointer(*out.value, EntryReason::Pointer);
                }
                rd_written = true;
            } else if (in.op == Op::LDR && in.rn != 15 && in.pre_index && !in.writeback) {
                rd_written = true;
                const RegState& b = R[in.rn];
                if (in.op2.is_imm && in.op2.imm == 0 && b.tag == Tag::TablePtr) {
                    out.tag = Tag::TableEntry, out.count = b.count, out.base = b.base;
                } else if (plain_reg(in.op2)) {
                    const RegState& o = R[in.op2.rm];
                    if (b.tag == Tag::Index4 && o.value) out.tag = Tag::TableEntry, out.count = b.count, out.base = *o.value;
                    if (o.tag == Tag::Index4 && b.value) out.tag = Tag::TableEntry, out.count = o.count, out.base = *b.value;
                }
            } else if (in.is_data_processing() && !in.is_compare()) {
                rd_written = true;
                const auto& o2 = in.op2;
                if (in.op == Op::MOV && o2.is_imm) {
                    out.value = o2.imm;
                } else if (in.op == Op::MOV && plain_reg(o2)) {
                    out = o2.rm == 15 ? RegState{in.pc_value} : R[o2.rm];
                } else if (in.op == Op::MOV && !o2.is_imm && !o2.shift_by_reg && o2.shift == core::arm::ShiftType::LSL &&
                           o2.shift_amount == 2) {
                    if (o2.rm == st.cmp_reg) out.tag = Tag::Index4, out.count = st.cmp_n + 1;
                    else out.tag = Tag::Times4, out.src = o2.rm;
                } else if (in.op == Op::ADD && in.rn == 15 && o2.is_imm) {
                    out.value = in.pc_value + o2.imm;
                } else if (in.op == Op::ADD && o2.is_imm && R[in.rn].value) {
                    out.value = *R[in.rn].value + o2.imm;
                } else if (in.op == Op::ORR && o2.is_imm && R[in.rn].value) {
                    out.value = *R[in.rn].value | o2.imm;
                } else if (in.op == Op::ADD && plain_reg(o2)) {
                    const RegState &a = R[in.rn], &b = R[o2.rm];
                    if (a.tag == Tag::Index4 && b.value) out.tag = Tag::TablePtr, out.count = a.count, out.base = *b.value;
                    if (b.tag == Tag::Index4 && a.value) out.tag = Tag::TablePtr, out.count = b.count, out.base = *a.value;
                }
            } else if (in.op == Op::LDR || in.op == Op::LDRB || in.op == Op::LDRH || in.op == Op::LDRSB ||
                       in.op == Op::LDRSH || in.op == Op::MUL || in.op == Op::MLA || in.op == Op::MRS ||
                       in.op == Op::SWP || in.op == Op::SWPB) {
                rd_written = true;
            }

            // ── fluxo de controle ──
            switch (in.op) {
            case Op::B:
                if (functions_.count(in.target) && in.target != f.addr && always) {
                    f.callees.insert(in.target); // chamada de cauda para função conhecida
                } else {
                    work.push_back({in.target, st});
                }
                ends_block = always;
                break;

            case Op::BL: {
                if (f.thumb && in.cond == Cond::AL && is_shared_epilogue(in.target)) {
                    // "bl" como salto longo para o epílogo: faz parte desta função.
                    work.push_back({in.target, st});
                    break;
                }
                add_entry(in.target, f.thumb, EntryReason::Call);
                f.callees.insert(in.target);
                // Veneer "_call_via_rN": o alvo é só "bx rN"; resolvemos com o rN do chamador.
                const Instr first = decode_at(in.target, f.thumb);
                if (first.op == Op::BX && first.rm < 15 && R[first.rm].value) {
                    const uint32_t t = *R[first.rm].value;
                    if (in_code(t)) {
                        add_entry(t, t & 1, EntryReason::Pointer);
                        f.callees.insert(t & ~1u);
                    } else {
                        external_.push_back({t, pc});
                    }
                }
                for (int r : {0, 1, 2, 3, 12, 14}) R[r] = {};
                break;
            }

            case Op::BX: {
                const RegState& t = R[in.rm];
                if (in.rm == 14 || t.tag == Tag::Popped) {
                    ends_block = always; // retorno (bx lr / pop {rN}; bx rN)
                    f.returns.insert(pc);
                } else if (in.rm < 15 && t.value) {
                    const uint32_t v = *t.value;
                    if (in_code(v)) {
                        add_entry(v, v & 1, prev_sets_lr ? EntryReason::Call : EntryReason::Jump);
                        f.callees.insert(v & ~1u);
                    } else {
                        external_.push_back({v, pc});
                    }
                    ends_block = always && !prev_sets_lr;
                    if (prev_sets_lr) for (int r : {0, 1, 2, 3, 12}) R[r] = {};
                } else {
                    if (pc == f.addr && always) f.is_veneer = true;
                    else f.unresolved.push_back(pc);
                    ends_block = always && !prev_sets_lr;
                }
                break;
            }

            case Op::LDM:
                if (in.reg_list & 0x8000) {
                    ends_block = always; // retorno (pop {..., pc})
                    f.returns.insert(pc);
                }
                break;

            case Op::STR:
                // O crt0 instala o handler de IRQ gravando seu endereço em 0x03007FFC.
                if (in.pre_index && in.op2.is_imm && R[in.rn].value && R[in.rd].value) {
                    const uint32_t ea = in.up ? *R[in.rn].value + in.op2.imm : *R[in.rn].value - in.op2.imm;
                    const uint32_t v = *R[in.rd].value;
                    if (ea == kIrqVector && in_code(v)) add_entry(v, v & 1, EntryReason::Pointer);
                }
                break;

            case Op::LDR:
                if (in.rd == 15) {
                    if (out.value) {
                        if (in_code(*out.value)) add_entry(*out.value, false, EntryReason::Jump);
                        else external_.push_back({*out.value, pc});
                    } else if (!f.thumb && in.rn == 15 && in.pre_index && !in.op2.is_imm && in.op2.rm == st.cmp_reg &&
                               in.op2.shift == core::arm::ShiftType::LSL && in.op2.shift_amount == 2) {
                        // ARM: ldr pc, [pc, rX, lsl #2] — tabela logo após a instrução seguinte
                        if (!resolve_table(pc, in.pc_value, st.cmp_n + 1, st)) f.unresolved.push_back(pc);
                    } else {
                        f.unresolved.push_back(pc);
                    }
                    ends_block = always;
                }
                break;

            case Op::THUMB_BL_LO: // chamada para LR montado à mão
                f.unresolved.push_back(pc);
                break;

            default:
                if (in.is_data_processing() && !in.is_compare() && in.rd == 15) {
                    const bool ret = in.op == Op::MOV && plain_reg(in.op2) &&
                                     (in.op2.rm == 14 || R[in.op2.rm].tag == Tag::Popped);
                    if (ret) f.returns.insert(pc);
                    if (!ret) {
                        const RegState* src = in.op == Op::MOV && plain_reg(in.op2) ? &R[in.op2.rm] : nullptr;
                        if (src && src->tag == Tag::TableEntry) {
                            if (!resolve_table(pc, src->base, src->count, st)) f.unresolved.push_back(pc);
                        } else if (out.value && in_code(*out.value)) {
                            add_entry(*out.value, f.thumb, EntryReason::Jump);
                        } else {
                            f.unresolved.push_back(pc);
                        }
                    }
                    ends_block = always;
                }
                break;
            }

            prev_sets_lr = !f.thumb && in.op == Op::MOV && in.rd == 14 && plain_reg(in.op2) && in.op2.rm == 15;

            // ── atualização do estado ──
            if (in.op == Op::CMP && in.op2.is_imm) {
                st.cmp_reg = in.rn;
                st.cmp_n = in.op2.imm;
                // Índice já multiplicado antes do cmp: agora sabemos o limite.
                for (auto& r : R)
                    if (r.tag == Tag::Times4 && r.src == in.rn) r.tag = Tag::Index4, r.count = in.op2.imm + 1;
            }
            if (rd_written && in.rd != 15) {
                R[in.rd] = always ? out : RegState{};
                if (in.rd == st.cmp_reg) st.cmp_reg = -1;
                for (int r = 0; r < 16; ++r)
                    if (r != in.rd && R[r].tag == Tag::Times4 && R[r].src == in.rd) R[r] = {};
            }
            if (in.op == Op::LDM) {
                const bool pop = in.rn == 13 && in.writeback && in.up;
                for (int r = 0; r < 15; ++r)
                    if ((in.reg_list >> r) & 1) {
                        R[r] = {};
                        if (pop) R[r].tag = Tag::Popped;
                        if (r == st.cmp_reg) st.cmp_reg = -1;
                    }
            }
            if (in.writeback && in.rn < 15) R[in.rn] = {};
            if (in.op >= Op::UMULL && in.op <= Op::SMLAL) R[in.rd] = {}, R[in.rn] = {};

            if (ends_block) break;
            if (in.op == Op::B || (in.writes_pc() && !always)) {
                work.push_back({next, st}); // fall-through de desvio condicional
                break;
            }
            pc = next;
        }
    }
}

} // namespace recomp
