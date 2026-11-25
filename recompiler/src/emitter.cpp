#include "emitter.h"

#include <bit>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <set>
#include <vector>

namespace recomp {

using core::arm::Cond;
using core::arm::Instr;
using core::arm::Op;
using core::arm::Operand;
using core::arm::ShiftType;

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string hex(uint32_t v) { return fmt("0x%08Xu", v); }
std::string fn_name(uint32_t addr) { return fmt("f_%08X", addr); }
std::string label(uint32_t addr) { return fmt("L_%08X", addr); }

const char* cond_expr(Cond c) {
    switch (c) {
    case Cond::EQ: return "c.z_";
    case Cond::NE: return "!c.z_";
    case Cond::CS: return "c.c_";
    case Cond::CC: return "!c.c_";
    case Cond::MI: return "c.n_";
    case Cond::PL: return "!c.n_";
    case Cond::VS: return "c.v_";
    case Cond::VC: return "!c.v_";
    case Cond::HI: return "(c.c_ && !c.z_)";
    case Cond::LS: return "(!c.c_ || c.z_)";
    case Cond::GE: return "(c.n_ == c.v_)";
    case Cond::LT: return "(c.n_ != c.v_)";
    case Cond::GT: return "(!c.z_ && c.n_ == c.v_)";
    case Cond::LE: return "(c.z_ || c.n_ != c.v_)";
    default: return "true";
    }
}

bool is_load(Op op) {
    return op == Op::LDR || op == Op::LDRB || op == Op::LDRH || op == Op::LDRSB || op == Op::LDRSH;
}

class FunctionEmitter {
public:
    FunctionEmitter(const Analyzer& a, const core::Rom& rom, const Function& f, std::string& out, EmitStats& st)
        : a_(a), rom_(rom), f_(f), o_(out), st_(st) {}

    void emit() {
        collect_labels();
        line("void %s(gba::Gba& g) {", fn_name(f_.addr).c_str());
        line("    auto& c = g.cpu;");
        line("    uint32_t* r = c.r.data();");
        line("    const uint32_t entry_lr = r[14];");
        line("    (void)entry_lr;");

        std::vector<uint32_t> order(f_.instrs.begin(), f_.instrs.end());
        for (size_t i = 0; i < order.size(); ++i) {
            const uint32_t pc = order[i];
            const Instr in = a_.decode_at(pc, f_.thumb);
            if (labels_.count(pc)) {
                line("%s:", label(pc).c_str());
                line("    g.cycles += %u;", segment_cost(order, i));
                line("    if (g.cycles >= g.next_event) g.sync_at(%s);", hex(pc).c_str());
            }
            line("    // %08X  %s", pc, core::arm::disassemble(in).c_str());
            const bool falls = emit_instr(in);
            ++st_.instructions;

            const uint32_t next = pc + in.size;
            const bool contiguous = i + 1 < order.size() && order[i + 1] == next;
            if (falls && !contiguous) {
                if (f_.instrs.count(next)) {
                    line("    goto %s;", label(next).c_str());
                } else {
                    // Fluxo segue para código que a análise não percorreu.
                    line("    c.t_ = %s; r[15] = %s; g.resume_until(entry_lr & ~1u); return;",
                         f_.thumb ? "true" : "false", hex(next).c_str());
                }
            }
        }
        line("}");
        line("");
    }

private:
    void line(const char* f, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, f);
        std::vsnprintf(buf, sizeof buf, f, ap);
        va_end(ap);
        o_ += buf;
        o_ += '\n';
    }

    void collect_labels() {
        labels_.insert(f_.addr);
        for (uint32_t b : f_.blocks)
            if (f_.instrs.count(b)) labels_.insert(b);
        for (uint32_t pc : f_.instrs) {
            const Instr in = a_.decode_at(pc, f_.thumb);
            if ((in.op == Op::B || in.op == Op::BL) && f_.instrs.count(in.target)) labels_.insert(in.target);
            const uint32_t next = pc + in.size;
            if (f_.instrs.count(next) && (in.op == Op::B || in.writes_pc() || in.op == Op::BL)) labels_.insert(next);
        }
        for (const auto& [at, targets] : f_.tables)
            for (uint32_t t : targets)
                if (f_.instrs.count(t)) labels_.insert(t);
        for (uint32_t e : f_.extra_entries)
            if (f_.instrs.count(e)) labels_.insert(e);
        // Qualquer instrução cuja anterior não flui para ela precisa de rótulo (alvo de goto).
        uint32_t prev_end = 0;
        for (uint32_t pc : f_.instrs) {
            if (pc != prev_end) labels_.insert(pc);
            prev_end = pc + a_.decode_at(pc, f_.thumb).size;
        }
    }

