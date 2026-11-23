#pragma once

// Keeps the settings of a running window current: reads the file once, then
// looks at it about every second (one stat; the file is read only when its time
// or size changed) and reloads it (docs/tasks/settings.md). A palette entry can
// reload at once. A file that is broken is reported and the values from before
// stay; a value that does not fit is a warning. Pure given a file system and a
// clock (milliseconds), so tests need neither files nor waiting. inotify can be
// added later; polling stays the baseline.
#include "settings.h"
#include "settingsfile.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace frontend::settings {

// The settings as they are in the file now, once (a program that only needs a
// value at start); the defaults when there is no file or it is broken.
inline Settings loadSettingsOnce(FileSystem &fs, const std::string &path) {
    std::string text;
    if (!fs.stat(path) || !fs.read(path, text)) {
        return Settings();
    }
    auto loaded = Settings::fromText(text);
    return loaded.ok() ? std::move(loaded.settings) : Settings();
}

class SettingsWatcher {
public:
    enum class Event {
        None,    // Nothing new.
        Changed, // New values (apply them).
        Failed,  // The file is broken: new message in error(), the old values
                 // stay.
    };
    using Clock = std::function<std::int64_t()>;
    static constexpr std::int64_t kPollMs = 1000;

    SettingsWatcher(FileSystem &fs, std::string path, Clock clock)
        : fs_(fs)
        , path_(std::move(path))
        , clock_(std::move(clock)) {
        lastPoll_ = clock_();
        load(false);
    }

    const Settings &settings() const {
        return settings_;
    }
    // The standing problem with the file ("" when it is fine).
    const std::string &error() const {
        return error_;
    }
    // Values of the last good read that did not fit (defaults were used for
    // them).
    const std::vector<std::string> &warnings() const {
        return warnings_;
    }
    const std::string &path() const {
        return path_;
    }

    // Call in every round of the window's loop; looks at the file at most once
    // a second.
    Event poll() {
        const auto now = clock_();
        if (now - lastPoll_ < kPollMs) {
            return Event::None;
        }
        lastPoll_ = now;
        if (fs_.stat(path_) == signature_) {
            return Event::None;
        }
        return load(false);
    }
    // At once, whatever the file looks like (the palette entry "Reload
    // settings").
    Event reloadNow() {
        lastPoll_ = clock_();
        return load(true);
    }

    // Writes `edited` (which should come from settings() with changes, so that
    // nothing else is lost). Refused while the file is broken: the user's text
    // is not replaced behind their back.
    bool save(const Settings &edited, std::string &error) {
        if (!error_.empty()) {
            error = "the settings file is broken (" + error_ +
                    "); fix or remove it first";
            return false;
        }
        if (path_.empty()) {
            error = "there is no place for a settings file (no HOME)";
            return false;
        }
        if (!fs_.writeAtomic(path_, edited.toText(), error)) {
            return false;
        }
        settings_ = edited;
        signature_ = fs_.stat(path_); // Our own write is not news.
        return true;
    }

private:
    Event load(bool explicitReload) {
        const auto signature = fs_.stat(path_);
        std::string text;
        if (signature && !fs_.read(path_, text)) {
            return fail("cannot read the file", signature, explicitReload);
        }
        signature_ = signature;
        if (!signature) { // No file: the defaults.
            const bool changed = !settings_.sameValues(Settings());
            settings_ = Settings();
            error_.clear();
            warnings_.clear();
            brokenSignature_.reset();
            return changed ? Event::Changed : Event::None;
        }
        auto loaded = Settings::fromText(text);
        if (!loaded.ok()) {
            return fail(loaded.error, signature, explicitReload);
        }
        const bool changed = !settings_.sameValues(loaded.settings);
        settings_ =
            std::move(loaded.settings); // Also when only unknown keys changed:
                                        // a save must keep the latest.
        warnings_ = std::move(loaded.warnings);
        error_.clear();
        brokenSignature_.reset();
        return changed ? Event::Changed : Event::None;
    }
    Event fail(const std::string &why,
               const std::optional<FileStat> &signature,
               bool explicitReload) {
        signature_ = signature;
        const bool again =
            brokenSignature_ && signature && *brokenSignature_ == *signature;
        error_ = why;
        brokenSignature_ = signature;
        return again && !explicitReload ? Event::None : Event::Failed;
    }

    FileSystem &fs_;
    std::string path_;
    Clock clock_;
    Settings settings_;
    std::vector<std::string> warnings_;
    std::string error_;
    std::optional<FileStat> signature_;
    std::optional<FileStat>
        brokenSignature_; // The file state that was reported as broken.
    std::int64_t lastPoll_ = 0;
};

} // namespace frontend::settings
