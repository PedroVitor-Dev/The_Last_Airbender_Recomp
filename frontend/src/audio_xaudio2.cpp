#include "audio_out.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>

#include <array>
#include <cstdio>

namespace {

class XAudio2Out final : public AudioOut {
public:
    ~XAudio2Out() override {
        if (voice_) {
            voice_->Stop();
            voice_->DestroyVoice();
        }
        if (master_) master_->DestroyVoice();
        if (xa_) xa_->Release();
        if (com_) CoUninitialize();
    }

    bool init(uint32_t rate) {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (FAILED(XAudio2Create(&xa_, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(xa_->CreateMasteringVoice(&master_))) return false;
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = rate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = rate * 4;
        if (FAILED(xa_->CreateSourceVoice(&voice_, &fmt))) return false;
        return SUCCEEDED(voice_->Start());
    }

    void submit(const std::vector<int16_t>& samples) override {
        if (samples.empty()) return;
        if (queued() >= kBuffers) return; // fila cheia: descarta em vez de bloquear
        auto& buf = ring_[next_];
        next_ = (next_ + 1) % kBuffers;
        buf = samples;
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = static_cast<UINT32>(buf.size() * sizeof(int16_t));
        xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
        voice_->SubmitSourceBuffer(&xb);
    }

    uint32_t queued() const override {
        XAUDIO2_VOICE_STATE st{};
        voice_->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return st.BuffersQueued;
    }

private:
    static constexpr uint32_t kBuffers = 8;
    bool com_ = false;
    IXAudio2* xa_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    std::array<std::vector<int16_t>, kBuffers> ring_;
    uint32_t next_ = 0;
};

} // namespace

std::unique_ptr<AudioOut> AudioOut::create(uint32_t rate) {
    auto out = std::make_unique<XAudio2Out>();
    if (!out->init(rate)) {
        std::fprintf(stderr, "aviso: áudio indisponível (XAudio2)\n");
        return nullptr;
    }
    return out;
}

#else

std::unique_ptr<AudioOut> AudioOut::create(uint32_t) { return nullptr; }

#endif
