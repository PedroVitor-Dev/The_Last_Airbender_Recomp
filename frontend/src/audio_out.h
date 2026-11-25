#pragma once
// Saída de áudio do frontend (XAudio2 no Windows). O áudio também serve de relógio:
// o laço principal espera enquanto há buffers demais na fila.

#include <cstdint>
#include <memory>
#include <vector>

class AudioOut {
public:
    static std::unique_ptr<AudioOut> create(uint32_t sample_rate); // nullptr se indisponível
    virtual ~AudioOut() = default;

    // Enfileira amostras estéreo int16 intercaladas.
    virtual void submit(const std::vector<int16_t>& samples) = 0;
    // Quantos buffers ainda aguardam reprodução.
    virtual uint32_t queued() const = 0;
};
