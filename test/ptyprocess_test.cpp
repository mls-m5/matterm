#include "mls-unit-test/unittest.h"
#include "ptyprocess.h"

#include <array>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace {
std::string collect(PtyProcess &process, std::string_view until = {}) {
    std::string output;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<char, 4096> buffer{};
        const auto result = process.read(buffer.data(), buffer.size());
        output.append(buffer.data(), result.count);
        if (!until.empty() && output.find(until) != std::string::npos) {
            return output;
        }
        if (result.state == PtyProcess::ReadState::Closed &&
            process.pollExit()) {
            return output;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("PTY child timed out");
}
void send(PtyProcess &process, std::string_view bytes) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!bytes.empty() && std::chrono::steady_clock::now() < deadline) {
        bytes.remove_prefix(process.write(bytes));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!bytes.empty()) {
        throw std::runtime_error("PTY write timed out");
    }
}
} // namespace

TEST_SUIT_BEGIN(PtyProcess)

TEST_CASE("Child has a controlling tty on all stdio descriptors and initial "
          "winsize") {
    PtyProcess process({PTY_TEST_CHILD, "tty"}, 80, 24);
    const auto output = collect(process);
    EXPECT(output.find("TTY:80:24:vt100") != std::string::npos);
    EXPECT(process.closed());
    const auto status = process.pollExit();
    EXPECT(status.has_value());
    EXPECT_EQ(status->code, 7);
    EXPECT_EQ(status->signal, 0);
}

TEST_CASE(
    "Nonblocking writes reach the child through canonical terminal input") {
    PtyProcess process({PTY_TEST_CHILD, "echo"}, 80, 24);
    EXPECT(collect(process, "READY").find("READY") != std::string::npos);
    send(process, "hello\n");
    EXPECT(collect(process).find("READ:hello") != std::string::npos);
    EXPECT_EQ(process.pollExit()->code, 0);
}

TEST_CASE("Resize updates winsize and signals the foreground process group") {
    PtyProcess process({PTY_TEST_CHILD, "resize"}, 80, 24);
    collect(process, "READY");
    process.resize(120, 42);
    EXPECT(collect(process).find("SIZE:120:42") != std::string::npos);
    EXPECT_EQ(process.pollExit()->code, 0);
}

TEST_CASE(
    "Ctrl C is delivered by the tty driver rather than synthetic signals") {
    PtyProcess process({PTY_TEST_CHILD, "signal"}, 80, 24);
    collect(process, "READY");
    send(process, std::string_view("\x03", 1));
    EXPECT(collect(process).find("SIGINT") != std::string::npos);
    EXPECT_EQ(process.pollExit()->code, 0);
}

TEST_CASE("Idle reads would block and termination closes and reaps the child") {
    PtyProcess process({PTY_TEST_CHILD, "hold"}, 80, 24);
    collect(process, "READY");
    std::array<char, 100> buffer{};
    EXPECT(process.read(buffer.data(), buffer.size()).state ==
           PtyProcess::ReadState::WouldBlock);
    process.terminate();
    EXPECT(process.closed());
    EXPECT(process.pollExit().has_value());
    process.terminate();
}

TEST_CASE("Invalid startup and exec errors fail without leaking a child") {
    EXPECT_THROW((PtyProcess({}, 80, 24)), std::invalid_argument);
    EXPECT_THROW((PtyProcess({"/bin/sh"}, 0, 24)), std::invalid_argument);
    EXPECT_THROW((PtyProcess({"/bin/sh"}, 80, 65536)), std::invalid_argument);
    EXPECT_THROW((PtyProcess({"/matterm-nonexistent-program"}, 80, 24)),
                 std::system_error);
    EXPECT_THROW((PtyProcess({"/bin"}, 80, 24)), std::system_error);
}

TEST_SUIT_END
