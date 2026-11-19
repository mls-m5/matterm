#include "terminalmodel.h"
#include "vtparser.h"
#include "workload.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    try {
        const std::string name = argc > 1 ? argv[1] : "long";
        const auto requested =
            argc > 2 ? std::stoull(argv[2]) : 2 * 1024 * 1024;
        const auto chunk = argc > 3 ? std::stoull(argv[3]) : 4096;
        const int repetitions = argc > 4 ? std::stoi(argv[4]) : 5;
        if (argc > 5 || chunk == 0 || repetitions < 1 || repetitions > 100) {
            throw std::invalid_argument(
                "Usage: matterm-parser-bench [long|short|color|redraw] [bytes] "
                "[chunk] [repetitions]");
        }
        const auto bytes = benchmark::workload(name, std::size_t(requested));
        std::vector<double> samples;
        std::uint64_t checksum = 0;
        for (int run = -1; run < repetitions; ++run) {
            TerminalModel model(80, 24, 1000);
            VtParser parser(model);
            const auto begin = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < bytes.size();) {
                const auto count =
                    std::min(std::size_t(chunk), bytes.size() - i);
                parser.feed(std::string_view(bytes).substr(i, count));
                i += count;
            }
            parser.finish();
            const auto end = std::chrono::steady_clock::now();
            // Outside timing, but observable: retain the result of parsing
            // work.
            for (std::size_t y = 0; y < model.rows(); ++y) {
                for (std::size_t x = 0; x < model.columns(); ++x) {
                    const auto &cell = model.cell(x, y);
                    checksum = checksum * 131 + cell.codePoint +
                               unsigned(cell.attributes);
                    checksum = checksum * 131 + cell.foreground.r +
                               256u * cell.foreground.g +
                               65536u * cell.foreground.b;
                    checksum = checksum * 131 + cell.background.r +
                               256u * cell.background.g +
                               65536u * cell.background.b;
                }
            }
            checksum += model.historySize();
            if (run >= 0) {
                samples.push_back(
                    std::chrono::duration<double>(end - begin).count());
            }
        }
        std::sort(samples.begin(), samples.end());
        const double seconds = samples[samples.size() / 2];
        std::cout << "workload=" << name << " bytes=" << bytes.size()
                  << " chunk=" << chunk
                  << " grid=80x24 history=1000 repetitions=" << repetitions
                  << " warmup=1 tracing=off\n"
                  << "parser_model_median_ms=" << seconds * 1000
                  << " MiB_per_s="
                  << (bytes.size() / (1024.0 * 1024.0)) / seconds
                  << " checksum=" << checksum << '\n';
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
