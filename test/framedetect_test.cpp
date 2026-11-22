#include "framedetect.h"
#include "mls-unit-test/unittest.h"

namespace {
using namespace frontend;

FrameProbe wayland() {
    FrameProbe p;
    p.videoDriver = "wayland";
    return p;
}
} // namespace

TEST_SUIT_BEGIN(FrameDetect)

TEST_CASE("Borders: zero on every side is no frame, any size is one, unknown is unknown; only on Wayland") {
    FrameProbe p = wayland();
    EXPECT(detectByBorders(p) == FrameState::Unknown); // SDL gave no answer.
    p.bordersKnown = true;
    EXPECT(detectByBorders(p) == FrameState::NoFrame);
    p.top = 37;
    EXPECT(detectByBorders(p) == FrameState::HasFrame);
    p = wayland();
    p.bordersKnown = true;
    p.left = 1;
    EXPECT(detectByBorders(p) == FrameState::HasFrame);
    p = FrameProbe{};
    p.videoDriver = "x11";
    p.bordersKnown = true;
    EXPECT(detectByBorders(p) == FrameState::Unknown); // A window manager may simply be absent.
    p.videoDriver = "dummy";
    EXPECT(detectByBorders(p) == FrameState::Unknown);
}

TEST_CASE("Plugins: a folder without a plugin is no frame, a plugin is one, no folder is unknown") {
    FrameProbe p = wayland();
    EXPECT(detectByPlugins(p) == FrameState::Unknown); // -1: nothing to look in.
    p.pluginFiles = 0;
    EXPECT(detectByPlugins(p) == FrameState::NoFrame);
    p.pluginFiles = 2;
    EXPECT(detectByPlugins(p) == FrameState::HasFrame);
    p.videoDriver = "x11";
    p.pluginFiles = 0;
    EXPECT(detectByPlugins(p) == FrameState::Unknown);
}

TEST_CASE("Message: only the libdecor line on Wayland says no frame; silence says nothing") {
    FrameProbe p = wayland();
    EXPECT(detectByMessage(p) == FrameState::Unknown);
    p.libdecorSaidNoPlugins = true;
    EXPECT(detectByMessage(p) == FrameState::NoFrame);
    p.videoDriver = "x11";
    EXPECT(detectByMessage(p) == FrameState::Unknown);
}

TEST_CASE("Any: a frame anywhere wins, otherwise one no-frame is enough") {
    FrameProbe p = wayland();
    EXPECT(decide(DetectVariant::Any, p) == FrameState::Unknown);
    p.libdecorSaidNoPlugins = true;
    EXPECT(decide(DetectVariant::Any, p) == FrameState::NoFrame);
    p.bordersKnown = true;
    p.top = 30; // SDL says there is a title bar although libdecor complained.
    EXPECT(decide(DetectVariant::Any, p) == FrameState::HasFrame);
    EXPECT(decide(DetectVariant::Off, p) == FrameState::Unknown);
    EXPECT(decide(DetectVariant::Message, p) == FrameState::NoFrame);
    EXPECT(decide(DetectVariant::Borders, p) == FrameState::HasFrame);
}

TEST_CASE("The variants have names and anything else is refused") {
    DetectVariant v = DetectVariant::Off;
    for (const char *name : {"off", "borders", "plugins", "message", "any"}) {
        EXPECT(parseDetectVariant(name, v));
    }
    EXPECT(v == DetectVariant::Any);
    for (const DetectVariant each : {DetectVariant::Off, DetectVariant::Borders, DetectVariant::Plugins,
                                     DetectVariant::Message, DetectVariant::Any}) {
        DetectVariant back = DetectVariant::Off;
        EXPECT(parseDetectVariant(detectVariantName(each), back) && back == each);
    }
    EXPECT(!parseDetectVariant("", v));
    EXPECT(!parseDetectVariant("Borders", v));
    EXPECT(std::string(frameStateName(FrameState::NoFrame)) == "no frame");
    EXPECT(noFrameNote("plugins").find("'plugins'") != std::string::npos);
    EXPECT(noFrameNote("plugins").find('\n') == std::string::npos);
}

TEST_SUIT_END