    // Custo estático (ciclos) das instruções até o próximo rótulo.
    uint32_t segment_cost(const std::vector<uint32_t>& order, size_t i) const {
        uint32_t total = 0;
        for (size_t k = i; k < order.size(); ++k) {
            if (k > i && labels_.count(order[k])) break;
            total += instr_cost(a_.decode_at(order[k], f_.thumb));
        }
        return total;
    }

    uint32_t instr_cost(const Instr& in) const {
        uint32_t c = f_.thumb ? 3 : 6; // busca na ROM
        switch (in.op) {
        case Op::LDR: case Op::LDRB: case Op::LDRH: case Op::LDRSB: case Op::LDRSH: c += 4; break;
        case Op::STR: case Op::STRB: case Op::STRH: c += 3; break;
        case Op::LDM: case Op::STM: c += 3 * static_cast<uint32_t>(std::popcount(in.reg_list)); break;
        case Op::MUL: case Op::MLA: c += 3; break;
        case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL: c += 4; break;
        case Op::SWP: case Op::SWPB: c += 4; break;
        case Op::B: case Op::BL: case Op::BX: case Op::SWI: c += 2; break;
        default: break;
        }
        return c;
    }

    std::string R(uint8_t n, const Instr& in) const { return n == 15 ? hex(in.pc_value) : fmt("r[%u]", n); }

    // Operando 2 → expressão. `carry` recebe o código que atualiza `sc` (carry do shifter),
    // vazio quando o shifter não altera o carry.
    std::string operand(const Instr& in, const Operand& o, std::string& carry, std::string& pre) const {
        carry.clear();
        if (o.is_imm) {
            if (o.imm_rotate) carry = (o.imm >> 31) ? "sc = true;" : "sc = false;";
            return hex(o.imm);
        }
        const std::string m = R(o.rm, in);
        if (o.shift_by_reg) {
            const char* fn = o.shift == ShiftType::LSL ? "lsl_r" : o.shift == ShiftType::LSR ? "lsr_r"
                           : o.shift == ShiftType::ASR ? "asr_r" : "ror_r";
            // PC lido por shift-por-registrador vale addr+12 (pc_value já reflete isso).
            pre = fmt("uint32_t sh = rt::%s(%s, r[%u] & 0xFFu, sc);", fn, m.c_str(), o.rs);
            carry = "/*sc já atualizado*/";
            return "sh";
        }
        const uint32_t n = o.shift_amount;
        switch (o.shift) {
        case ShiftType::LSL:
            if (n == 0) return m;
            carry = fmt("sc = (%s >> %u) & 1u;", m.c_str(), 32 - n);
            return fmt("(%s << %u)", m.c_str(), n);
        case ShiftType::LSR:
            if (n == 32) { carry = fmt("sc = (%s >> 31) != 0;", m.c_str()); return "0u"; }
            carry = fmt("sc = (%s >> %u) & 1u;", m.c_str(), n - 1);
            return fmt("(%s >> %u)", m.c_str(), n);
        case ShiftType::ASR:
            if (n == 32) {
                carry = fmt("sc = (%s >> 31) != 0;", m.c_str());
                return fmt("(uint32_t)((int32_t)%s >> 31)", m.c_str());
            }
            carry = fmt("sc = (%s >> %u) & 1u;", m.c_str(), n - 1);
            return fmt("(uint32_t)((int32_t)%s >> %u)", m.c_str(), n);
        case ShiftType::ROR:
            carry = fmt("sc = (%s >> %u) & 1u;", m.c_str(), n - 1);
            return fmt("std::rotr(%s, %u)", m.c_str(), n);
        default: // RRX
            carry = fmt("sc = %s & 1u;", m.c_str());
            return fmt("(((uint32_t)c.c_ << 31) | (%s >> 1))", m.c_str());
        }
    }

