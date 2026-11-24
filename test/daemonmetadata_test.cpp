#include "daemonclient.h"
#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"
#include "paneprobe.h"

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
TEST_SUIT_END
