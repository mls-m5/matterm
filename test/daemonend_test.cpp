#include "daemonclient.h"
#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"

#include <csignal>
#include <sys/wait.h>

namespace {
// A daemon that predates EndSession: answers the handshake, then BadMessage and
// hangs up.
struct OldDaemon {
    std::string path;
    pid_t child = -1;
    explicit OldDaemon(const std::string &socketPath)
        : path(socketPath) {
        child = ::fork();
        if (child == 0) {
            const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
            (void)::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
            (void)::listen(fd, 4);
            for (;;) {
                const int c = ::accept(fd, nullptr, nullptr);
                if (c < 0) {
                    continue;
                }
                char buffer[256];
                (void)::read(
                    c, buffer, sizeof buffer); // The hello and the request it
                                               // does not know.
                std::string out, frame;
                mtd::ControlWelcome welcome;
                welcome.build = "matterm-daemon";
                mtd::encode(mtd::ControlReply(welcome), frame);
                mtd::encodeFrame(frame, out);
                frame.clear();
                mtd::encode(mtd::ControlReply(mtd::ControlError{
                                mtd::ControlErrorCode::BadMessage,
                                "bad control message"}),
                            frame);
                mtd::encodeFrame(frame, out);
                (void)!::write(c, out.data(), out.size());
                ::close(c);
            }
        }
        for (int i = 0; i < 200 && ::access(path.c_str(), F_OK) != 0; ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }
    ~OldDaemon() {
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        ::unlink(path.c_str());
    }
};

bool listed(const std::string &socket, const std::string &id) {
    std::string error;
    const auto reply = frontend::askDaemon(socket, mtd::ListSessions{}, error);
    if (!reply || !std::holds_alternative<mtd::SessionList>(*reply)) {
        return false;
    }
    for (const auto &s : std::get<mtd::SessionList>(*reply).sessions) {
        if (s.id == id) {
            return true;
        }
    }
    return false;
}

bool goneSoon(const std::string &socket, const std::string &id) {
    for (int i = 0; i < 100; ++i) {
        if (!listed(socket, id)) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}
} // namespace

TEST_SUIT_BEGIN(DaemonEnd)

TEST_CASE("EndSession round-trips") {
    std::string bytes;
    EXPECT(mtd::encode(mtd::ControlRequest(mtd::EndSession{"abc"}), bytes));
    const auto request = mtd::decodeRequest(bytes);
    EXPECT(request.has_value() &&
           std::get<mtd::EndSession>(*request).id == "abc");
    EXPECT(!mtd::decodeRequest(bytes + "x").has_value());
    EXPECT(!mtd::decodeRequest(std::string("\x4b\0\0", 3))
                .has_value()); // An empty id is no id.
    bytes.clear();
    EXPECT(mtd::encode(mtd::ControlReply(mtd::SessionEnded{"abc"}), bytes));
    const auto reply = mtd::decodeReply(bytes);
    EXPECT(reply.has_value() &&
           std::get<mtd::SessionEnded>(*reply).id == "abc");
}

TEST_CASE("An idle shell that nobody is attached to is ended and gone from the "
          "list") {
    Running r;
    EXPECT(r.started);
    std::string error;
    const auto id =
        frontend::createTerminal(r.path, mtd::CreateTerminal{}, error);
    EXPECT(id.has_value());
    EXPECT(listed(r.path, *id));
    EXPECT(frontend::endSession(r.path, *id, error));
    EXPECT(goneSoon(r.path, *id));
}

TEST_CASE("A busy program is hung up on, and a client attached to it sees the "
          "program end") {
    Running r;
    EXPECT(r.started);
    std::string error;
    mtd::CreateTerminal m;
    m.command = {"/bin/sh", "-c", "echo busy; exec sleep 600"};
    const auto id = frontend::createTerminal(r.path, m, error);
    EXPECT(id.has_value());
    DaemonClient c(r.path);
    EXPECT(attach(c, *id, 0).has_value());
    EXPECT(c.outputContains("busy"));
    EXPECT(frontend::endSession(r.path, *id, error));
    bool exited = false;
    for (int i = 0; i < 100 && !exited; ++i) {
        const auto message = c.server(200);
        if (message) {
            if (const auto *output = std::get_if<protocol::Output>(&*message)) {
                c.send(protocol::Ack{output->offset + output->data.size()});
            }
            exited = std::holds_alternative<protocol::Exit>(*message);
        }
    }
    EXPECT(exited);
    EXPECT(goneSoon(r.path, *id));
}

TEST_CASE("A program that ignores the hang-up is killed after the grace") {
    Running r(60, std::nullopt, {}, [](mtd::DaemonConfig &c) {
        c.process.terminationGrace = std::chrono::milliseconds(200);
    });
    EXPECT(r.started);
    std::string error;
    mtd::CreateTerminal m;
    m.command = {
        "/bin/sh", "-c", "trap '' HUP; echo up; while :; do sleep 1; done"};
    const auto id = frontend::createTerminal(r.path, m, error);
    EXPECT(id.has_value());
    std::this_thread::sleep_for(300ms);
    EXPECT(frontend::endSession(r.path, *id, error));
    EXPECT(goneSoon(r.path, *id));
}

TEST_CASE(
    "An unknown id is UnknownSession, and a session cannot be ended twice") {
    Running r;
    EXPECT(r.started);
    std::string error;
    EXPECT(!frontend::endSession(
        r.path, "0123456789abcdef0123456789abcdef", error));
    EXPECT(error.find("no such") != std::string::npos);
    const auto id =
        frontend::createTerminal(r.path, mtd::CreateTerminal{}, error);
    EXPECT(id.has_value());
    EXPECT(frontend::endSession(r.path, *id, error));
    EXPECT(goneSoon(r.path, *id));
    EXPECT(!frontend::endSession(r.path, *id, error));
}

TEST_CASE("A web connection cannot end anything") {
    Running r;
    EXPECT(r.started);
    std::string error;
    const auto id =
        frontend::createTerminal(r.path, mtd::CreateTerminal{}, error);
    EXPECT(id.has_value());
    auto web = exposedClient(r.path);
    EXPECT(web != nullptr);
    EXPECT(web->sendControl(mtd::EndSession{*id}));
    const auto refused = web->controlReply();
    EXPECT(refused.has_value() &&
           std::holds_alternative<mtd::ControlError>(*refused));
    EXPECT(listed(r.path, *id));
}

TEST_CASE("An older daemon is named as the reason") {
    std::string t =
        (std::filesystem::temp_directory_path() / "mtendXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    OldDaemon old(dir + "/old.sock");
    std::string error;
    EXPECT(!frontend::endSession(
        old.path, "0123456789abcdef0123456789abcdef", error));
    EXPECT(error.find("older than") != std::string::npos);
    EXPECT(error.find("cannot end a terminal") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_SUIT_END