    // Escrita no PC: retorno, tabela de salto ou salto indireto. `mask_mode`: o valor
    // decide o modo (BX); caso contrário o modo atual é mantido.
    void pc_write(const Instr& in, const std::string& val, bool interworking) {
        const uint32_t pc = in.addr;
        const char* tbit = f_.thumb ? "1u" : "0u";
        if (f_.returns.count(pc)) {
            if (interworking)
                line("        { uint32_t t = %s; c.t_ = t & 1u; r[15] = t & (c.t_ ? ~1u : ~3u); return; }", val.c_str());
            else
                line("        r[15] = (%s) & %s; return;", val.c_str(), f_.thumb ? "~1u" : "~3u");
            return;
        }
        auto tab = f_.tables.find(pc);
        if (tab != f_.tables.end()) {
            line("        switch ((%s) & %s) {", val.c_str(), f_.thumb ? "~1u" : "~3u");
            std::set<uint32_t> done;
            for (uint32_t t : tab->second) {
                if (!done.insert(t).second || !f_.instrs.count(t)) continue;
                line("        case %s: goto %s;", hex(t).c_str(), label(t).c_str());
            }
            line("        default: g.jump_indirect(((%s) & ~1u) | %s); return;", val.c_str(), tbit);
            line("        }");
            return;
        }
        if (!f_.extra_entries.empty()) {
            // Código encadeado: alvos que são entradas desta função continuam aqui mesmo.
            line("        switch ((%s) & ~1u) {", val.c_str());
            line("        case %s: goto %s;", hex(f_.addr).c_str(), label(f_.addr).c_str());
            for (uint32_t e : f_.extra_entries)
                if (f_.instrs.count(e)) line("        case %s: goto %s;", hex(e).c_str(), label(e).c_str());
            line("        default: break;");
            line("        }");
        }
        if (interworking) line("        g.jump_indirect(%s); return;", val.c_str());
        else line("        g.jump_indirect(((%s) & ~1u) | %s); return;", val.c_str(), tbit);
    }

    void call_target(uint32_t target, bool thumb) {
        auto it = a_.functions().find(target);
        if (it != a_.functions().end() && it->second.thumb == thumb && !it->second.invalid)
            line("        %s(g);", fn_name(target).c_str());
        else
            line("        g.call_function(%s, %s);", hex(target).c_str(), thumb ? "true" : "false");
    }

    // Emite uma instrução. Devolve true se o fluxo pode seguir para a próxima.
    bool emit_instr(const Instr& in) {
        const bool cond = in.cond != Cond::AL;
        if (cond) line("    if (%s) {", cond_expr(in.cond));
        else line("    {");
        const bool falls = emit_body(in) || cond;
        line("    }");
        return falls;
    }

