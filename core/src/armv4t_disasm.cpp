#include <cstdio>

#include "core/armv4t.h"

namespace core::arm {

namespace {

std::string fmt(const char* f, uint32_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string imm_str(uint32_t v) { return v <= 9 ? fmt("#%u", v) : fmt("#0x%X", v); }

const char* shift_name(ShiftType s) {
    switch (s) {
    case ShiftType::LSL: return "lsl";
    case ShiftType::LSR: return "lsr";
    case ShiftType::ASR: return "asr";
    case ShiftType::ROR: return "ror";
    default: return "rrx";
    }
}

std::string shifted_reg(const Operand& o) {
    std::string s = reg_name(o.rm);
    if (o.shift == ShiftType::RRX) return s + ", rrx";
    if (o.shift_by_reg) return s + ", " + shift_name(o.shift) + " " + reg_name(o.rs);
    if (o.shift == ShiftType::LSL && o.shift_amount == 0) return s;
    return s + ", " + shift_name(o.shift) + " #" + std::to_string(o.shift_amount);
}

std::string operand_str(const Operand& o) { return o.is_imm ? imm_str(o.imm) : shifted_reg(o); }

std::string reg_list_str(uint16_t list) {
    std::string s = "{";
    bool first = true;
    for (int r = 0; r < 16;) {
        if (!((list >> r) & 1)) { ++r; continue; }
        int end = r;
        // Faixas só para r0-r12; sp/lr/pc sempre explícitos.
        while (end + 1 <= 12 && ((list >> (end + 1)) & 1)) ++end;
        if (!first) s += ", ";
        first = false;
        s += reg_name(static_cast<uint8_t>(r));
        if (end > r) s += (end == r + 1 ? ", " : "-") + reg_name(static_cast<uint8_t>(end));
        r = end + 1;
    }
    return s + "}";
}

std::string psr_str(const Instr& in) {
    std::string s = in.spsr ? "spsr" : "cpsr";
    if (in.op == Op::MSR) {
        s += "_";
        if (in.psr_mask & 8) s += "f";
        if (in.psr_mask & 4) s += "s";
        if (in.psr_mask & 2) s += "x";
        if (in.psr_mask & 1) s += "c";
    }
    return s;
}

std::string mnemonic(const Instr& in, const char* base, bool s_suffix) {
    std::string m = base;
    if (in.cond != Cond::AL) m += cond_name(in.cond);
    if (s_suffix && in.set_flags) m += "s";
    return m;
}

std::string mem_str(const Instr& in) {
    std::string base = "[" + reg_name(in.rn);
    std::string off;
    const bool zero = in.op2.is_imm && in.op2.imm == 0;
    if (in.op2.is_imm) {
        off = std::string("#") + (in.up ? "" : "-") + (in.op2.imm <= 9 ? std::to_string(in.op2.imm)
                                                                      : fmt("0x%X", in.op2.imm));
    } else {
        off = std::string(in.up ? "" : "-") + shifted_reg(in.op2);
    }
    std::string s;
    if (in.pre_index) {
        s = base + (zero ? "" : ", " + off) + "]" + (in.writeback ? "!" : "");
        if (in.rn == 15 && in.op2.is_imm && !in.writeback) {
            uint32_t ea = in.up ? in.pc_value + in.op2.imm : in.pc_value - in.op2.imm;
            s += fmt(" @ 0x%08X", ea);
        }
    } else {
        s = base + "], " + off;
    }
    return s;
}

} // namespace

const char* cond_name(Cond c) {
    static const char* n[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                              "hi", "ls", "ge", "lt", "gt", "le", "", "nv"};
    return n[static_cast<int>(c)];
}

const char* op_name(Op op) {
    static const char* n[] = {
        "and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn",
        "orr", "mov", "bic", "mvn", "mul", "mla", "umull", "umlal", "smull", "smlal",
        "ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh", "ldm", "stm", "swp", "swpb",
        "b", "bl", "bx", "swi", "mrs", "msr", "bl.hi", "bl.lo", "cdp", "undefined"};
    return n[static_cast<int>(op)];
}

std::string reg_name(uint8_t r) {
    switch (r) {
    case 13: return "sp";
    case 14: return "lr";
    case 15: return "pc";
    default: return "r" + std::to_string(r);
    }
}

std::string disassemble(const Instr& in) {
    const std::string rd = reg_name(in.rd), rn = reg_name(in.rn);
    switch (in.op) {
    case Op::MOV: case Op::MVN:
        return mnemonic(in, op_name(in.op), true) + " " + rd + ", " + operand_str(in.op2);
    case Op::TST: case Op::TEQ: case Op::CMP: case Op::CMN:
        return mnemonic(in, op_name(in.op), false) + " " + rn + ", " + operand_str(in.op2);
    case Op::MUL:
        return mnemonic(in, "mul", true) + " " + rd + ", " + reg_name(in.rm) + ", " + reg_name(in.rs);
    case Op::MLA:
        return mnemonic(in, "mla", true) + " " + rd + ", " + reg_name(in.rm) + ", " +
               reg_name(in.rs) + ", " + rn;
    case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL:
        return mnemonic(in, op_name(in.op), true) + " " + rd + ", " + rn + ", " +
               reg_name(in.rm) + ", " + reg_name(in.rs);
    case Op::LDR: case Op::STR: case Op::LDRB: case Op::STRB:
    case Op::LDRH: case Op::STRH: case Op::LDRSB: case Op::LDRSH: {
        std::string m = mnemonic(in, op_name(in.op), false);
        if (in.user_bank) m += "t";
        return m + " " + rd + ", " + mem_str(in);
    }
    case Op::LDM: case Op::STM: {
        const bool push = in.op == Op::STM && in.rn == 13 && in.writeback && in.pre_index && !in.up;
        const bool pop = in.op == Op::LDM && in.rn == 13 && in.writeback && !in.pre_index && in.up;
        std::string list = reg_list_str(in.reg_list) + (in.user_bank ? "^" : "");
        if (push) return mnemonic(in, "push", false) + " " + list;
        if (pop) return mnemonic(in, "pop", false) + " " + list;
        std::string m = mnemonic(in, op_name(in.op), false);
        m += in.up ? (in.pre_index ? "ib" : "ia") : (in.pre_index ? "db" : "da");
        return m + " " + rn + (in.writeback ? "!" : "") + ", " + list;
    }
    case Op::SWP: case Op::SWPB:
        return mnemonic(in, op_name(in.op), false) + " " + rd + ", " + reg_name(in.rm) + ", [" + rn + "]";
    case Op::B: case Op::BL:
        return mnemonic(in, op_name(in.op), false) + fmt(" 0x%08X", in.target);
    case Op::BX:
        return mnemonic(in, "bx", false) + " " + reg_name(in.rm);
    case Op::SWI:
        return mnemonic(in, "swi", false) + fmt(" 0x%X", in.swi_comment);
    case Op::MRS:
        return mnemonic(in, "mrs", false) + " " + rd + ", " + psr_str(in);
    case Op::MSR:
        return mnemonic(in, "msr", false) + " " + psr_str(in) + ", " + operand_str(in.op2);
    case Op::THUMB_BL_HI: case Op::THUMB_BL_LO:
        return std::string(op_name(in.op)) + fmt(" #0x%X", in.op2.imm);
    case Op::COPROC: case Op::UNDEFINED:
        return in.thumb ? fmt(".hword 0x%04X", in.raw) : fmt(".word 0x%08X", in.raw);
    default: { // processamento de dados com 3 operandos
        std::string s = mnemonic(in, op_name(in.op), true) + " " + rd + ", " + rn + ", " +
                        operand_str(in.op2);
        if (in.rn == 15 && in.op2.is_imm && in.rd != 15 && (in.op == Op::ADD || in.op == Op::SUB)) {
            uint32_t v = in.op == Op::ADD ? in.pc_value + in.op2.imm : in.pc_value - in.op2.imm;
            s += fmt(" @ 0x%08X", v);
        }
        return s;
    }
    }
}

} // namespace core::arm
