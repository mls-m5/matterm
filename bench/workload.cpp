#include "workload.h"

#include <limits>
#include <stdexcept>

std::string benchmark::workload(std::string_view name,
                                std::size_t approximateBytes) {
    std::string unit;
    if (name == "long") {
        unit = std::string(78, 'x') + "\r\n";
    }
    else if (name == "short") {
        unit = "short output line 0123456789\r\n";
    }
    else if (name == "color") {
        unit = "\x1b[31mred\x1b[32mgreen\x1b[38;2;100;150;200mRGB\x1b[0m "
               "normal\r\n";
    }
    else if (name == "redraw") {
        for (int row = 1; row <= 24; ++row) {
            unit += "\x1b[" + std::to_string(row) + ";1H" +
                    std::string(78, char('A' + row % 26));
        }
    }
    else {
        throw std::invalid_argument(
            "Workload must be long, short, color, or redraw");
    }
    if (approximateBytes > 256 * 1024 * 1024 || approximateBytes == 0) {
        throw std::invalid_argument("Workload bytes must be in 1..268435456");
    }
    std::string output;
    output.reserve(approximateBytes + unit.size());
    while (output.size() < approximateBytes) {
        output += unit;
    }
    return output; // Always end at a complete workload unit, never half a CSI.
}
