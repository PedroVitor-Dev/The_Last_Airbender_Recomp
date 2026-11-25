#pragma once
// Decodificador ARMv4T (ARM7TDMI).
//
// ARM e Thumb são decodificados para a MESMA representação intermediária: toda
// instrução Thumb tem um equivalente ARM, então o recompilador e o interpretador só
// precisam conhecer um conjunto de operações. O par Thumb BL (prefixo + sufixo) é
// fundido numa única instrução BL de 4 bytes quando aparece completo.

#include <cstdint>
#include <string>

namespace core::arm {

enum class Cond : uint8_t { EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV };

enum class Op : uint8_t {
    // Processamento de dados (mesma ordem do opcode ARM, bits 21-24)
    AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN,
    // Multiplicação
    MUL, MLA, UMULL, UMLAL, SMULL, SMLAL,
    // Memória
    LDR, STR, LDRB, STRB, LDRH, STRH, LDRSB, LDRSH, LDM, STM, SWP, SWPB,
    // Desvios e sistema
    B, BL, BX, SWI, MRS, MSR,
    // Metades soltas do BL Thumb (quando o par não aparece junto)
    THUMB_BL_HI, // LR = PC + (offset << 12)
    THUMB_BL_LO, // PC = LR + (offset << 1); LR = retorno | 1
    COPROC,      // CDP/LDC/STC/MCR/MRC — não existem no GBA, tratado como indefinido
    UNDEFINED,
};

enum class ShiftType : uint8_t { LSL, LSR, ASR, ROR, RRX };

// Segundo operando de processamento de dados, ou deslocamento de acesso à memória.
struct Operand {
    bool is_imm = true;
    uint32_t imm = 0;       // valor já rotacionado / deslocamento imediato
    uint8_t imm_rotate = 0; // rotação usada (≠0 ⇒ carry = bit 31 do imediato)
    uint8_t rm = 0;
    ShiftType shift = ShiftType::LSL;
    bool shift_by_reg = false;
    uint8_t shift_amount = 0; // 0..32 (LSR/ASR #32 já normalizados)
    uint8_t rs = 0;           // registrador de deslocamento quando shift_by_reg
};

struct Instr {
    uint32_t addr = 0;
    uint32_t raw = 0;     // palavra ARM, ou halfword(s) Thumb (BL fundido: hi | lo<<16)
    uint8_t size = 4;     // 2 ou 4 bytes
    bool thumb = false;
    Op op = Op::UNDEFINED;
    Cond cond = Cond::AL;
    bool set_flags = false;

    uint8_t rd = 0;  // destino (RdLo em multiplicação longa)
    uint8_t rn = 0;  // base / 1º operando (RdHi em multiplicação longa)
    uint8_t rs = 0;  // multiplicação: Rs
    uint8_t rm = 0;  // multiplicação / SWP / BX: Rm
    Operand op2;     // DP: operando 2; memória: deslocamento

    // Memória
    bool pre_index = true;
    bool up = true;
    bool writeback = false;
    uint16_t reg_list = 0;
    bool user_bank = false; // bit S do LDM/STM (^)

    // Desvio
    uint32_t target = 0;    // endereço absoluto (B/BL); bit 0 não usado

    // PSR
    bool spsr = false;
    uint8_t psr_mask = 0;   // MSR: bits f,s,x,c (3..0)

    uint32_t swi_comment = 0;

    // Valor que o PC lê nesta instrução, já com o alinhamento que a instrução aplica
    // (ARM: addr+8; Thumb: addr+4; LDR/ADD relativos ao PC em Thumb: (addr+4)&~3).
    uint32_t pc_value = 0;

    bool is_data_processing() const { return op <= Op::MVN; }
    bool is_compare() const { return op >= Op::TST && op <= Op::CMN; }
    bool writes_pc() const;
};

Instr decode_arm(uint32_t addr, uint32_t word);
// `next` é a halfword seguinte, usada para fundir o par BL; passe 0 se indisponível.
Instr decode_thumb(uint32_t addr, uint16_t half, uint16_t next);

std::string disassemble(const Instr& in);
const char* cond_name(Cond c);
const char* op_name(Op op);
std::string reg_name(uint8_t r);

} // namespace core::arm
