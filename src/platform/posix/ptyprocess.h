#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Linux PTY/process ownership only. No terminal parsing, GUI, or input
// encoding. Prefer constructing before graphics/worker threads. Owns and reaps
// the child; do not waitpid it elsewhere. Destruction closes the PTY, sends
// HUP, then uses a bounded grace period before KILL/reaping if necessary.
class PtyProcess {
public:
    enum class ReadState { Data, WouldBlock, Closed };
    struct ReadResult {
        ReadState state;
        std::size_t count;
    };
    struct ExitStatus {
        int code = -1;
        int signal = 0;
    };

    PtyProcess(const std::vector<std::string> &arguments,
               int columns,
               int rows,
               const std::string &term = "vt100");
    ~PtyProcess();
    PtyProcess(const PtyProcess &) = delete;
    PtyProcess &operator=(const PtyProcess &) = delete;
    PtyProcess(PtyProcess &&) = delete;
    PtyProcess &operator=(PtyProcess &&) = delete;

    ReadResult read(char *buffer, std::size_t capacity);
    // Nonblocking: returns bytes accepted, possibly zero/partial. Caller queues
    // the remainder. CR/ETX etc. go to the tty driver, not simulated signals.
    std::size_t write(std::string_view bytes);
    void resize(int columns, int rows);
    std::optional<ExitStatus> pollExit();
    void terminate() noexcept;
    int nativeHandle() const noexcept;
    bool closed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
