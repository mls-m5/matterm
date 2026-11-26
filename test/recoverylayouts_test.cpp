#include "mls-unit-test/unittest.h"
#include "recoverylayouts.h"
#include <sys/wait.h>

namespace {
frontend::SavedTab tab() {
    frontend::SavedTab t;
    t.title = "work";
    frontend::SavedPane pane;
    pane.terminal = std::string(32, 'a');
    t.root = frontend::SavedNode::leafOf(std::move(pane));
    return t;
}
} // namespace
TEST_SUIT_BEGIN(RecoveryLayouts)
TEST_CASE(
    "Checkpoints survive SIGKILL but live windows and other hosts are hidden") {
    char directory[] = "/tmp/matterm-recovery-XXXXXX";
    EXPECT(::mkdtemp(directory) != nullptr);
    int pipe[2];
    EXPECT(::pipe2(pipe, O_CLOEXEC) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(pipe[0]);
        frontend::RecoveryLayouts r(directory, "ssh:host");
        std::set<std::string> current;
        std::string error;
        const char ok = r.checkpoint(1, tab(), current, error) ? 'y' : 'n';
        ::write(pipe[1], &ok, 1);
        for (;;)
            ::pause();
    }
    ::close(pipe[1]);
    char ok = 0;
    EXPECT(::read(pipe[0], &ok, 1) == 1 && ok == 'y');
    ::close(pipe[0]);
    frontend::RecoveryLayouts reader(directory, "ssh:host");
    const auto files = reader.files().list();
    EXPECT(files.size() == 1);
    EXPECT(!reader.available(files.front().name));
    frontend::RecoveryLayouts other(directory, "ssh:other");
    EXPECT(other.files().list().empty());
    ::kill(child, SIGKILL);
    int status;
    EXPECT(::waitpid(child, &status, 0) == child);
    EXPECT(reader.available(files.front().name));
    const auto saved = reader.files().load(files.front().name);
    EXPECT(saved && saved->tabs[0].title == "work");
    EXPECT(saved && saved->tabs[0].root->pane.terminal == std::string(32, 'a'));
    std::filesystem::remove_all(directory);
}
TEST_CASE("Unchanged layouts do not rewrite files; explicit close retires "
          "checkpoints") {
    char directory[] = "/tmp/matterm-recovery-XXXXXX";
    EXPECT(::mkdtemp(directory) != nullptr);
    frontend::RecoveryLayouts r(directory, "local:socket");
    std::string error;
    std::set<std::string> current;
    EXPECT(r.checkpoint(7, tab(), current, error));
    const auto name = r.files().list().front().name;
    const auto path = r.files().directory() + "/" + name + ".json";
    const auto timestamp = std::filesystem::last_write_time(path);
    EXPECT(r.checkpoint(7, tab(), current, error));
    EXPECT(std::filesystem::last_write_time(path) == timestamp);
    r.retire();
    EXPECT(r.files().list().empty());
    std::filesystem::remove_all(directory);
}
TEST_SUIT_END
