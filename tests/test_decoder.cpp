// Testes do decodificador/disassembler ARMv4T com codificações conhecidas.
#include <cstdio>
#include <string>

#include "core/armv4t.h"

using namespace core::arm;

static int g_fail = 0, g_total = 0;

static void check(const char* what, const std::string& got, const std::string& expected) {
    ++g_total;
    if (got != expected) {
        ++g_fail;
        std::printf("FALHOU %s\n  esperado: %s\n  obtido  : %s\n", what, expected.c_str(), got.c_str());
    }
}

static void arm(uint32_t word, const char* expected, uint32_t addr = 0x08000000) {
    char what[32];
    std::snprintf(what, sizeof what, "ARM %08X", word);
    check(what, disassemble(decode_arm(addr, word)), expected);
}

static void thumb(uint16_t h, const char* expected, uint32_t addr = 0x08000000, uint16_t next = 0) {
    char what[32];
    std::snprintf(what, sizeof what, "Thumb %04X", h);
    check(what, disassemble(decode_thumb(addr, h, next)), expected);
}

int main() {
    // ── ARM ──
    arm(0xE3A00001, "mov r0, #1");
    arm(0xE3B00000, "movs r0, #0");
    arm(0xE3A004FF, "mov r0, #0xFF000000");
    arm(0xE0810002, "add r0, r1, r2");
    arm(0xE1A00081, "mov r0, r1, lsl #1");
    arm(0xE1A00120, "mov r0, r0, lsr #2");
    arm(0xE1A00020, "mov r0, r0, lsr #32");
    arm(0xE1A00060, "mov r0, r0, rrx");
    arm(0xE0810312, "add r0, r1, r2, lsl r3");
    arm(0xE12FFF1E, "bx lr");
    arm(0xEB000000, "bl 0x08000008");
    arm(0x0A000000, "beq 0x08000008");
    arm(0xEAFFFFFE, "b 0x08000000");
    arm(0xE59F0004, "ldr r0, [pc, #4] @ 0x0800000C");
    arm(0xE4910004, "ldr r0, [r1], #4");
    arm(0xE7910102, "ldr r0, [r1, r2, lsl #2]");
    arm(0xE1D000B2, "ldrh r0, [r0, #2]");
    arm(0xE92D4010, "push {r4, lr}");
    arm(0xE8BD8010, "pop {r4, pc}");
    arm(0xE8900006, "ldmia r0, {r1, r2}");
    arm(0xE0000291, "mul r0, r1, r2");
    arm(0xE0810392, "umull r0, r1, r2, r3");
    arm(0xE1010092, "swp r0, r2, [r1]");
    arm(0xEF000005, "swi 0x5");
    arm(0xE10F0000, "mrs r0, cpsr");
    arm(0xE129F000, "msr cpsr_fc, r0");

    // ── Thumb ──
    thumb(0x2001, "movs r0, #1");
    thumb(0x1888, "adds r0, r1, r2");
    thumb(0x0048, "movs r0, r1, lsl #1");
    thumb(0x4088, "movs r0, r0, lsl r1");
    thumb(0x40C8, "movs r0, r0, lsr r1");
    thumb(0x4240, "rsbs r0, r0, #0");
    thumb(0x4348, "muls r0, r1, r0");
    thumb(0x4685, "mov sp, r0");
    thumb(0x4770, "bx lr");
    thumb(0xB510, "push {r4, lr}");
    thumb(0xBD10, "pop {r4, pc}");
    thumb(0xB082, "sub sp, sp, #8");
    thumb(0x4801, "ldr r0, [pc, #4] @ 0x08000108", 0x08000100);
    thumb(0xA001, "add r0, pc, #4 @ 0x08000108", 0x08000102);
    thumb(0x9801, "ldr r0, [sp, #4]");
    thumb(0x8840, "ldrh r0, [r0, #2]");
    thumb(0x5C88, "ldrb r0, [r1, r2]");
    thumb(0x5E88, "ldrsh r0, [r1, r2]");
    thumb(0xC80C, "ldmia r0!, {r2, r3}");
    thumb(0xC803, "ldmia r0, {r0, r1}");
    thumb(0xD0FE, "beq 0x08000000");
    thumb(0xE7FE, "b 0x08000000");
    thumb(0xDF05, "swi 0x5");
    thumb(0xF000, "bl 0x08000008", 0x08000000, 0xF802);
    thumb(0xF000, "bl.hi #0x0");

    {
        auto bl = decode_thumb(0x08000000, 0xF000, 0xF802);
        ++g_total;
        if (bl.size != 4 || !bl.writes_pc()) { ++g_fail; std::printf("FALHOU BL fundido: tamanho/escrita de PC\n"); }
    }

    std::printf("%d/%d testes OK\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
