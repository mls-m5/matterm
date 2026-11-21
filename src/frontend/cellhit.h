#pragma once

#include "terminalmodel.h"

#include <algorithm>
#include <optional>

namespace frontend {
// The text cell under a point, in device pixels relative to the surface the
// terminal is drawn on: the whole window, or one pane of it. The text is
// limited to the surface (a pane narrower than the model's columns shows only
// some). Without `clampToText` a point outside the text has no cell; with it,
// the nearest cell. Pure geometry, no SDL.
inline std::optional<TerminalModel::TextCell> cellAt(const TerminalModel &model,
                                                     double x,
                                                     double y,
                                                     int cellWidth,
                                                     int cellHeight,
                                                     double surfaceWidth,
                                                     double surfaceHeight,
                                                     bool clampToText) {
    const auto &viewport = model.viewport();
    const double textWidth =
        std::min(surfaceWidth, double(model.columns()) * cellWidth);
    const double textHeight =
        std::min(surfaceHeight, double(model.rows()) * cellHeight);
    if (cellWidth <= 0 || cellHeight <= 0 || textWidth < 1 || textHeight < 1 ||
        !viewport.rowCount() ||
        (!clampToText &&
         (x < 0 || x >= textWidth || y < 0 || y >= textHeight))) {
        return std::nullopt;
    }
    const auto column =
        std::size_t(std::clamp(x, 0.0, textWidth - 1) / cellWidth);
    const auto row = std::size_t((std::clamp(y, 0.0, textHeight - 1) -
                                  double(viewport.pixelOffset(cellHeight))) /
                                 cellHeight);
    return TerminalModel::TextCell{model.firstRetainedRow() +
                                       viewport.firstRow() +
                                       std::min(row, viewport.rowCount() - 1),
                                   column};
}
} // namespace frontend
