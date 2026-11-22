#pragma once

#include "sessionsnapshot.h"

#include <optional>
#include <string>
#include <vector>

// Saved sessions as files (docs/TABS_PLAN.md section 9): one JSON file per name in
// one folder, owner only. The folder and the files are untrusted on the way back in:
// a file that is not ours, a link, too large or not exactly the schema is skipped.
namespace frontend {

struct SessionInfo {
    std::string name;
    std::int64_t saved = 0;
    std::size_t tabs = 0;
    std::size_t panes = 0;
};

class SessionFiles {
public:
    static constexpr std::size_t kMaxSessions = 256; // Listed at most.

    explicit SessionFiles(std::string directory) : directory_(std::move(directory)) {}
    // $XDG_CONFIG_HOME/matterm/sessions or ~/.config/matterm/sessions; empty if neither is known.
    static std::string defaultDirectory();
    // The same folder with "layouts" instead of "sessions": detached tabs (docs/TABS_PLAN.md s13).
    static std::string defaultLayoutDirectory();

    const std::string &directory() const { return directory_; }
    // Newest first; files that cannot be read or are not valid are left out.
    std::vector<SessionInfo> list() const;
    std::optional<SessionSnapshot> load(const std::string &name) const;
    // Validates, writes a temporary file (0600, exclusive), flushes it and renames it over the
    // old one: a crash leaves the old file or the new one, never half of one.
    bool save(const SessionSnapshot &snapshot, std::string *error = nullptr) const;
    bool remove(const std::string &name) const;

private:
    bool ensureDirectory(std::string *error) const;
    std::string pathOf(const std::string &name) const { return directory_ + "/" + name + ".json"; }
    std::string directory_;
};

} // namespace frontend
