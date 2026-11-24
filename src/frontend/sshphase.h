#pragma once

// Optional, payload-free startup instrumentation. These are boundaries we can
// observe locally, not claims that DNS/authentication/remote work are
// separable.
#include <chrono>
#include <functional>
#include <string_view>

namespace frontend {
enum class SshPhase {
    Startup,
    Installation,
    LocalIdentity,
    RemoteInspection,
    Upload,
    Confirmation,
    InstallLinks,
    InstallCleanup,
    RemoteEnsure,
    ForwardSocket,
    ControlProbe
};
inline std::string_view sshPhaseName(SshPhase phase) {
    switch (phase) {
    case SshPhase::Startup:
        return "ssh_startup";
    case SshPhase::Installation:
        return "ssh_installation";
    case SshPhase::LocalIdentity:
        return "ssh_local_identity";
    case SshPhase::RemoteInspection:
        return "ssh_remote_inspection";
    case SshPhase::Upload:
        return "ssh_upload";
    case SshPhase::Confirmation:
        return "ssh_confirmation";
    case SshPhase::InstallLinks:
        return "ssh_install_links";
    case SshPhase::InstallCleanup:
        return "ssh_install_cleanup";
    case SshPhase::RemoteEnsure:
        return "ssh_remote_ensure";
    case SshPhase::ForwardSocket:
        return "ssh_forward_socket";
    case SshPhase::ControlProbe:
        return "ssh_control_probe";
    }
    return "ssh_unknown";
}
struct SshPhaseEvent {
    SshPhase phase;
    bool begin;
    double elapsedMs;
    bool success;
};
using SshPhaseCallback = std::function<void(const SshPhaseEvent &)>;
class SshPhaseScope {
public:
    SshPhaseScope(const SshPhaseCallback &callback, SshPhase phase)
        : callback_(callback)
        , phase_(phase) {
        if (callback_) {
            began_ = Clock::now();
            emit({phase_, true, 0, false});
        }
    }
    ~SshPhaseScope() {
        if (callback_)
            emit({phase_,
                  false,
                  std::chrono::duration<double, std::milli>(Clock::now() -
                                                            began_)
                      .count(),
                  success_});
    }
    SshPhaseScope(const SshPhaseScope &) = delete;
    SshPhaseScope &operator=(const SshPhaseScope &) = delete;
    void succeeded() {
        success_ = true;
    }

private:
    void emit(const SshPhaseEvent &event) noexcept {
        // Diagnostics must not alter connection/cleanup behavior, including
        // destructor unwinding if a caller's logging callback fails.
        try {
            callback_(event);
        }
        catch (...) {
        }
    }
    using Clock = std::chrono::steady_clock;
    SshPhaseCallback callback_;
    SshPhase phase_;
    Clock::time_point began_{};
    bool success_ = false;
};
} // namespace frontend
