// chatd: the chat server. One shared room, WebSocket behind a TLS-terminating
// reverse proxy, messages only in RAM. See docs/CHAT.md.

#include "chatserver.h"

#include <boost/asio.hpp>
#include <charconv>
#include <csignal>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
void usage(std::ostream &out, const char *program) {
    out << "Usage: " << program << " --origin URL [OPTIONS]\n\n"
        << "The Mattrix chat server: one room, short messages, kept only in "
           "memory.\n\n"
           "  --help, -h              Show this help.\n"
           "  --listen HOST:PORT      WebSocket listener (default "
           "127.0.0.1:8097). Put a\n"
           "                          reverse proxy with TLS in front of it.\n"
           "  --origin URL            An allowed Origin, e.g. "
           "https://you.github.io\n"
           "                          (repeat for several; at least one is "
           "required).\n"
           "  --trust-proxy IP        Honor X-Forwarded-For from this peer "
           "(default: none).\n"
           "  --test-tcp HOST:PORT    ALSO listen for plain TCP lines "
           "(nc/telnet) on a\n"
           "                          LOOPBACK address, for local testing "
           "only.\n"
           "  --no-websocket          Do not listen for WebSocket (with "
           "--test-tcp).\n"
           "  --console               Read operator commands from standard "
           "input.\n";
}

bool splitHostPort(const std::string &text,
                   std::string &host,
                   std::uint16_t &port) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    host = text.substr(0, colon);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    unsigned value = 0;
    const auto *begin = text.data() + colon + 1;
    const auto *end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value > 65535) {
        return false;
    }
    port = std::uint16_t(value);
    return true;
}
} // namespace

int main(int argc, char **argv) {
    chatd::Options options;
    options.wsPort = 8097;
    bool console = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout, argv[0]);
            return 0;
        }
        else if (arg == "--listen" && i + 1 < argc) {
            if (!splitHostPort(argv[++i], options.wsHost, options.wsPort)) {
                std::cerr << "--listen needs HOST:PORT\n";
                return 1;
            }
        }
        else if (arg == "--origin" && i + 1 < argc) {
            options.origins.emplace_back(argv[++i]);
        }
        else if (arg == "--trust-proxy" && i + 1 < argc) {
            options.trustProxy = argv[++i];
        }
        else if (arg == "--test-tcp" && i + 1 < argc) {
            options.tcpEnabled = true;
            if (!splitHostPort(argv[++i], options.tcpHost, options.tcpPort)) {
                std::cerr << "--test-tcp needs HOST:PORT\n";
                return 1;
            }
        }
        else if (arg == "--no-websocket") {
            options.wsEnabled = false;
        }
        else if (arg == "--console") {
            console = true;
        }
        else {
            usage(std::cerr, argv[0]);
            return 1;
        }
    }
    try {
        boost::asio::io_context io;
        chatd::ChatServer server(io, options);
        server.start();
        std::cerr << "chatd: listening";
        if (options.wsEnabled) {
            std::cerr << " ws " << options.wsHost << ':' << server.wsPort();
        }
        if (options.tcpEnabled) {
            std::cerr << " tcp(TEST ONLY) " << options.tcpHost << ':'
                      << server.tcpPort();
        }
        std::cerr << '\n';
        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code &, int) {
            server.stop();
            io.stop();
        });
        boost::asio::posix::stream_descriptor input(io);
        boost::asio::streambuf lines;
        std::function<void()> readConsole = [&] {
            boost::asio::async_read_until(
                input,
                lines,
                '\n',
                [&](const boost::system::error_code &ec, std::size_t) {
                    if (ec) {
                        return;
                    }
                    std::istream stream(&lines);
                    std::string command;
                    std::getline(stream, command);
                    std::cout << server.console(command) << std::flush;
                    readConsole();
                });
        };
        if (console) {
            input.assign(::dup(STDIN_FILENO));
            std::cerr << "chatd: console on standard input (type help)\n";
            readConsole();
        }
        io.run();
    }
    catch (const std::exception &error) {
        std::cerr << "chatd: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
