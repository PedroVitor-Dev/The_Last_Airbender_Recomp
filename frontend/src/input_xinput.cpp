// Gamepad via XInput (controles Xbox e compatíveis, como 8BitDo em modo X-input). Usa o
// primeiro controle conectado e procura de novo a cada ~2 s quando nenhum está conectado
// (consultar portas vazias é lento).
//
// O botão Xbox/home não aparece na API pública. A função não documentada XInputGetStateEx
// (ordinal 100 da xinput1_4.dll), usada por emuladores e pela Steam, devolve o mesmo estado com
// o bit 0x0400 para ele. Se não existir, o resto funciona normalmente (Back+Start abre o menu).

#include "input_xinput.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

namespace fe {

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

GetStateFn get_state_fn() {
    static GetStateFn fn = [] {
        if (HMODULE m = LoadLibraryW(L"xinput1_4.dll"))
            if (auto ex = reinterpret_cast<GetStateFn>(GetProcAddress(m, reinterpret_cast<LPCSTR>(100)))) return ex;
        return static_cast<GetStateFn>(&XInputGetState);
    }();
    return fn;
}

} // namespace

uint32_t poll_pad_raw(int deadzone) {
    static int port = -1;
    static int recheck = 0;
    const GetStateFn get = get_state_fn();

    XINPUT_STATE st{};
    if (port < 0) {
        if (--recheck > 0) return 0;
        recheck = 120;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
            if (get(i, &st) == ERROR_SUCCESS) {
                port = static_cast<int>(i);
                break;
            }
        if (port < 0) return 0;
    }
    if (get(static_cast<DWORD>(port), &st) != ERROR_SUCCESS) {
        port = -1; // desconectou
        return 0;
    }

    const XINPUT_GAMEPAD& gp = st.Gamepad;
    uint32_t raw = gp.wButtons;
    if (gp.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) raw |= PAD_LT;
    if (gp.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) raw |= PAD_RT;
    if (gp.sThumbLX > deadzone) raw |= PAD_LS_RIGHT;
    if (gp.sThumbLX < -deadzone) raw |= PAD_LS_LEFT;
    if (gp.sThumbLY > deadzone) raw |= PAD_LS_UP;
    if (gp.sThumbLY < -deadzone) raw |= PAD_LS_DOWN;
    return raw;
}

uint16_t pad_to_gba(const Config& cfg, uint32_t raw) {
    uint16_t keys = 0;
    for (int b = 0; b < BTN_COUNT; ++b)
        for (uint32_t m : cfg.pad[b])
            if (raw & m) keys |= static_cast<uint16_t>(1u << b);
    if (cfg.analog) {
        if (raw & PAD_LS_RIGHT) keys |= 1u << BTN_RIGHT;
        if (raw & PAD_LS_LEFT) keys |= 1u << BTN_LEFT;
        if (raw & PAD_LS_UP) keys |= 1u << BTN_UP;
        if (raw & PAD_LS_DOWN) keys |= 1u << BTN_DOWN;
    }
    return keys;
}

} // namespace fe

#endif
