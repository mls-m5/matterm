#include "ptysession.h"

#include "ptylink.h"

#include "terminalmodel.h"
#include "vtparser.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace {
std::size_t validated(int value) {
    if (value <= 0 || value > 65535) {
        throw std::invalid_argument("Invalid PTY session size");
    }
    return std::size_t(value);
}
} // namespace

struct PtySession::Impl {
    TerminalModel model;
    PtyLink link;
    VtEvents events;
    std::function<void(std::string_view)> outputObserver;
    bool replyOverflow = false;
    VtParser parser;

    Impl(const std::vector<std::string> &arguments,
         int columns,
         int rows,
         VtEvents handlers,
         const std::string &term,
         PtyProcess::ColorCapability color)
        : model(validated(columns), validated(rows), 1000)
        , link(arguments, columns, rows, term, color)
        , events(std::move(handlers))
        , parser(model,
                 {[this](std::string_view bytes) {
                      if (!link.sendInput(bytes)) {
                          replyOverflow = true;
                      }
                      if (this->events.reply) {
                          this->events.reply(bytes);
                      }
                  },
                  [this](std::string_view title) {
                      if (this->events.title) {
                          this->events.title(title);
                      }
                  },
                  this->events.palette,
                  [this](std::size_t index, Rgb8 rgb) {
                      if (this->events.setPaletteColor) {
                          this->events.setPaletteColor(index, rgb);
                      }
                  },
                  [this](std::optional<std::size_t> index) {
                      if (this->events.resetPaletteColor) {
                          this->events.resetPaletteColor(index);
                      }
                  }}) {
        link.setEvents({[this](std::string_view bytes) {
                            if (outputObserver) {
                                outputObserver(bytes);
                            }
                            parser.feed(bytes);
                        },
                        {},
                        {}});
        link.setEndOfOutputHandler([this] { parser.finish(); });
        model.eraseDisplay(TerminalModel::EraseMode::All);
    }
    void pump(std::size_t readBudget, std::size_t writeBudget) {
        link.pump(readBudget, writeBudget);
        if (replyOverflow) {
            throw std::length_error("PTY reply queue limit exceeded");
        }
    }
};

PtySession::PtySession(const std::vector<std::string> &arguments,
                       int columns,
                       int rows,
                       VtEvents events,
                       const std::string &term,
                       PtyProcess::ColorCapability color)
    : impl_(std::make_unique<Impl>(
          arguments, columns, rows, std::move(events), term, color)) {}
PtySession::~PtySession() = default;
void PtySession::setOutputObserver(
    std::function<void(std::string_view)> observer) {
    impl_->outputObserver = std::move(observer);
}
TerminalModel &PtySession::model() noexcept {
    return impl_->model;
}
const TerminalModes &PtySession::modes() const noexcept {
    return impl_->parser.modes();
}
bool PtySession::send(std::string_view bytes) {
    return impl_->link.sendInput(bytes);
}
void PtySession::pump(std::size_t readBudget, std::size_t writeBudget) {
    impl_->pump(readBudget, writeBudget);
}
void PtySession::resize(int columns, int rows) {
    if (columns <= 0 || columns > 65535 || rows <= 0 || rows > 65535) {
        throw std::invalid_argument(
            "PTY session dimensions must be in 1..65535");
    }
    impl_->model.resize(std::size_t(columns), std::size_t(rows));
    impl_->link.resize(columns, rows);
}
bool PtySession::finished() const noexcept {
    return impl_->link.finished();
}
std::optional<PtyProcess::ExitStatus> PtySession::exitStatus() const noexcept {
    return impl_->link.exitStatus();
}
