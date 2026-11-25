#pragma once
// tlagba.ini: teclado, controle, atalhos, janela e áudio. Criado com os padrões (e comentários)
// na primeira execução, ao lado do executável, e regravado quando a tela de configurações muda
// alguma opção.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fe {

// Índices dos botões do GBA (mesma ordem dos bits de gba::Key).
enum GbaButton { BTN_A, BTN_B, BTN_SELECT, BTN_START, BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN, BTN_R, BTN_L, BTN_COUNT };

// Bits do estado bruto do gamepad além das máscaras XInput (que ocupam os 16 bits de baixo).
constexpr uint32_t PAD_GUIDE = 0x0400;    // botão Xbox / home (só via XInputGetStateEx)
constexpr uint32_t PAD_LT = 0x10000, PAD_RT = 0x20000;
constexpr uint32_t PAD_LS_UP = 0x40000, PAD_LS_DOWN = 0x80000, PAD_LS_LEFT = 0x100000, PAD_LS_RIGHT = 0x200000;

struct Config {
    int scale = 3;
    bool fullscreen = false;
    int volume = 100; // 0–100 (0 = mudo)
    std::array<std::vector<int>, BTN_COUNT> keyboard;    // códigos de tecla (VK) por botão
    std::array<std::vector<uint32_t>, BTN_COUNT> pad;    // máscaras do gamepad por botão
    bool analog = true;                                  // analógico esquerdo como direcional
    int deadzone = 8000;                                 // 0–32767
    int key_menu = 0, key_save = 0, key_load = 0, key_slot_prev = 0, key_slot_next = 0;
    int key_turbo = 0, key_pause = 0, key_fullscreen = 0, key_screenshot = 0;
    std::filesystem::path rom; // última ROM aberta (usada quando o executável é aberto sem argumentos)
};

// Lê o arquivo (criando-o com os padrões se não existir). Linhas inválidas viram avisos.
Config load_config(const std::filesystem::path& path, std::vector<std::string>& warnings);
Config default_config();
bool save_config(const std::filesystem::path& path, const Config& c);

const char* key_name(int vk);          // nome da tecla no ini ("X", "ENTER", "F5"…)
const char* pad_name(uint32_t mask);   // nome do botão no ini ("A", "LB", "DPAD_UP"…)
const char* button_name(int gba_button); // "A", "B", "SELECT"…

} // namespace fe
