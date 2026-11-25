#pragma once

#include <cstdint>

#include "config.h"

namespace fe {

// Estado bruto do primeiro gamepad conectado: máscaras XInput (inclusive PAD_GUIDE, o botão
// Xbox/home, quando o driver expõe) + PAD_LT/PAD_RT + direções do analógico esquerdo
// (PAD_LS_*, segundo a zona morta). 0 se nenhum controle estiver conectado.
uint32_t poll_pad_raw(int deadzone);

// Botões do GBA (máscara de gba::Key) a partir do estado bruto, segundo o mapeamento do ini.
uint16_t pad_to_gba(const Config& cfg, uint32_t raw);

} // namespace fe
