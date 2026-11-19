#pragma once

// The pip synthesizer of the native demo: short electronic blips mixed into a
// float buffer. No audio device in here, so it can be tested; beeper.cpp plays
// it through SDL.

#include <algorithm>
#include <cmath>
#include <vector>

class PipSynth {
public:
    explicit PipSynth(int sampleRate)
        : rate_(sampleRate) {}

    // A pip of `frequency` Hertz for `durationMs`, `volume` 0..1. Pips are
    // soft at both ends (no clicks); at most kMaxPips sound at once (the oldest
    // gives way).
    void add(double frequency, int durationMs, double volume) {
        Pip pip;
        pip.increment = frequency / rate_;
        pip.total = std::max(1, int(double(rate_) * durationMs / 1000.0));
        pip.volume = float(std::clamp(volume, 0.0, 1.0));
        pips_.push_back(pip);
        if (pips_.size() > kMaxPips) {
            pips_.erase(pips_.begin());
        }
    }

    // Writes (not adds to) `frames` mono samples.
    void mix(float *out, int frames) {
        std::fill(out, out + frames, 0.0f);
        for (auto &pip : pips_) {
            const int edge = std::max(
                1, std::min(pip.total / 4, rate_ / 400)); // About 2.5 ms.
            for (int i = 0; i < frames && pip.position < pip.total;
                 ++i, ++pip.position) {
                const int left = pip.total - pip.position;
                const float envelope =
                    std::min({1.0f,
                              float(pip.position + 1) / float(edge),
                              float(left) / float(edge)});
                // A square wave softened with its third harmonic: the sound of
                // a film computer without the harshness.
                const double phase = pip.phase;
                const float wave = float(0.7 * std::sin(2 * M_PI * phase) +
                                         0.25 * std::sin(6 * M_PI * phase));
                out[i] += wave * envelope * pip.volume;
                pip.phase += pip.increment;
                if (pip.phase >= 1.0) {
                    pip.phase -= 1.0;
                }
            }
        }
        pips_.erase(
            std::remove_if(pips_.begin(),
                           pips_.end(),
                           [](const Pip &p) { return p.position >= p.total; }),
            pips_.end());
        for (int i = 0; i < frames; ++i) {
            out[i] = std::clamp(out[i], -1.0f, 1.0f);
        }
    }

    bool silent() const {
        return pips_.empty();
    }
    std::size_t active() const {
        return pips_.size();
    }

private:
    static constexpr std::size_t kMaxPips = 16;
    struct Pip {
        double phase = 0, increment = 0;
        int position = 0, total = 0;
        float volume = 0;
    };
    int rate_;
    std::vector<Pip> pips_;
};