    bool emit_body(const Instr& in) {
        const uint32_t next = in.addr + in.size;
        switch (in.op) {
        case Op::AND: case Op::EOR: case Op::SUB: case Op::RSB: case Op::ADD: case Op::ADC:
        case Op::SBC: case Op::RSC: case Op::TST: case Op::TEQ: case Op::CMP: case Op::CMN:
        case Op::ORR: case Op::MOV: case Op::BIC: case Op::MVN:
            return emit_dp(in);
        case Op::MUL: case Op::MLA:
            line("        uint32_t res = r[%u] * r[%u]%s;", in.rm, in.rs, in.op == Op::MLA ? fmt(" + r[%u]", in.rn).c_str() : "");
            line("        r[%u] = res;", in.rd);
            if (in.set_flags) line("        rt::nz(c, res);");
            return true;
        case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL: {
            const bool sgn = in.op == Op::SMULL || in.op == Op::SMLAL;
            const bool acc = in.op == Op::UMLAL || in.op == Op::SMLAL;
            if (sgn) line("        uint64_t p = (uint64_t)((int64_t)(int32_t)r[%u] * (int64_t)(int32_t)r[%u]);", in.rm, in.rs);
            else line("        uint64_t p = (uint64_t)r[%u] * (uint64_t)r[%u];", in.rm, in.rs);
            if (acc) line("        p += ((uint64_t)r[%u] << 32) | r[%u];", in.rn, in.rd);
            line("        r[%u] = (uint32_t)p; r[%u] = (uint32_t)(p >> 32);", in.rd, in.rn);
            if (in.set_flags) line("        c.n_ = (p >> 63) & 1; c.z_ = p == 0;");
            return true;
        }
        case Op::LDR: case Op::STR: case Op::LDRB: case Op::STRB:
        case Op::LDRH: case Op::STRH: case Op::LDRSB: case Op::LDRSH:
            return emit_mem(in);
        case Op::LDM: case Op::STM:
            return emit_block(in);
        case Op::SWP: case Op::SWPB:
            line("        uint32_t a = r[%u];", in.rn);
            if (in.op == Op::SWP) line("        uint32_t old = rt::ldr(g, a); rt::wr32(g, a & ~3u, r[%u]);", in.rm);
            else line("        uint32_t old = rt::rd8(g, a); rt::wr8(g, a, r[%u]);", in.rm);
            line("        r[%u] = old;", in.rd);
            return true;
        case Op::B:
            if (f_.instrs.count(in.target)) {
                line("        goto %s;", label(in.target).c_str());
            } else {
                call_target(in.target, f_.thumb); // chamada de cauda
                line("        return;");
            }
            return false;
        case Op::BL: {
            line("        r[14] = %s;", hex(next | (f_.thumb ? 1u : 0u)).c_str());
            // "bl" para código desta mesma função (fora a entrada: recursão) é salto longo.
            if (f_.instrs.count(in.target) && in.target != f_.addr && !f_.extra_entries.count(in.target)) {
                line("        goto %s; // bl usado como salto longo", label(in.target).c_str());
                return false;
            }
            call_target(in.target, f_.thumb);
            line("        if (r[15] != %s) return; // desempilhou além de nós: quem chamou confere", hex(next).c_str());
            return true;
        }
        case Op::BX: {
            const std::string v = R(in.rm, in);
            // ARM: "mov lr, pc; bx rN" é uma chamada indireta.
            if (!f_.thumb && !f_.returns.count(in.addr)) {
                const Instr prev = a_.decode_at(in.addr - 4, false);
                if (f_.instrs.count(in.addr - 4) && prev.op == Op::MOV && prev.rd == 14 && !prev.op2.is_imm &&
                    prev.op2.rm == 15 && prev.op2.shift_amount == 0 && !prev.op2.shift_by_reg) {
                    line("        g.jump_indirect(%s);", v.c_str());
                    line("        if (r[15] != %s) return; // desempilhou além de nós: quem chamou confere", hex(next).c_str());
                    return true;
                }
            }
            pc_write(in, v, true);
            return false;
        }
        case Op::THUMB_BL_HI:
            line("        r[14] = %s;", hex(in.pc_value + in.op2.imm).c_str());
            return true;
        case Op::THUMB_BL_LO:
            line("        uint32_t t = r[14] + %s;", hex(in.op2.imm).c_str());
            line("        r[14] = %s;", hex(next | 1u).c_str());
            line("        g.call_function(t, true);");
            line("        if (r[15] != %s) return; // desempilhou além de nós: quem chamou confere", hex(next).c_str());
            return true;
        case Op::SWI:
            line("        g.swi(%s, %s);", hex(in.thumb ? in.swi_comment : in.swi_comment >> 16).c_str(),
                 hex(next).c_str());
            return true;
        case Op::MRS:
            line("        r[%u] = %s;", in.rd, in.spsr ? "c.spsr_" : "c.cpsr()");
            return true;
        case Op::MSR: {
            std::string carry, pre;
            const std::string v = operand(in, in.op2, carry, pre);
            line("        rt::msr(c, %s, %u, %s);", in.spsr ? "true" : "false", in.psr_mask, v.c_str());
            return true;
        }
        default:
            ++st_.unimplemented;
            line("        g.unimplemented(%s, \"%s\");", hex(in.addr).c_str(), core::arm::op_name(in.op));
            return false;
        }
    }

