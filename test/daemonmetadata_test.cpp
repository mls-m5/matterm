#include "daemonclient.h"
#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"
#include "paneprobe.h"
#include <algorithm>

namespace {
struct FakeProc : frontend::ProcSource {
    std::optional<std::vector<std::string>> args;
    std::optional<std::string> cwd(int) const override {
        return std::nullopt;
    }
    std::optional<std::vector<std::string>> cmdline(int) const override {
        return args;
    }
};
} // namespace
TEST_SUIT_BEGIN(DaemonMetadata)
TEST_CASE("Unknown foreground is not classified as a shell or idle prompt") {
    FakeProc proc;
    EXPECT(!frontend::probePane(proc, 1, 1).foregroundBusy);
    proc.args = std::vector<std::string>{"/bin/sh"};
    EXPECT(frontend::probePane(proc, 1, 1).foregroundBusy == false);
    EXPECT(!frontend::probePane(proc, 1, 0).foregroundBusy);
    proc.args = std::vector<std::string>{"nvim", "file"};
    EXPECT(frontend::probePane(proc, 1, 2).foregroundBusy == true);
    proc.args.reset();
    const auto unreadable = frontend::probePane(proc, 1, 2);
    EXPECT(unreadable
               .busy); // Existing conservative close-prompt behavior remains.
    EXPECT(!unreadable.foregroundBusy);
}
TEST_CASE("Details fetch uses retained output without attachment or takeover") {
    Running r;
    EXPECT(r.started);
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    EXPECT(ctl->sendControl(
        mtd::CreateTerminal{"",
                            80,
                            24,
                            {"/bin/sh", "-c", "printf ready; exec sleep 30"},
                            "preview"}));
    const auto created = ctl->controlReply();
    EXPECT(created.has_value());
    const auto id = std::get<mtd::SessionCreated>(*created).id;
    std::optional<mtd::SessionDetail> detail;
    for (int i = 0; i < 200; ++i) {
        EXPECT(ctl->sendControl(mtd::GetSessionDetails{{id}, false}));
        const auto reply = ctl->controlReply();
        EXPECT(reply.has_value());
        const auto &rows = std::get<mtd::SessionDetails>(*reply);
        EXPECT(rows.processedCount == 1 && rows.rows.size() == 1);
        detail = rows.rows.front();
        if (detail->outputAgeMs)
            break;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT(detail && detail->outputAgeMs);
    EXPECT(detail->name == "preview");
    EXPECT(detail->foregroundBusy == true);
    EXPECT(!detail->preview);
    EXPECT(ctl->sendControl(mtd::GetSessionDetails{{id}, true}));
    const auto withPreview = ctl->controlReply();
    EXPECT(withPreview &&
           std::holds_alternative<mtd::SessionDetails>(*withPreview));
    EXPECT(std::get<mtd::SessionDetails>(*withPreview).rows.front().preview ==
           "ready");
    EXPECT(ctl->sendControl(mtd::GetSessionDetails{{id}, false}));
    const auto withoutPreview = ctl->controlReply();
    EXPECT(
        withoutPreview &&
        !std::get<mtd::SessionDetails>(*withoutPreview).rows.front().preview);
    EXPECT(ctl->sendControl(mtd::ListSessions{}));
    const auto list = ctl->controlReply();
    EXPECT(list.has_value());
    const auto &sessions = std::get<mtd::SessionList>(*list).sessions;
    EXPECT(sessions.size() == 1 && !sessions[0].attached &&
           sessions[0].outputEnd == 5);
    DaemonClient attached(r.path);
    EXPECT(attach(attached, id).has_value());
    EXPECT(attached.outputContains("ready"));
    for (int i = 0; i < 2; ++i) {
        EXPECT(ctl->sendControl(mtd::GetSessionDetails{{id}, false}));
        const auto reply = ctl->controlReply();
        EXPECT(reply && std::holds_alternative<mtd::SessionDetails>(*reply));
    }
    EXPECT(!attached.eof);
    EXPECT(attached.send(protocol::Ping{99}));
    const auto pong = attached.server();
    EXPECT(pong && std::holds_alternative<protocol::Pong>(*pong));
    auto web = exposedClient(r.path);
    EXPECT(web != nullptr);
    EXPECT(web->sendControl(mtd::GetSessionDetails{{id}, true}));
    const auto denied = web->controlReply();
    EXPECT(denied && std::holds_alternative<mtd::ControlError>(*denied));
}
TEST_CASE("Unavailable older daemon and disappeared ids have bounded "
          "compatible responses") {
    Running old(60, std::nullopt, {}, [](mtd::DaemonConfig &c) {
        c.version.additive = 4;
    });
    auto ctl = controlClient(old.path);
    EXPECT(ctl != nullptr);
    const std::string gone(32, 'a');
    EXPECT(ctl->sendControl(mtd::GetSessionDetails{{gone}, true}));
    const auto unsupported = ctl->controlReply();
    EXPECT(unsupported &&
           std::holds_alternative<mtd::ControlError>(*unsupported));
    Running current;
    auto modern = controlClient(current.path);
    EXPECT(modern != nullptr);
    EXPECT(modern->sendControl(mtd::GetSessionDetails{{gone}, true}));
    const auto missing = modern->controlReply();
    EXPECT(missing && std::holds_alternative<mtd::SessionDetails>(*missing));
    const auto &details = std::get<mtd::SessionDetails>(*missing);
    EXPECT(details.processedCount == 1 && details.rows.empty());
    EXPECT(modern->sendControl(mtd::ListSessions{}));
    const auto legacy = modern->controlReply();
    EXPECT(legacy && std::holds_alternative<mtd::SessionList>(*legacy));
}
TEST_CASE("Real metadata batches make bounded prefix progress below the 16KiB "
          "frame") {
    Running r;
    EXPECT(r.started);
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    std::string folder = r.dir;
    for (int i = 0; i < 9; ++i)
        folder += "/" + std::string(200, char('a' + i));
    std::filesystem::create_directories(folder);
    std::vector<std::string> ids;
    for (int i = 0; i < 16; ++i) {
        EXPECT(ctl->sendControl(mtd::CreateTerminal{
            folder, 80, 24, {"/bin/sleep", "30"}, std::string(64, 'n')}));
        const auto created = ctl->controlReply();
        EXPECT(created &&
               std::holds_alternative<mtd::SessionCreated>(*created));
        ids.push_back(std::get<mtd::SessionCreated>(*created).id);
    }
    std::size_t processed = 0;
    int batches = 0;
    while (processed < ids.size()) {
        const std::vector<std::string> remaining(
            ids.begin() + std::ptrdiff_t(processed), ids.end());
        EXPECT(ctl->sendControl(mtd::GetSessionDetails{remaining, false}));
        const auto reply = ctl->controlReply();
        EXPECT(reply && std::holds_alternative<mtd::SessionDetails>(*reply));
        const auto &details = std::get<mtd::SessionDetails>(*reply);
        EXPECT(details.processedCount > 0 &&
               details.processedCount <= remaining.size());
        EXPECT(details.rows.size() == details.processedCount);
        for (std::size_t i = 0; i < details.rows.size(); ++i) {
            EXPECT(details.rows[i].id == remaining[i]);
            EXPECT(details.rows[i].cwd == folder);
            EXPECT(!details.rows[i].preview);
        }
        std::string encoded;
        EXPECT(mtd::encode(*reply, encoded));
        EXPECT(encoded.size() <= mtd::kMaxFrame);
        processed += details.processedCount;
        ++batches;
    }
    EXPECT(batches > 1 && batches <= 16);
}
TEST_CASE("Separate control connections share the daemon decoding budget and "
          "off cannot refill it") {
    Running r;
    EXPECT(r.started);
    auto first = controlClient(r.path), second = controlClient(r.path);
    EXPECT(first && second);
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        const std::vector<std::string> command =
            i == 4
                ? std::vector<std::string>{"/bin/sh",
                                           "-c",
                                           "printf ready; exec sleep 30"}
                : std::vector<std::string>{
                      "python3",
                      "-c",
                      "import sys,time; sys.stdout.buffer.write(b'x'*16384); "
                      "sys.stdout.flush(); time.sleep(30)"};
        EXPECT(
            first->sendControl(mtd::CreateTerminal{"", 80, 24, command, ""}));
        const auto created = first->controlReply();
        EXPECT(created &&
               std::holds_alternative<mtd::SessionCreated>(*created));
        ids.push_back(std::get<mtd::SessionCreated>(*created).id);
    }
    bool ready = false;
    for (int attempt = 0; attempt < 300 && !ready; ++attempt) {
        EXPECT(first->sendControl(mtd::ListSessions{}));
        const auto reply = first->controlReply();
        EXPECT(reply && std::holds_alternative<mtd::SessionList>(*reply));
        ready =
            std::get<mtd::SessionList>(*reply).sessions.size() == ids.size();
        for (const auto &s : std::get<mtd::SessionList>(*reply).sessions) {
            const auto index =
                std::find(ids.begin(), ids.end(), s.id) - ids.begin();
            if (index < 5 && s.outputEnd != (index == 4 ? 5u : 16384u))
                ready = false;
        }
        if (!ready)
            std::this_thread::sleep_for(10ms);
    }
    EXPECT(ready);
    const std::vector<std::string> four(ids.begin(), ids.begin() + 4);
    const auto began = std::chrono::steady_clock::now();
    EXPECT(first->sendControl(mtd::GetSessionDetails{four, true}));
    const auto consumed = first->controlReply();
    EXPECT(consumed && std::holds_alternative<mtd::SessionDetails>(*consumed));
    EXPECT(std::get<mtd::SessionDetails>(*consumed).rows.size() == 4);
    for (const auto &s : std::get<mtd::SessionDetails>(*consumed).rows)
        EXPECT(s.preview && s.preview->size() == 256);
    EXPECT(second->sendControl(mtd::GetSessionDetails{{ids[4]}, true}));
    const auto blocked = second->controlReply();
    EXPECT(blocked && std::holds_alternative<mtd::SessionDetails>(*blocked));
    // Timing guard avoids claiming a fixed-window budget failure if a loaded
    // test host paused these real processes through an entire refill period.
    if (std::chrono::steady_clock::now() - began < 500ms)
        EXPECT(!std::get<mtd::SessionDetails>(*blocked).rows.front().preview);
    EXPECT(first->sendControl(mtd::GetSessionDetails{four, false}));
    const auto off = first->controlReply();
    EXPECT(off && std::holds_alternative<mtd::SessionDetails>(*off));
    for (const auto &s : std::get<mtd::SessionDetails>(*off).rows)
        EXPECT(!s.preview);
    std::this_thread::sleep_for(1100ms);
    EXPECT(second->sendControl(mtd::GetSessionDetails{{ids[4]}, true}));
    const auto refill = second->controlReply();
    EXPECT(refill && std::holds_alternative<mtd::SessionDetails>(*refill));
    EXPECT(std::get<mtd::SessionDetails>(*refill).rows.front().preview ==
           "ready");
}
TEST_SUIT_END
