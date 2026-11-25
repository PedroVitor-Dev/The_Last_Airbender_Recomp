// Interpretador ARM7TDMI sobre a IR de core::arm. É a referência de comportamento
// para o código recompilado e o fallback para código não recompilado.

#include <bit>
#include <cstdio>
#include <stdexcept>

#include "core/armv4t.h"
#include "gba/gba.h"

namespace gba {

using core::arm::Cond;
using core::arm::Instr;
using core::arm::Op;
using core::arm::Operand;
using core::arm::ShiftType;

Cpu::Cpu(Gba& gba) : gba_(gba) {}

// ───────────────────────────────────────────────────── modos e bancos ──

int Cpu::bank_index(uint32_t mode) const {
    switch (mode) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0;
    }
}

void Cpu::switch_mode(uint32_t new_mode) {
    const int ob = bank_index(mode_), nb = bank_index(new_mode);
    if (ob != nb) {
        bank_r13_[ob] = r[13];
        bank_r14_[ob] = r[14];
        bank_spsr_[ob] = spsr_;
        if (ob == 1) {
            for (int i = 0; i < 5; ++i) bank_fiq_r8_[i] = r[8 + i], r[8 + i] = bank_usr_r8_[i];
        }
        if (nb == 1) {
            for (int i = 0; i < 5; ++i) bank_usr_r8_[i] = r[8 + i], r[8 + i] = bank_fiq_r8_[i];
        }
        r[13] = bank_r13_[nb];
        r[14] = bank_r14_[nb];
        spsr_ = bank_spsr_[nb];
    }
    mode_ = new_mode;
}

uint32_t Cpu::cpsr() const {
    return (uint32_t(n_) << 31) | (uint32_t(z_) << 30) | (uint32_t(c_) << 29) | (uint32_t(v_) << 28) |
           (uint32_t(i_) << 7) | (uint32_t(f_) << 6) | (uint32_t(t_) << 5) | mode_;
}

void Cpu::set_cpsr(uint32_t v) {
    n_ = v >> 31 & 1;
    z_ = v >> 30 & 1;
    c_ = v >> 29 & 1;
    v_ = v >> 28 & 1;
    i_ = v >> 7 & 1;
    f_ = v >> 6 & 1;
    t_ = v >> 5 & 1;
    switch_mode((v & 0x1F) | 0x10);
}

uint32_t Cpu::user_reg(int n) const {
    if (n == 15 || n < 8) return r[n];
    if (bank_index(mode_) == 1 && n < 13) return bank_usr_r8_[n - 8];
    if (n >= 13 && bank_index(mode_) != 0) return n == 13 ? bank_r13_[0] : bank_r14_[0];
    return r[n];
}

void Cpu::set_user_reg(int n, uint32_t v) {
    if (n == 15 || n < 8) { r[n] = v; return; }
    if (bank_index(mode_) == 1 && n < 13) { bank_usr_r8_[n - 8] = v; return; }
    if (n >= 13 && bank_index(mode_) != 0) { (n == 13 ? bank_r13_[0] : bank_r14_[0]) = v; return; }
    r[n] = v;
}

void Cpu::reset_after_bios() {
    r.fill(0);
    bank_r13_.fill(0);
    bank_r14_.fill(0);
    bank_spsr_.fill(0);
    mode_ = MODE_SYS;
    bank_r13_[bank_index(MODE_SVC)] = 0x03007FE0;
    bank_r13_[bank_index(MODE_IRQ)] = 0x03007FA0;
    r[13] = 0x03007F00;
    set_cpsr(MODE_SYS);
    r[15] = 0x08000000;
    halted = false;
}

void Cpu::copy_state_from(const Cpu& o) {
    r = o.r;
    halted = o.halted;
    n_ = o.n_, z_ = o.z_, c_ = o.c_, v_ = o.v_;
    i_ = o.i_, f_ = o.f_, t_ = o.t_;
    mode_ = o.mode_;
    spsr_ = o.spsr_;
    bank_r13_ = o.bank_r13_;
    bank_r14_ = o.bank_r14_;
    bank_spsr_ = o.bank_spsr_;
    bank_fiq_r8_ = o.bank_fiq_r8_;
    bank_usr_r8_ = o.bank_usr_r8_;
}

void Cpu::branch_to(uint32_t addr, bool thumb) {
    t_ = thumb;
    r[15] = addr & (thumb ? ~1u : ~3u);
}

void Cpu::enter_irq() {
    const uint32_t old = cpsr();
    const uint32_t ret = r[15]; // próxima instrução a executar
    switch_mode(MODE_IRQ);
    spsr_ = old;
    r[14] = ret + 4;            // o handler retorna com "subs pc, lr, #4"
    i_ = true;
    t_ = false;
    r[15] = 0x18;
    halted = false;
    ++irq_count;
}

