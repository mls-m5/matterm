#include "beeper.h"

#include <algorithm>

namespace {
constexpr int kSampleRate = 44100;
constexpr double kMasterVolume = 0.5; // Pips are small blips; keep them gentle.
} // namespace

Beeper::Beeper()
    : synth_(std::make_unique<PipSynth>(kSampleRate)) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return;
    }
    SDL_AudioSpec want{};
    want.freq = kSampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = &Beeper::callback;
    want.userdata = this;
    SDL_AudioSpec have{};
    // No format changes allowed: SDL converts for the device instead.
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (device_ != 0) {
        SDL_PauseAudioDevice(device_, 0);
    }
}

Beeper::~Beeper() {
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Beeper::play(const fakeos::Beep &beep) {
    if (device_ == 0) {
        return;
    }
    SDL_LockAudioDevice(device_);
    synth_->add(beep.frequency, beep.durationMs, beep.volume * kMasterVolume);
    SDL_UnlockAudioDevice(device_);
}

void Beeper::callback(void *self, Uint8 *stream, int bytes) {
    auto *beeper = static_cast<Beeper *>(self);
    beeper->synth_->mix(reinterpret_cast<float *>(stream),
                        bytes / int(sizeof(float)));
}