    bool emit_dp(const Instr& in) {
        std::string carry, pre;
        const bool logical = in.op == Op::AND || in.op == Op::EOR || in.op == Op::TST || in.op == Op::TEQ ||
                             in.op == Op::ORR || in.op == Op::MOV || in.op == Op::BIC || in.op == Op::MVN;
        const bool need_sc = (in.set_flags && logical) || in.op2.shift_by_reg;
        if (need_sc) line("        bool sc = c.c_;");
        const std::string b_expr = operand(in, in.op2, carry, pre);
        if (!pre.empty()) line("        %s", pre.c_str());
        if (in.op != Op::MOV && in.op != Op::MVN) line("        uint32_t a = %s;", R(in.rn, in).c_str());
        line("        uint32_t b = %s;", b_expr.c_str());
        if (in.set_flags && logical && !carry.empty() && !in.op2.shift_by_reg) line("        %s", carry.c_str());

        std::string res;
        const bool s = in.set_flags;
        switch (in.op) {
        case Op::AND: case Op::TST: res = "a & b"; break;
        case Op::EOR: case Op::TEQ: res = "a ^ b"; break;
        case Op::ORR: res = "a | b"; break;
        case Op::MOV: res = "b"; break;
        case Op::BIC: res = "a & ~b"; break;
        case Op::MVN: res = "~b"; break;
        case Op::SUB: case Op::CMP: res = s ? "rt::subs(c, a, b)" : "a - b"; break;
        case Op::RSB: res = s ? "rt::subs(c, b, a)" : "b - a"; break;
        case Op::ADD: case Op::CMN: res = s ? "rt::adds(c, a, b)" : "a + b"; break;
        case Op::ADC: res = s ? "rt::adcs(c, a, b)" : "a + b + (uint32_t)c.c_"; break;
        case Op::SBC: res = s ? "rt::sbcs(c, a, b)" : "a - b - (uint32_t)!c.c_"; break;
        default: res = s ? "rt::sbcs(c, b, a)" : "b - a - (uint32_t)!c.c_"; break; // RSC
        }
        line("        uint32_t res = %s;", res.c_str());
        if (s && logical) {
            line("        rt::nz(c, res);");
            if (need_sc) line("        c.c_ = sc;");
        }
        if (in.is_compare()) {
            line("        (void)res;");
            return true;
        }
        if (in.rd != 15) {
            line("        r[%u] = res;", in.rd);
            return true;
        }
        if (s) {
            ++st_.unimplemented;
            line("        g.unimplemented(%s, \"retorno de exceção\");", hex(in.addr).c_str());
            return false;
        }
        pc_write(in, "res", false);
        return false;
    }

    bool emit_mem(const Instr& in) {
        const bool load = is_load(in.op);
        // Literal na ROM: o valor é constante.
        if (in.op == Op::LDR && in.rn == 15 && in.op2.is_imm && in.pre_index && !in.writeback && in.rd != 15) {
            const uint32_t ea = in.up ? in.pc_value + in.op2.imm : in.pc_value - in.op2.imm;
            if (a_.in_rom(ea) && a_.in_rom(ea + 3)) {
                line("        r[%u] = %s;", in.rd, hex(std::rotr(rom_.read32(ea & ~3u), static_cast<int>((ea & 3) * 8))).c_str());
                return true;
            }
        }
        std::string off;
        if (in.op2.is_imm) {
            off = hex(in.op2.imm);
        } else {
            std::string carry, pre;
            Operand o = in.op2;
            off = operand(in, o, carry, pre); // sem shift por registrador em memória
        }
        line("        uint32_t base = %s;", R(in.rn, in).c_str());
        line("        uint32_t moved = base %c %s;", in.up ? '+' : '-', off.c_str());
        line("        uint32_t ea = %s;", in.pre_index ? "moved" : "base");
        if (load) {
            const char* fn = in.op == Op::LDR ? "rt::ldr" : in.op == Op::LDRB ? "rt::rd8" : in.op == Op::LDRH ? "rt::ldrh"
                           : in.op == Op::LDRSB ? "rt::ldrsb" : "rt::ldrsh";
            line("        uint32_t v = %s(g, ea);", fn);
            if (in.writeback && in.rn != in.rd) line("        r[%u] = moved;", in.rn);
            if (in.rd == 15) {
                pc_write(in, "v", false);
                return false;
            }
            line("        r[%u] = v;", in.rd);
        } else {
            const std::string v = in.rd == 15 ? hex(in.pc_value + 4) : fmt("r[%u]", in.rd);
            if (in.op == Op::STR) line("        rt::wr32(g, ea & ~3u, %s);", v.c_str());
            else if (in.op == Op::STRB) line("        rt::wr8(g, ea, %s);", v.c_str());
            else line("        rt::wr16(g, ea & ~1u, %s);", v.c_str());
            if (in.writeback) line("        r[%u] = moved;", in.rn);
        }
        return true;
    }