// ──────────────────────────────────────────────────────────── execução ──

namespace {

struct AluResult {
    uint32_t value;
    bool c, v;
};

inline AluResult add_with_carry(uint32_t a, uint32_t b, bool cin) {
    const uint64_t wide = uint64_t(a) + b + cin;
    const uint32_t r = static_cast<uint32_t>(wide);
    return {r, (wide >> 32) != 0, ((~(a ^ b) & (a ^ r)) >> 31) != 0};
}

} // namespace

uint32_t Cpu::step() {
    if (!i_ && gba_.io.irq_ready()) enter_irq();

    Bus& bus = gba_.bus;
    const uint32_t pc = r[15];
    Instr in;
    uint32_t cycles;
    if (t_) {
        const uint16_t h = bus.read16(pc);
        const uint16_t next = (h & 0xF800) == 0xF000 ? bus.read16(pc + 2) : 0;
        in = core::arm::decode_thumb(pc, h, next);
        cycles = Bus::access_cycles(pc, false);
    } else {
        in = core::arm::decode_arm(pc, bus.read32(pc));
        cycles = Bus::access_cycles(pc, true);
    }
    if (on_exec) on_exec(pc, t_);
    ++instr_count;
    r[15] = pc + in.size;

    // Condição
    bool pass;
    switch (in.cond) {
    case Cond::EQ: pass = z_; break;
    case Cond::NE: pass = !z_; break;
    case Cond::CS: pass = c_; break;
    case Cond::CC: pass = !c_; break;
    case Cond::MI: pass = n_; break;
    case Cond::PL: pass = !n_; break;
    case Cond::VS: pass = v_; break;
    case Cond::VC: pass = !v_; break;
    case Cond::HI: pass = c_ && !z_; break;
    case Cond::LS: pass = !c_ || z_; break;
    case Cond::GE: pass = n_ == v_; break;
    case Cond::LT: pass = n_ != v_; break;
    case Cond::GT: pass = !z_ && n_ == v_; break;
    case Cond::LE: pass = z_ || n_ != v_; break;
    case Cond::AL: pass = true; break;
    default: pass = false; break;
    }
    if (!pass) return cycles;

    auto R = [&](uint8_t n) -> uint32_t { return n == 15 ? in.pc_value : r[n]; };

    // Operando com deslocamento; atualiza `carry` com o carry do shifter.
    auto shifter = [&](const Operand& o, bool& carry) -> uint32_t {
        if (o.is_imm) {
            if (o.imm_rotate) carry = o.imm >> 31;
            return o.imm;
        }
        uint32_t v = R(o.rm);
        uint32_t amt = o.shift_by_reg ? (r[o.rs] & 0xFF) : o.shift_amount;
        if (o.shift_by_reg) {
            ++cycles;
            if (amt == 0) return v;
        }
        switch (o.shift) {
        case ShiftType::LSL:
            if (amt == 0) return v;
            if (amt < 32) { carry = (v >> (32 - amt)) & 1; return v << amt; }
            carry = amt == 32 ? (v & 1) : false;
            return 0;
        case ShiftType::LSR:
            if (amt < 32) { carry = (v >> (amt - 1)) & 1; return v >> amt; }
            carry = amt == 32 ? (v >> 31) : false;
            return 0;
        case ShiftType::ASR:
            if (amt < 32) { carry = (static_cast<int32_t>(v) >> (amt - 1)) & 1; return static_cast<uint32_t>(static_cast<int32_t>(v) >> amt); }
            carry = v >> 31;
            return carry ? 0xFFFFFFFFu : 0;
        case ShiftType::ROR:
            amt &= 31;
            if (amt == 0) { carry = v >> 31; return v; }
            v = std::rotr(v, static_cast<int>(amt));
            carry = v >> 31;
            return v;
        default: { // RRX
            const bool out = v & 1;
            v = (uint32_t(c_) << 31) | (v >> 1);
            carry = out;
            return v;
        }
        }
    };

    auto write_pc = [&](uint32_t v) {
        r[15] = v & (t_ ? ~1u : ~3u);
        cycles += 2;
    };

    switch (in.op) {
    // ── processamento de dados ──
    case Op::AND: case Op::EOR: case Op::SUB: case Op::RSB: case Op::ADD: case Op::ADC:
    case Op::SBC: case Op::RSC: case Op::TST: case Op::TEQ: case Op::CMP: case Op::CMN:
    case Op::ORR: case Op::MOV: case Op::BIC: case Op::MVN: {
        bool carry = c_;
        const uint32_t b = shifter(in.op2, carry);
        const uint32_t a = R(in.rn);
        uint32_t res = 0;
        bool arith = false;
        AluResult ar{};
        switch (in.op) {
        case Op::AND: case Op::TST: res = a & b; break;
        case Op::EOR: case Op::TEQ: res = a ^ b; break;
        case Op::ORR: res = a | b; break;
        case Op::MOV: res = b; break;
        case Op::BIC: res = a & ~b; break;
        case Op::MVN: res = ~b; break;
        case Op::SUB: case Op::CMP: ar = add_with_carry(a, ~b, true); arith = true; break;
        case Op::RSB: ar = add_with_carry(b, ~a, true); arith = true; break;
        case Op::ADD: case Op::CMN: ar = add_with_carry(a, b, false); arith = true; break;
        case Op::ADC: ar = add_with_carry(a, b, c_); arith = true; break;
        case Op::SBC: ar = add_with_carry(a, ~b, c_); arith = true; break;
        default: ar = add_with_carry(b, ~a, c_); arith = true; break; // RSC
        }
        if (arith) res = ar.value;
        const bool writes = !in.is_compare();
        if (in.set_flags && writes && in.rd == 15) {
            set_cpsr(spsr_); // retorno de exceção (ex.: subs pc, lr, #4)
            r[15] = res & (t_ ? ~1u : ~3u);
            cycles += 2;
            break;
        }
        if (in.set_flags) {
            n_ = res >> 31;
            z_ = res == 0;
            if (arith) c_ = ar.c, v_ = ar.v;
            else c_ = carry;
        }
        if (writes) {
            if (in.rd == 15) write_pc(res);
            else r[in.rd] = res;
        }
        break;
    }

    // ── multiplicação ──
    case Op::MUL: case Op::MLA: {
        uint32_t res = r[in.rm] * r[in.rs];
        if (in.op == Op::MLA) res += r[in.rn];
        r[in.rd] = res;
        if (in.set_flags) n_ = res >> 31, z_ = res == 0;
        cycles += 3;
        break;
    }
    case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL: {
        uint64_t res;
        if (in.op == Op::SMULL || in.op == Op::SMLAL)
            res = static_cast<uint64_t>(int64_t(int32_t(r[in.rm])) * int64_t(int32_t(r[in.rs])));
        else
            res = uint64_t(r[in.rm]) * uint64_t(r[in.rs]);
        if (in.op == Op::UMLAL || in.op == Op::SMLAL) res += (uint64_t(r[in.rn]) << 32) | r[in.rd];
        r[in.rd] = static_cast<uint32_t>(res);
        r[in.rn] = static_cast<uint32_t>(res >> 32);
        if (in.set_flags) n_ = (res >> 63) & 1, z_ = res == 0;
        cycles += 4;
        break;
    }

    // ── carga/armazenamento simples ──
    case Op::LDR: case Op::STR: case Op::LDRB: case Op::STRB:
    case Op::LDRH: case Op::STRH: case Op::LDRSB: case Op::LDRSH: {
        bool dummy = c_;
        const uint32_t off = in.op2.is_imm ? in.op2.imm : shifter(in.op2, dummy);
        const uint32_t base = R(in.rn);
        const uint32_t moved = in.up ? base + off : base - off;
        const uint32_t ea = in.pre_index ? moved : base;
        const bool load = in.op == Op::LDR || in.op == Op::LDRB || in.op == Op::LDRH ||
                          in.op == Op::LDRSB || in.op == Op::LDRSH;
        cycles += Bus::access_cycles(ea, in.op == Op::LDR || in.op == Op::STR);
        if (load) {
            uint32_t v;
            switch (in.op) {
            case Op::LDR: v = std::rotr(bus.read32(ea & ~3u), static_cast<int>((ea & 3) * 8)); break;
            case Op::LDRB: v = bus.read8(ea); break;
            case Op::LDRH: v = std::rotr(uint32_t(bus.read16(ea & ~1u)), static_cast<int>((ea & 1) * 8)); break;
            case Op::LDRSB: v = static_cast<uint32_t>(int32_t(int8_t(bus.read8(ea)))); break;
            default: // LDRSH (desalinhado lê byte com sinal)
                v = (ea & 1) ? static_cast<uint32_t>(int32_t(int8_t(bus.read8(ea))))
                             : static_cast<uint32_t>(int32_t(int16_t(bus.read16(ea))));
                break;
            }
            if (in.writeback && in.rn != in.rd) r[in.rn] = moved;
            if (in.rd == 15) write_pc(v);
            else r[in.rd] = v;
            ++cycles;
        } else {
            const uint32_t v = in.rd == 15 ? in.pc_value + 4 : r[in.rd];
            switch (in.op) {
            case Op::STR: bus.write32(ea & ~3u, v); break;
            case Op::STRB: bus.write8(ea, static_cast<uint8_t>(v)); break;
            default: bus.write16(ea & ~1u, static_cast<uint16_t>(v)); break;
            }
            if (in.writeback) r[in.rn] = moved;
        }
        break;
    }

    // ── carga/armazenamento múltiplo ──
    case Op::LDM: case Op::STM: {
        uint16_t list = in.reg_list;
        const uint32_t base = r[in.rn];
        uint32_t n = static_cast<uint32_t>(std::popcount(list));
        bool empty = false;
        if (n == 0) { list = 0x8000; n = 16; empty = true; } // peculiaridade do ARMv4
        uint32_t start, final_base;
        if (in.up) {
            start = in.pre_index ? base + 4 : base;
            final_base = base + 4 * n;
        } else {
            final_base = base - 4 * n;
            start = in.pre_index ? final_base : final_base + 4;
        }
        const bool pc_in = (list >> 15) & 1;
        const bool user = in.user_bank && !(in.op == Op::LDM && pc_in);
        uint32_t addr = start;
        cycles += n * Bus::access_cycles(start, true);
        if (in.op == Op::STM) {
            const int lowest = std::countr_zero(list);
            for (int i = 0; i < 16; ++i) {
                if (!((list >> i) & 1)) continue;
                uint32_t v;
                if (i == 15) v = in.pc_value + (t_ ? 2 : 4);
                else if (i == in.rn) v = (i == lowest) ? base : final_base;
                else v = user ? user_reg(i) : r[i];
                bus.write32(addr & ~3u, v);
                addr += 4;
            }
            if (in.writeback) r[in.rn] = empty ? (in.up ? base + 0x40 : base - 0x40) : final_base;
        } else {
            if (in.writeback) r[in.rn] = empty ? (in.up ? base + 0x40 : base - 0x40) : final_base;
            for (int i = 0; i < 16; ++i) {
                if (!((list >> i) & 1)) continue;
                const uint32_t v = bus.read32(addr & ~3u);
                addr += 4;
                if (i == 15) {
                    if (in.user_bank) set_cpsr(spsr_); // ldm ..., {pc}^
                    write_pc(v);
                } else if (user) {
                    set_user_reg(i, v);
                } else {
                    r[i] = v;
                }
            }
        }
        break;
    }

    case Op::SWP: case Op::SWPB: {
        const uint32_t a = r[in.rn];
        if (in.op == Op::SWP) {
            const uint32_t old = std::rotr(bus.read32(a & ~3u), static_cast<int>((a & 3) * 8));
            bus.write32(a & ~3u, r[in.rm]);
            r[in.rd] = old;
        } else {
            const uint8_t old = bus.read8(a);
            bus.write8(a, static_cast<uint8_t>(r[in.rm]));
            r[in.rd] = old;
        }
        cycles += 4;
        break;
    }

    // ── desvios ──
    case Op::B:
        r[15] = in.target;
        cycles += 2;
        break;
    case Op::BL:
        r[14] = t_ ? (r[15] | 1) : r[15];
        r[15] = in.target;
        cycles += 2;
        break;
    case Op::BX: {
        const uint32_t v = R(in.rm);
        branch_to(v, v & 1);
        cycles += 2;
        break;
    }
    case Op::THUMB_BL_HI:
        r[14] = in.pc_value + in.op2.imm;
        break;
    case Op::THUMB_BL_LO: {
        const uint32_t ret = r[15] | 1;
        r[15] = (r[14] + in.op2.imm) & ~1u;
        r[14] = ret;
        cycles += 2;
        break;
    }

    // ── PSR ──
    case Op::MRS:
        r[in.rd] = in.spsr ? spsr_ : cpsr();
        break;
    case Op::MSR: {
        bool dummy = c_;
        const uint32_t v = in.op2.is_imm ? in.op2.imm : shifter(in.op2, dummy);
        uint32_t mask = 0;
        if (in.psr_mask & 8) mask |= 0xFF000000;
        if (in.psr_mask & 1) mask |= 0x000000FF;
        if (in.spsr) {
            if (bank_index(mode_) != 0) spsr_ = (spsr_ & ~mask) | (v & mask);
        } else {
            if (mode_ == MODE_USR) mask &= 0xFF000000;
            const uint32_t nv = (cpsr() & ~mask) | (v & mask);
            const bool t = t_;
            set_cpsr(nv);
            t_ = t; // MSR não troca o estado Thumb
        }
        break;
    }

    case Op::SWI: {
        const uint32_t number = t_ ? in.swi_comment : (in.swi_comment >> 16);
        if (!bios_swi(gba_, number)) {
            std::fprintf(stderr, "[bios] SWI 0x%02X não suportada em 0x%08X\n", number, pc);
        }
        cycles += 3;
        break;
    }

    default: {
        char msg[96];
        std::snprintf(msg, sizeof msg, "instrução inválida em 0x%08X (%s): 0x%08X", pc, t_ ? "thumb" : "arm", in.raw);
        throw std::runtime_error(msg);
    }
    }
    return cycles;
}

} // namespace gba
