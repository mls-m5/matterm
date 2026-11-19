#pragma once

#include "cursorstate.h"

#include <algorithm>
#include <cmath>

// Presentation-only smoothing of the cursor between cell positions. The model
// keeps exact cell positions; the caller feeds the target cell and a clock each
// frame and gets back fractional offsets for the renderer. Nothing here knows
// about SDL, terminals, or time sources: `now` is any monotonic seconds value.
class CursorGlide {
public:
    struct Config {
        bool enabled = true;
        // Time to reach a new target. A new target during a glide starts from
        // the position currently shown, so motion stays continuous.
        double durationSeconds = 0.08;
        // Jumps farther than this (in cells) snap instead of gliding; screen
        // clears and full redraws move the cursor across the window.
        double maxGlideCells = 12;
    };

    CursorGlide() = default;
    explicit CursorGlide(Config config)
        : config_(config) {}

    // Call once per frame with the cursor's true cell, before blink masking.
    // `visible` false (hidden cursor, alternate-screen switch) snaps.
    void update(int column, int row, bool visible, double now) {
        const double targetColumn = column, targetRow = row;
        if (!config_.enabled || !visible || !placed_) {
            snapTo(targetColumn, targetRow, now);
            return;
        }
        if (targetColumn != targetColumn_ || targetRow != targetRow_) {
            // Where the cursor is drawn right now becomes the glide's start.
            const auto shown = position(now);
            if (std::hypot(shown.first - targetColumn,
                           shown.second - targetRow) > config_.maxGlideCells) {
                snapTo(targetColumn, targetRow, now);
                return;
            }
            startColumn_ = shown.first;
            startRow_ = shown.second;
            targetColumn_ = targetColumn;
            targetRow_ = targetRow;
            start_ = now;
        }
        now_ = now;
    }

    // Fractional offsets (in cells) to add to the target cell when drawing,
    // that is where the cursor is shown minus where it is going: zero when
    // settled.
    float columnOffset() const {
        return float(position(now_).first - targetColumn_);
    }
    float rowOffset() const {
        return float(position(now_).second - targetRow_);
    }
    bool animating() const {
        return config_.enabled && placed_ && now_ - start_ < duration();
    }
    // Fill the offsets of `cursor`; the cell fields stay the true position.
    void apply(CursorState &cursor) const {
        cursor.columnOffset = columnOffset();
        cursor.rowOffset = rowOffset();
    }
    bool enabled() const {
        return config_.enabled;
    }

private:
    double duration() const {
        return std::max(config_.durationSeconds, 1e-6);
    }
    void snapTo(double column, double row, double now) {
        placed_ = true;
        startColumn_ = targetColumn_ = column;
        startRow_ = targetRow_ = row;
        now_ = now;
        start_ = now - 1e6; // Long finished: not animating.
    }
    // Ease-out: fast at first, settling softly.
    std::pair<double, double> position(double now) const {
        const double t = std::clamp((now - start_) / duration(), 0.0, 1.0);
        const double eased = 1 - (1 - t) * (1 - t);
        return {startColumn_ + (targetColumn_ - startColumn_) * eased,
                startRow_ + (targetRow_ - startRow_) * eased};
    }

    Config config_;
    bool placed_ = false;
    double startColumn_ = 0, startRow_ = 0;
    double targetColumn_ = 0, targetRow_ = 0;
    double start_ = -1e9, now_ = 0;
};
