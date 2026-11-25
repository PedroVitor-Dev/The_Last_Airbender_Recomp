#include "core/armv4t.h"

namespace core::arm {

namespace {

constexpr uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
constexpr bool bit(uint32_t v, int n) { return (v >> n) & 1; }
constexpr uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

int32_t sign_extend(uint32_t v, int width) {
    uint32_t m = 1u << (width - 1);
    return static_cast<int32_t>((v ^ m) - m);
}

// Deslocamento por imediato com a normalização do ARM: LSR/ASR #0 = #32, ROR #0 = RRX.
void set_imm_shift(Operand& o, uint32_t type, uint32_t amount) {
    o.shift_by_reg = false;
    switch (type) {
    case 0: o.shift = ShiftType::LSL; o.shift_amount = static_cast<uint8_t>(amount); break;
    case 1: o.shift = ShiftType::LSR; o.shift_amount = static_cast<uint8_t>(amount ? amount : 32); break;
    case 2: o.shift = ShiftType::ASR; o.shift_amount = static_cast<uint8_t>(amount ? amount : 32); break;
    default:
        if (amount) { o.shift = ShiftType::ROR; o.shift_amount = static_cast<uint8_t>(amount); }
        else { o.shift = ShiftType::RRX; o.shift_amount = 1; }
        break;
    }
}

Operand reg_operand(uint8_t rm) {
    Operand o;
    o.is_imm = false;
    o.rm = rm;
    return o;
}

Operand imm_operand(uint32_t v) {
    Operand o;
    o.is_imm = true;
    o.imm = v;
    return o;
}

Instr undefined(Instr in) {
    in.op = Op::UNDEFINED;
    return in;
}

} // namespace

bool Instr::writes_pc() const {
    switch (op) {
    case Op::B: case Op::BL: case Op::BX: case Op::THUMB_BL_LO: return true;
    case Op::LDM: return (reg_list >> 15) & 1;
    case Op::LDR: return rd == 15;
    case Op::MUL: case Op::MLA: return false;
    default:
        if (is_data_processing() && !is_compare()) return rd == 15;
        return false;
    }
}

// ───────────────────────────────────────────────────────────────── ARM ──

Instr decode_arm(uint32_t addr, uint32_t w) {
    Instr in;
    in.addr = addr;
    in.raw = w;
    in.size = 4;
    in.thumb = false;
    in.cond = static_cast<Cond>(bits(w, 31, 28));
    in.pc_value = addr + 8;

    if (in.cond == Cond::NV) return undefined(in); // espaço incondicional não existe no v4T

    const uint32_t cls = bits(w, 27, 25);

    if (cls == 0b000) {
        if ((w & 0x0FFFFFF0) == 0x012FFF10) { // BX
            in.op = Op::BX;
            in.rm = static_cast<uint8_t>(bits(w, 3, 0));
            return in;
        }
        if ((w & 0x0FC000F0) == 0x00000090) { // MUL / MLA
            in.op = bit(w, 21) ? Op::MLA : Op::MUL;
            in.set_flags = bit(w, 20);
            in.rd = static_cast<uint8_t>(bits(w, 19, 16));
            in.rn = static_cast<uint8_t>(bits(w, 15, 12));
            in.rs = static_cast<uint8_t>(bits(w, 11, 8));
            in.rm = static_cast<uint8_t>(bits(w, 3, 0));
            return in;
        }
        if ((w & 0x0F8000F0) == 0x00800090) { // multiplicação longa
            const bool sign = bit(w, 22), acc = bit(w, 21);
            in.op = sign ? (acc ? Op::SMLAL : Op::SMULL) : (acc ? Op::UMLAL : Op::UMULL);
            in.set_flags = bit(w, 20);
            in.rn = static_cast<uint8_t>(bits(w, 19, 16)); // RdHi
            in.rd = static_cast<uint8_t>(bits(w, 15, 12)); // RdLo
            in.rs = static_cast<uint8_t>(bits(w, 11, 8));
            in.rm = static_cast<uint8_t>(bits(w, 3, 0));
            return in;
        }
        if ((w & 0x0FB00FF0) == 0x01000090) { // SWP / SWPB
            in.op = bit(w, 22) ? Op::SWPB : Op::SWP;
            in.rn = static_cast<uint8_t>(bits(w, 19, 16));
            in.rd = static_cast<uint8_t>(bits(w, 15, 12));
            in.rm = static_cast<uint8_t>(bits(w, 3, 0));
            return in;
        }
        if ((w & 0x0E000090) == 0x00000090) { // transferências de halfword / com sinal
            const uint32_t sh = bits(w, 6, 5);
            const bool load = bit(w, 20);
            if (!load && sh != 1) return undefined(in); // LDRD/STRD só no v5TE
            in.op = !load ? Op::STRH : (sh == 1 ? Op::LDRH : sh == 2 ? Op::LDRSB : Op::LDRSH);
            in.pre_index = bit(w, 24);
            in.up = bit(w, 23);
            in.writeback = !in.pre_index || bit(w, 21);
            in.rn = static_cast<uint8_t>(bits(w, 19, 16));
            in.rd = static_cast<uint8_t>(bits(w, 15, 12));
            in.op2 = bit(w, 22) ? imm_operand((bits(w, 11, 8) << 4) | bits(w, 3, 0))
                                : reg_operand(static_cast<uint8_t>(bits(w, 3, 0)));
            return in;
        }
        if ((w & 0x0FBF0FFF) == 0x010F0000) { // MRS
            in.op = Op::MRS;
            in.spsr = bit(w, 22);
            in.rd = static_cast<uint8_t>(bits(w, 15, 12));
            return in;
        }
        if ((w & 0x0FB0FFF0) == 0x0120F000) { // MSR (registrador)
            in.op = Op::MSR;
            in.spsr = bit(w, 22);
            in.psr_mask = static_cast<uint8_t>(bits(w, 19, 16));
            in.op2 = reg_operand(static_cast<uint8_t>(bits(w, 3, 0)));
            return in;
        }
    }

    if (cls == 0b001 && (w & 0x0FB0F000) == 0x0320F000) { // MSR (imediato)
        in.op = Op::MSR;
        in.spsr = bit(w, 22);
        in.psr_mask = static_cast<uint8_t>(bits(w, 19, 16));
        const uint32_t rot = bits(w, 11, 8) * 2;
        in.op2 = imm_operand(ror32(bits(w, 7, 0), rot));
        in.op2.imm_rotate = static_cast<uint8_t>(rot);
        return in;
    }

    if (cls == 0b000 || cls == 0b001) { // processamento de dados
        in.op = static_cast<Op>(bits(w, 24, 21));
        in.set_flags = bit(w, 20);
        in.rn = static_cast<uint8_t>(bits(w, 19, 16));
        in.rd = static_cast<uint8_t>(bits(w, 15, 12));
        if (in.is_compare() && !in.set_flags) return undefined(in); // espaço de MRS/MSR
        if (cls == 0b001) {
            const uint32_t rot = bits(w, 11, 8) * 2;
            in.op2 = imm_operand(ror32(bits(w, 7, 0), rot));
            in.op2.imm_rotate = static_cast<uint8_t>(rot);
        } else {
            in.op2 = reg_operand(static_cast<uint8_t>(bits(w, 3, 0)));
            if (bit(w, 4)) {
                in.op2.shift_by_reg = true;
                in.op2.rs = static_cast<uint8_t>(bits(w, 11, 8));
                in.op2.shift = static_cast<ShiftType>(bits(w, 6, 5));
                in.pc_value = addr + 12; // PC lido um ciclo depois
            } else {
                set_imm_shift(in.op2, bits(w, 6, 5), bits(w, 11, 7));
            }
        }
        return in;
    }

    if (cls == 0b010 || cls == 0b011) { // LDR/STR
        if (cls == 0b011 && bit(w, 4)) return undefined(in);
        const bool load = bit(w, 20), byte = bit(w, 22);
        in.op = load ? (byte ? Op::LDRB : Op::LDR) : (byte ? Op::STRB : Op::STR);
        in.pre_index = bit(w, 24);
        in.up = bit(w, 23);
        in.writeback = !in.pre_index || bit(w, 21);
        in.user_bank = !in.pre_index && bit(w, 21); // LDRT/STRT
        in.rn = static_cast<uint8_t>(bits(w, 19, 16));
        in.rd = static_cast<uint8_t>(bits(w, 15, 12));
        if (cls == 0b010) {
            in.op2 = imm_operand(bits(w, 11, 0));
        } else {
            in.op2 = reg_operand(static_cast<uint8_t>(bits(w, 3, 0)));
            set_imm_shift(in.op2, bits(w, 6, 5), bits(w, 11, 7));
        }
        return in;
    }

    if (cls == 0b100) { // LDM/STM
        in.op = bit(w, 20) ? Op::LDM : Op::STM;
        in.pre_index = bit(w, 24);
        in.up = bit(w, 23);
        in.user_bank = bit(w, 22);
        in.writeback = bit(w, 21);
        in.rn = static_cast<uint8_t>(bits(w, 19, 16));
        in.reg_list = static_cast<uint16_t>(bits(w, 15, 0));
        return in;
    }

    if (cls == 0b101) { // B/BL
        in.op = bit(w, 24) ? Op::BL : Op::B;
        in.target = addr + 8 + static_cast<uint32_t>(sign_extend(bits(w, 23, 0), 24) * 4);
        return in;
    }

    if (bits(w, 27, 24) == 0xF) { // SWI
        in.op = Op::SWI;
        in.swi_comment = bits(w, 23, 0);
        return in;
    }

    in.op = Op::COPROC;
    return in;
}

// ─────────────────────────────────────────────────────────────── Thumb ──

Instr decode_thumb(uint32_t addr, uint16_t h, uint16_t next) {
    Instr in;
    in.addr = addr;
    in.raw = h;
    in.size = 2;
    in.thumb = true;
    in.cond = Cond::AL;
    in.pc_value = addr + 4;

    auto r = [h](int lo) { return static_cast<uint8_t>(bits(h, lo + 2, lo)); };

    switch (h >> 13) {
    case 0b000:
        if (bits(h, 12, 11) == 0b11) { // formato 2: ADD/SUB registrador ou imm3
            in.op = bit(h, 9) ? Op::SUB : Op::ADD;
            in.set_flags = true;
            in.rd = r(0);
            in.rn = r(3);
            in.op2 = bit(h, 10) ? imm_operand(bits(h, 8, 6)) : reg_operand(r(6));
        } else { // formato 1: deslocamento por imediato → MOVS rd, rm, <shift> #imm
            in.op = Op::MOV;
            in.set_flags = true;
            in.rd = r(0);
            in.op2 = reg_operand(r(3));
            set_imm_shift(in.op2, bits(h, 12, 11), bits(h, 10, 6));
        }
        return in;

    case 0b001: { // formato 3: MOV/CMP/ADD/SUB imm8
        static constexpr Op ops[] = {Op::MOV, Op::CMP, Op::ADD, Op::SUB};
        in.op = ops[bits(h, 12, 11)];
        in.set_flags = true;
        in.rd = in.rn = r(8);
        in.op2 = imm_operand(bits(h, 7, 0));
        return in;
    }

    case 0b010:
        if (bits(h, 12, 10) == 0b000) { // formato 4: ALU
            const uint8_t rd = r(0), rs = r(3);
            in.set_flags = true;
            in.rd = in.rn = rd;
            in.op2 = reg_operand(rs);
            switch (bits(h, 9, 6)) {
            case 0x0: in.op = Op::AND; break;
            case 0x1: in.op = Op::EOR; break;
            case 0x2: case 0x3: case 0x4: case 0x7: { // LSL/LSR/ASR/ROR por registrador
                // Indexado pelo opcode ALU: 2=LSL, 3=LSR, 4=ASR, 7=ROR.
                static constexpr ShiftType st[] = {ShiftType::LSL, ShiftType::LSL, ShiftType::LSL,
                                                   ShiftType::LSR, ShiftType::ASR, ShiftType::LSL,
                                                   ShiftType::LSL, ShiftType::ROR};
                in.op = Op::MOV;
                in.op2 = reg_operand(rd);
                in.op2.shift_by_reg = true;
                in.op2.rs = rs;
                in.op2.shift = st[bits(h, 9, 6)];
                break;
            }
            case 0x5: in.op = Op::ADC; break;
            case 0x6: in.op = Op::SBC; break;
            case 0x8: in.op = Op::TST; break;
            case 0x9: in.op = Op::RSB; in.rn = rs; in.op2 = imm_operand(0); break; // NEG
            case 0xA: in.op = Op::CMP; break;
            case 0xB: in.op = Op::CMN; break;
            case 0xC: in.op = Op::ORR; break;
            case 0xD: in.op = Op::MUL; in.rm = rs; in.rs = rd; break; // MULS rd, rs, rd
            case 0xE: in.op = Op::BIC; break;
            default:  in.op = Op::MVN; break;
            }
            return in;
        }
        if (bits(h, 12, 10) == 0b001) { // formato 5: registradores altos / BX
            const uint8_t rd = static_cast<uint8_t>(r(0) | (bit(h, 7) << 3));
            const uint8_t rs = static_cast<uint8_t>(r(3) | (bit(h, 6) << 3));
            switch (bits(h, 9, 8)) {
            case 0: in.op = Op::ADD; in.rd = in.rn = rd; in.op2 = reg_operand(rs); break;
            case 1: in.op = Op::CMP; in.set_flags = true; in.rn = rd; in.op2 = reg_operand(rs); break;
            case 2: in.op = Op::MOV; in.rd = rd; in.op2 = reg_operand(rs); break;
            default: in.op = Op::BX; in.rm = rs; break;
            }
            return in;
        }
        if (bits(h, 12, 11) == 0b01) { // formato 6: LDR rd, [pc, #imm8*4]
            in.op = Op::LDR;
            in.rd = r(8);
            in.rn = 15;
            in.op2 = imm_operand(bits(h, 7, 0) * 4);
            in.pc_value = (addr + 4) & ~3u;
            return in;
        }
        { // formatos 7 e 8: carga/armazenamento com deslocamento por registrador
            in.rd = r(0);
            in.rn = r(3);
            in.op2 = reg_operand(r(6));
            static constexpr Op f7[] = {Op::STR, Op::STRB, Op::LDR, Op::LDRB};
            static constexpr Op f8[] = {Op::STRH, Op::LDRSB, Op::LDRH, Op::LDRSH};
            in.op = bit(h, 9) ? f8[bits(h, 11, 10)] : f7[bits(h, 11, 10)];
            return in;
        }

    case 0b011: { // formato 9: imm5 (palavra ×4, byte ×1)
        const bool byte = bit(h, 12), load = bit(h, 11);
        in.op = load ? (byte ? Op::LDRB : Op::LDR) : (byte ? Op::STRB : Op::STR);
        in.rd = r(0);
        in.rn = r(3);
        in.op2 = imm_operand(bits(h, 10, 6) * (byte ? 1 : 4));
        return in;
    }

    case 0b100:
        if (!bit(h, 12)) { // formato 10: LDRH/STRH imm5*2
            in.op = bit(h, 11) ? Op::LDRH : Op::STRH;
            in.rd = r(0);
            in.rn = r(3);
            in.op2 = imm_operand(bits(h, 10, 6) * 2);
        } else { // formato 11: relativo ao SP
            in.op = bit(h, 11) ? Op::LDR : Op::STR;
            in.rd = r(8);
            in.rn = 13;
            in.op2 = imm_operand(bits(h, 7, 0) * 4);
        }
        return in;

    case 0b101:
        if (!bit(h, 12)) { // formato 12: ADD rd, PC|SP, #imm8*4
            in.op = Op::ADD;
            in.rd = r(8);
            in.rn = bit(h, 11) ? 13 : 15;
            in.op2 = imm_operand(bits(h, 7, 0) * 4);
            if (in.rn == 15) in.pc_value = (addr + 4) & ~3u;
            return in;
        }
        if (bits(h, 11, 8) == 0b0000) { // formato 13: ADD/SUB SP, #imm7*4
            in.op = bit(h, 7) ? Op::SUB : Op::ADD;
            in.rd = in.rn = 13;
            in.op2 = imm_operand(bits(h, 6, 0) * 4);
            return in;
        }
        if (bits(h, 10, 9) == 0b10) { // formato 14: PUSH/POP
            const bool pop = bit(h, 11);
            in.op = pop ? Op::LDM : Op::STM;
            in.rn = 13;
            in.writeback = true;
            in.pre_index = !pop; // PUSH = STMDB, POP = LDMIA
            in.up = pop;
            in.reg_list = static_cast<uint16_t>(bits(h, 7, 0));
            if (bit(h, 8)) in.reg_list |= pop ? (1u << 15) : (1u << 14);
            return in;
        }
        return undefined(in);

    case 0b110:
        if (!bit(h, 12)) { // formato 15: LDMIA/STMIA rb!
            in.op = bit(h, 11) ? Op::LDM : Op::STM;
            in.rn = r(8);
            in.reg_list = static_cast<uint16_t>(bits(h, 7, 0));
            in.pre_index = false;
            in.up = true;
            // Em LDM com rb na lista, o writeback não acontece (valor carregado prevalece).
            in.writeback = !(in.op == Op::LDM && ((in.reg_list >> in.rn) & 1));
            return in;
        }
        {
            const uint32_t c = bits(h, 11, 8);
            if (c == 0xF) { // formato 17: SWI
                in.op = Op::SWI;
                in.swi_comment = bits(h, 7, 0);
                return in;
            }
            if (c == 0xE) return undefined(in);
            in.op = Op::B; // formato 16: desvio condicional
            in.cond = static_cast<Cond>(c);
            in.target = addr + 4 + static_cast<uint32_t>(sign_extend(bits(h, 7, 0), 8) * 2);
            return in;
        }

    default: // 0b111
        switch (bits(h, 12, 11)) {
        case 0b00: // formato 18: B incondicional
            in.op = Op::B;
            in.target = addr + 4 + static_cast<uint32_t>(sign_extend(bits(h, 10, 0), 11) * 2);
            return in;
        case 0b10: { // formato 19, primeira metade
            const int32_t hi = sign_extend(bits(h, 10, 0), 11) << 12;
            if ((next & 0xF800) == 0xF800) { // par completo → BL
                in.op = Op::BL;
                in.size = 4;
                in.raw = h | (uint32_t(next) << 16);
                in.target = addr + 4 + static_cast<uint32_t>(hi) + (bits(next, 10, 0) << 1);
                return in;
            }
            in.op = Op::THUMB_BL_HI;
            in.op2 = imm_operand(static_cast<uint32_t>(hi));
            return in;
        }
        case 0b11: // segunda metade solta
            in.op = Op::THUMB_BL_LO;
            in.op2 = imm_operand(bits(h, 10, 0) << 1);
            return in;
        default: // 0b01: BLX(2) no v5, indefinido no v4T
            return undefined(in);
        }
    }
}

} // namespace core::arm
