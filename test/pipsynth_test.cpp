#include "mls-unit-test/unittest.h"
#include "pipsynth.h"

#include <cmath>
#include <vector>

// The pip synthesizer of the native demo, without an audio device.
TEST_SUIT_BEGIN(PipSynth)

TEST_CASE("Silence without pips, and after they are over") {
    PipSynth synth(8000);
    std::vector<float> out(800, 1.0f);
    synth.mix(out.data(), int(out.size()));
    for (const float v : out) {
        EXPECT(v == 0.0f);
    }
    EXPECT(synth.silent());
    synth.add(1000, 20, 0.5);
    EXPECT(!synth.silent());
    out.assign(800, 0.0f);
    synth.mix(out.data(),
              int(out.size())); // 100 ms: the 20 ms pip is long over.
    EXPECT(synth.silent());
    for (std::size_t i = 400; i < out.size(); ++i) {
        EXPECT(out[i] == 0.0f);
    }
}

TEST_CASE("A pip has the asked pitch, loudness and length") {
    PipSynth synth(48000);
    synth.add(1000, 100, 0.4);
    std::vector<float> out(48000 / 5, 0.0f); // 200 ms.
    synth.mix(out.data(), int(out.size()));
    int crossings = 0, lastNonZero = 0;
    float peak = 0;
    for (std::size_t i = 1; i < out.size(); ++i) {
        crossings +=
            (out[i - 1] < 0) != (out[i] < 0) && out[i - 1] != 0 && out[i] != 0;
        peak = std::max(peak, std::fabs(out[i]));
        if (out[i] != 0) {
            lastNonZero = int(i);
        }
    }
    EXPECT(crossings >= 190 &&
           crossings <= 210); // 1000 Hz for 100 ms: 200 crossings.
    EXPECT(peak > 0.2f && peak <= 0.4001f);
    EXPECT(lastNonZero >= 4700 &&
           lastNonZero <= 4900); // About 100 ms at 48 kHz.
}

TEST_CASE("The envelope avoids clicks at both ends") {
    PipSynth synth(48000);
    synth.add(700, 40, 0.5);
    std::vector<float> out(4800, 0.0f);
    synth.mix(out.data(), int(out.size()));
    EXPECT(std::fabs(out[0]) < 0.05f);
    int last = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] != 0)
            last = int(i);
    }
    EXPECT(std::fabs(out[std::size_t(last)]) < 0.05f);
}

TEST_CASE("Many pips at once stay within range, and the number is bounded") {
    PipSynth synth(8000);
    for (int i = 0; i < 100; ++i) {
        synth.add(500 + i * 10, 200, 1.0);
    }
    std::vector<float> out(1600, 0.0f);
    synth.mix(out.data(), int(out.size()));
    for (const float v : out) {
        EXPECT(v >= -1.0f && v <= 1.0f);
    }
    EXPECT(synth.active() <= 16);
}

TEST_CASE("Mixing in small pieces gives the same samples as in one") {
    PipSynth a(8000), b(8000);
    a.add(900, 60, 0.3);
    b.add(900, 60, 0.3);
    std::vector<float> whole(640, 0.0f), parts(640, 0.0f);
    a.mix(whole.data(), 640);
    for (int i = 0; i < 640; i += 64) {
        b.mix(parts.data() + i, 64);
    }
    for (std::size_t i = 0; i < whole.size(); ++i) {
        EXPECT(std::fabs(whole[i] - parts[i]) < 1e-6f);
    }
}

TEST_SUIT_END