    bool emit_block(const Instr& in) {
        const uint16_t list = in.reg_list;
        const int n = std::popcount(list);
        if (n == 0 || in.user_bank) {
            ++st_.unimplemented;
            line("        g.unimplemented(%s, \"ldm/stm especial\");", hex(in.addr).c_str());
            return false;
        }
        line("        uint32_t base = r[%u];", in.rn);
        std::string start, final_base;
        if (in.up) {
            start = in.pre_index ? "base + 4u" : "base";
            final_base = fmt("base + %uu", 4 * n);
        } else {
            final_base = fmt("base - %uu", 4 * n);
            start = in.pre_index ? final_base : fmt("base - %uu", 4 * n - 4);
        }
        line("        uint32_t addr = %s;", start.c_str());
        if (in.op == Op::STM) {
            const int lowest = std::countr_zero(list);
            int k = 0;
            for (int i = 0; i < 16; ++i) {
                if (!((list >> i) & 1)) continue;
                std::string v;
                if (i == 15) v = hex(in.pc_value + (f_.thumb ? 2 : 4));
                else if (i == in.rn) v = (i == lowest) ? "base" : fmt("(%s)", final_base.c_str());
                else v = fmt("r[%d]", i);
                line("        rt::wr32(g, (addr + %du) & ~3u, %s);", 4 * k++, v.c_str());
            }
            if (in.writeback) line("        r[%u] = %s;", in.rn, final_base.c_str());
            return true;
        }
        if (in.writeback) line("        r[%u] = %s;", in.rn, final_base.c_str());
        int k = 0;
        for (int i = 0; i < 16; ++i) {
            if (!((list >> i) & 1)) continue;
            if (i == 15) line("        uint32_t pcv = rt::rd32(g, (addr + %du) & ~3u);", 4 * k++);
            else line("        r[%d] = rt::rd32(g, (addr + %du) & ~3u);", i, 4 * k++);
        }
        if (list & 0x8000) {
            pc_write(in, "pcv", false);
            return false;
        }
        return true;
    }

    const Analyzer& a_;
    const core::Rom& rom_;
    const Function& f_;
    std::string& o_;
    EmitStats& st_;
    std::set<uint32_t> labels_;
};

void write_file(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

} // namespace

EmitStats emit_cpp(const Analyzer& a, const core::Rom& rom, const std::filesystem::path& out_dir,
                   size_t functions_per_file) {
    namespace fs = std::filesystem;
    EmitStats st;
    fs::create_directories(out_dir);
    for (const auto& e : fs::directory_iterator(out_dir))
        if (e.path().extension() == ".cpp" || e.path().extension() == ".h") fs::remove(e.path());

    std::vector<const Function*> funcs;
    for (const auto& [addr, f] : a.functions())
        if (!f.invalid && !f.instrs.empty()) funcs.push_back(&f);

    // Declarações
    std::string decls = "#pragma once\n// Gerado por tlarecomp — não editar.\n#include \"gba/recomp_rt.h\"\n\n";
    for (const Function* f : funcs) decls += "void " + fn_name(f->addr) + "(gba::Gba& g);\n";
    write_file(out_dir / "recomp_decls.h", decls);

    // Corpos
    for (size_t i = 0; i < funcs.size(); i += functions_per_file) {
        std::string body = "// Gerado por tlarecomp a partir da ROM — não editar.\n#include \"recomp_decls.h\"\n\n";
        for (size_t k = i; k < funcs.size() && k < i + functions_per_file; ++k) {
            FunctionEmitter(a, rom, *funcs[k], body, st).emit();
            ++st.functions;
        }
        write_file(out_dir / fmt("recomp_%03zu.cpp", i / functions_per_file), body);
        ++st.files;
    }

    // Tabela de despacho
    std::string table = "// Gerado por tlarecomp — não editar.\n#include \"recomp_decls.h\"\n\n"
                        "extern const gba::RecompEntry tla_recomp_table[] = {\n";
    for (const Function* f : funcs)
        table += fmt("    {0x%08Xu, %s},\n", f->addr | (f->thumb ? 1u : 0u), fn_name(f->addr).c_str());
    table += "};\n";
    table += fmt("extern const size_t tla_recomp_count = %zu;\n", funcs.size());
    table += "extern const gba::RecompOverlay tla_recomp_overlays[] = {\n";
    for (const auto& o : a.overlays()) table += fmt("    {0x%08Xu, 0x%08Xu, %uu},\n", o.ram, o.rom, o.size);
    if (a.overlays().empty()) table += "    {0, 0, 0},\n";
    table += "};\n";
    table += fmt("extern const size_t tla_recomp_overlay_count = %zu;\n", a.overlays().size());
    write_file(out_dir / "recomp_table.cpp", table);
    ++st.files;
    return st;
}

} // namespace recomp
