#pragma once
#include "ptyprocess.h"
#include "serversession.h"
#include <functional>
#include <memory>
#include <vector>

namespace server {
struct ProcessConfig {
    std::vector<std::string> arguments{"/bin/sh"};
    std::vector<std::string> environmentAllowList{
        "HOME", "USER", "LOGNAME", "PATH", "LANG", "LC_ALL", "LC_CTYPE"};
    std::chrono::milliseconds terminationGrace{500};
    std::chrono::milliseconds terminationDrainTimeout{1000};
};
class PtySessionHost : public protocol::SessionHost {
public:
    using Send = std::function<void(std::string)>;
    using Close = std::function<void(int)>;
    PtySessionHost(ProcessConfig config, Send send, Close close);
    void send(std::string bytes) override;
    void closeTransport(int code) override;
    bool startProcess(std::uint16_t columns, std::uint16_t rows) override;
    bool writeInput(std::string_view bytes) override;
    void resizeProcess(std::uint16_t columns,
                       std::uint16_t rows,
                       std::uint16_t pixelWidth,
                       std::uint16_t pixelHeight) override;
    void terminateProcess() override;
    // Bounded nonblocking I/O. Reports exit only after EOF, never just waitpid.
    void pump(protocol::ServerSession &session, protocol::Time now);

private:
    ProcessConfig config_;
    Send send_;
    Close close_;
    std::unique_ptr<PtyProcess> process_;
    std::string input_;
    std::optional<protocol::Time> killAt_;
    bool exitReported_ = false;
};
} // namespace server
