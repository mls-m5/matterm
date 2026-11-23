#include "daemonclient.h"
#include "daemonfixture.h"
#include "mls-unit-test/unittest.h"

#include <fstream>

TEST_SUIT_BEGIN(DaemonCreate)

TEST_CASE(
    "CreateTerminal round-trips and refuses what the daemon would not run") {
    mtd::CreateTerminal m;
    m.cwd = "/tmp";
    m.columns = 100;
    m.rows = 30;
    m.command = {"/bin/sh", "-c", "echo hi"};
    m.name = "build";
    std::string bytes;
    EXPECT(mtd::encode(mtd::ControlRequest(m), bytes));
    const auto back = mtd::decodeRequest(bytes);
    EXPECT(back.has_value() &&
           std::holds_alternative<mtd::CreateTerminal>(*back));
    const auto &got = std::get<mtd::CreateTerminal>(*back);
    EXPECT(got.cwd == "/tmp" && got.columns == 100 && got.rows == 30 &&
           got.command == m.command && got.name == "build");
    std::string out;
    mtd::CreateTerminal empty;
    EXPECT(mtd::encode(mtd::ControlRequest(empty),
                       out)); // No command: the user's shell.
    out.clear();
    mtd::CreateTerminal many;
    many.command.assign(33, "x");
    EXPECT(!mtd::encode(mtd::ControlRequest(many), out));
    mtd::CreateTerminal nul;
    nul.command = {std::string("a\0b", 3)};
    EXPECT(!mtd::encode(mtd::ControlRequest(nul), out));
    mtd::CreateTerminal blankProgram;
    blankProgram.command = {""};
    EXPECT(!mtd::encode(mtd::ControlRequest(blankProgram), out));
    mtd::CreateTerminal badName;
    badName.name = "a\nb";
    EXPECT(!mtd::encode(mtd::ControlRequest(badName), out));
    mtd::CreateTerminal zero;
    zero.columns = 0;
    EXPECT(!mtd::encode(mtd::ControlRequest(zero), out) ||
           !mtd::decodeRequest(out).has_value());
    EXPECT(!mtd::decodeRequest(bytes + "x").has_value());
}

TEST_CASE("Every terminal the daemon starts knows its own session id") {
    Running r;
    EXPECT(r.started);
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    const auto id = createSession(*ctl, "");
    EXPECT(id.has_value());
    DaemonClient c(r.path);
    EXPECT(attach(c, *id, 0).has_value());
    EXPECT(c.send(protocol::Input{"echo S=$MATTERM_SESSION=E\n"}));
    EXPECT(c.outputContains("S=" + *id + "=E"));
    // Also a terminal that a Hello creates.
    DaemonClient d(r.path);
    const auto w = attach(d);
    EXPECT(w.has_value());
    EXPECT(d.send(protocol::Input{"echo T=$MATTERM_SESSION=E\n"}));
    EXPECT(d.outputContains("T=" + w->sessionId + "=E"));
}

TEST_CASE("CreateTerminal runs the command in the folder, names the terminal, "
          "and the id is set") {
    Running r;
    EXPECT(r.started);
    std::string t =
        (std::filesystem::temp_directory_path() / "mtcreXXXXXX").string();
    const std::string dir = ::mkdtemp(t.data());
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    mtd::CreateTerminal m;
    m.cwd = dir;
    m.columns = 90;
    m.rows = 20;
    m.command = {
        "/bin/sh",
        "-c",
        "pwd; echo C=$MATTERM_SESSION=E; stty size > size.txt; sleep 20"};
    m.name = "work";
    EXPECT(ctl->sendControl(m));
    const auto reply = ctl->controlReply();
    EXPECT(reply.has_value() &&
           std::holds_alternative<mtd::SessionCreated>(*reply));
    const auto id = std::get<mtd::SessionCreated>(*reply).id;
    // Before anyone attaches (attaching says its own size): the size it started
    // with.
    std::this_thread::sleep_for(400ms);
    DaemonClient c(r.path);
    EXPECT(attach(c, id, 0).has_value());
    EXPECT(c.outputContains(dir));
    EXPECT(c.outputContains("C=" + id + "=E"));
    // The size it started with (before our attach said its own).
    std::string size;
    for (int i = 0; i < 100 && size.empty(); ++i) {
        std::ifstream in(dir + "/size.txt");
        std::getline(in, size);
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(size, std::string("20 90"));
    EXPECT(ctl->sendControl(mtd::ListSessions{}));
    const auto list = ctl->controlReply();
    EXPECT(list.has_value());
    bool named = false;
    for (const auto &s : std::get<mtd::SessionList>(*list).sessions) {
        named = named || (s.id == id && s.name == "work");
    }
    EXPECT(named);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A command that does not exist is StartFailed, and a web connection "
          "cannot create") {
    Running r;
    EXPECT(r.started);
    auto ctl = controlClient(r.path);
    EXPECT(ctl != nullptr);
    mtd::CreateTerminal m;
    m.command = {"/nonexistent/program"};
    EXPECT(ctl->sendControl(m));
    const auto failed = ctl->controlReply();
    EXPECT(failed.has_value() &&
           std::holds_alternative<mtd::ControlError>(*failed));
    EXPECT(std::get<mtd::ControlError>(*failed).code ==
           mtd::ControlErrorCode::StartFailed);
    auto web = exposedClient(r.path);
    EXPECT(web != nullptr);
    EXPECT(web->sendControl(mtd::CreateTerminal{}));
    const auto refused = web->controlReply();
    EXPECT(refused.has_value() &&
           std::holds_alternative<mtd::ControlError>(*refused));
}

TEST_CASE("createTerminal gives the id, and says why when it cannot") {
    Running r;
    EXPECT(r.started);
    std::string error;
    mtd::CreateTerminal m;
    m.name = "mine";
    const auto id = frontend::createTerminal(r.path, m, error);
    EXPECT(id.has_value());
    m.command = {"/nonexistent/program"};
    EXPECT(!frontend::createTerminal(r.path, m, error).has_value());
    EXPECT(!error.empty());
    EXPECT(!frontend::createTerminal(
                r.dir + "/none.sock", mtd::CreateTerminal{}, error)
                .has_value());
}

TEST_SUIT_END
