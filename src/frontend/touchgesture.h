#pragma once

// Touch gesture classification for the browser client, free of SDL and the
// DOM so it can be unit-tested. Feed finger down/move/up events (positions in
// any consistent unit, times in milliseconds) and call tick() each frame; the
// returned actions say what the terminal should do:
//
//   one finger, moves past the slop   -> Scroll (pixels moved since the last
//                                        action; positive = finger moved down)
//   one finger, lifts without moving  -> Tap
//   one finger, held still            -> LongPress, then SelectExtend while it
//                                        moves and SelectEnd when it lifts
//   two fingers                       -> Pinch, in whole font steps
//
// A second finger turns any gesture into a pinch; after a pinch nothing else is
// reported until every finger has lifted.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace frontend {

class TouchGesture {
public:
    struct Config {
        double slop = 10; // Movement that is no longer a tap.
        std::uint64_t longPressMs = 450;
        double pixelsPerFontStep = 24; // Change in finger distance per step.
    };
    enum class Kind { Scroll, Tap, LongPress, SelectExtend, SelectEnd, Pinch };
    struct Action {
        Kind kind;
        double x = 0; // Position (LongPress, SelectExtend, Tap, SelectEnd).
        double y = 0;
        double amount = 0; // Scroll: pixels. Pinch: font steps (+ larger).
    };

    TouchGesture() = default;
    explicit TouchGesture(Config config)
        : config_(config) {}

    std::vector<Action> down(int id, double x, double y, std::uint64_t now) {
        std::vector<Action> out;
        if (!first_.active) {
            first_ = {true, id, x, y};
            start_ = {x, y};
            last_ = {x, y};
            downAt_ = now;
            state_ = State::Pending;
        }
        else if (!second_.active && id != first_.id) {
            second_ = {true, id, x, y};
            if (state_ == State::Selecting) {
                out.push_back({Kind::SelectEnd, last_.x, last_.y});
            }
            state_ = State::Pinching;
            baseline_ = distance();
        }
        return out;
    }

    std::vector<Action> move(int id, double x, double y, std::uint64_t) {
        std::vector<Action> out;
        if (first_.active && id == first_.id) {
            first_.x = x;
            first_.y = y;
        }
        else if (second_.active && id == second_.id) {
            second_.x = x;
            second_.y = y;
        }
        else {
            return out;
        }
        switch (state_) {
        case State::Pending:
            if (std::hypot(x - start_.x, y - start_.y) > config_.slop) {
                state_ = State::Scrolling;
                out.push_back({Kind::Scroll, x, y, y - last_.y});
                last_ = {x, y};
            }
            break;
        case State::Scrolling:
            if (y != last_.y) {
                out.push_back({Kind::Scroll, x, y, y - last_.y});
            }
            last_ = {x, y};
            break;
        case State::Selecting:
            last_ = {x, y};
            out.push_back({Kind::SelectExtend, x, y});
            break;
        case State::Pinching: {
            const double steps = std::trunc((distance() - baseline_) /
                                            config_.pixelsPerFontStep);
            if (steps != 0) {
                baseline_ += steps * config_.pixelsPerFontStep;
                out.push_back({Kind::Pinch, x, y, steps});
            }
            break;
        }
        case State::Idle:
        case State::Ignoring:
            break;
        }
        return out;
    }

    std::vector<Action> up(int id, std::uint64_t) {
        std::vector<Action> out;
        if (first_.active && id == first_.id) {
            if (state_ == State::Pending) {
                out.push_back({Kind::Tap, first_.x, first_.y});
            }
            else if (state_ == State::Selecting) {
                out.push_back({Kind::SelectEnd, first_.x, first_.y});
            }
            first_ = second_;
            second_ = {};
            // A remaining finger of a pinch must not start a scroll.
            state_ = first_.active ? State::Ignoring : State::Idle;
        }
        else if (second_.active && id == second_.id) {
            second_ = {};
            state_ = State::Ignoring;
        }
        if (!first_.active) {
            state_ = State::Idle;
        }
        return out;
    }

    // The finger stream was interrupted (focus lost, touch cancelled).
    std::vector<Action> cancel() {
        std::vector<Action> out;
        if (state_ == State::Selecting) {
            out.push_back({Kind::SelectEnd, last_.x, last_.y});
        }
        first_ = second_ = {};
        state_ = State::Idle;
        return out;
    }

    // Call every frame; reports a long press once its time has come.
    std::vector<Action> tick(std::uint64_t now) {
        std::vector<Action> out;
        if (state_ == State::Pending && now - downAt_ >= config_.longPressMs) {
            state_ = State::Selecting;
            out.push_back({Kind::LongPress, first_.x, first_.y});
        }
        return out;
    }

    // Time at which tick() will next have something to report, for loops that
    // sleep while idle; 0 when no long press is pending.
    std::uint64_t longPressDue() const noexcept {
        return state_ == State::Pending ? downAt_ + config_.longPressMs : 0;
    }
    // A gesture owns the touch: browser-synthesized mouse events are noise.
    bool active() const noexcept {
        return state_ != State::Idle;
    }

private:
    enum class State {
        Idle,
        Pending,
        Scrolling,
        Selecting,
        Pinching,
        Ignoring
    };
    struct Finger {
        bool active = false;
        int id = 0;
        double x = 0, y = 0;
    };
    struct Point {
        double x = 0, y = 0;
    };
    double distance() const {
        return std::hypot(first_.x - second_.x, first_.y - second_.y);
    }

    Config config_;
    State state_ = State::Idle;
    Finger first_, second_;
    Point start_, last_;
    std::uint64_t downAt_ = 0;
    double baseline_ = 0;
};

// Font size in CSS pixels, adjusted by pinch steps and kept in a usable range.
inline int steppedFontSize(int current,
                           int steps,
                           int smallest = 8,
                           int largest = 40) {
    return std::clamp(current + steps, smallest, largest);
}

// The renderer works in device pixels.
inline int fontDevicePixels(int cssPixels, double pixelRatio) {
    return std::clamp(
        int(std::lround(cssPixels * std::max(0.1, pixelRatio))), 8, 160);
}

} // namespace frontend
