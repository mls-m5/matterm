#pragma once

#include "pipsynth.h"
#include "types.h"

#include <SDL.h>

#include <memory>

// Plays the core's pips through SDL audio (the film-terminal beeps of the
// demo). Opening the device can fail (no sound card, a remote session): then
// ok() is false and play() does nothing.
class Beeper {
public:
    Beeper();
    ~Beeper();
    Beeper(const Beeper &) = delete;
    Beeper &operator=(const Beeper &) = delete;

    bool ok() const {
        return device_ != 0;
    }
    void play(const fakeos::Beep &beep);

private:
    static void callback(void *self, Uint8 *stream, int bytes);

    SDL_AudioDeviceID device_ = 0;
    bool started_ = false;
    std::unique_ptr<PipSynth> synth_;
};
