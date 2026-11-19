#include "server.h"
#include <boost/asio/signal_set.hpp>
#include <charconv>
#include <csignal>
#include <iostream>
#include <sodium.h>
#include <unistd.h>

namespace {
unsigned short portNumber(const std::string &text) {
    unsigned value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value > 65535)
        throw std::invalid_argument("Invalid port");
    return static_cast<unsigned short>(value);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (::geteuid() == 0)
            throw std::runtime_error("matterm-server refuses to run as root");
        server::initializeCrypto();
        server::ServerConfig config;
        std::string credentials;
        bool setPassword = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            auto value = [&]() -> std::string {
                if (++i == argc)
                    throw std::invalid_argument("Missing option value");
                return argv[i];
            };
            if (option == "--listen")
                config.address = value();
            else if (option == "--port")
                config.port = portNumber(value());
            else if (option == "--origin")
                config.allowedOrigin = value();
            else if (option == "--credentials")
                credentials = value();
            else if (option == "--static-dir")
                config.staticDir = value();
            else if (option == "--allow-plain-http")
                config.allowPlainHttp = true;
            else if (option == "--set-password")
                setPassword = true;
            else if (option == "--") {
                config.process.arguments.clear();
                for (++i; i < argc; ++i)
                    config.process.arguments.emplace_back(argv[i]);
                if (config.process.arguments.empty())
                    throw std::invalid_argument("Missing program after --");
                break;
            }
            else if (option == "--help") {
                std::cout << "matterm-server [--listen 127.0.0.1] [--port "
                             "8080] [--origin http://localhost:8080]\n"
                             "  [--credentials PATH] [--allow-plain-http] "
                             "[--static-dir DIR] [-- "
                             "PROGRAM ARG...]\n"
                             "matterm-server --set-password --credentials PATH "
                             "< password-file\n";
                return 0;
            }
            else
                throw std::invalid_argument("Unknown option");
        }
        if (setPassword) {
            if (credentials.empty())
                throw std::invalid_argument(
                    "--set-password requires --credentials PATH");
            std::string password;
            // Bounded stdin, never argv. Stop on LF; CR is a password byte.
            char c;
            while (std::cin.get(c) && c != '\n') {
                if (password.size() == 256)
                    throw std::invalid_argument("Password too long");
                password += c;
            }
            server::savePassword(credentials, password);
            sodium_memzero(password.data(), password.size());
            return 0;
        }
        config.passwordHash = server::loadPasswordHash(credentials);
        boost::asio::io_context io;
        server::Server listener(io, std::move(config));
        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait(
            [&io](const boost::system::error_code &, int) { io.stop(); });
        listener.start();
        std::cout << "matterm-server listening on port " << listener.port()
                  << std::endl;
        io.run();
    }
    catch (const std::exception &error) {
        // Messages from option/OS setup contain no request data or secrets.
        std::cerr << "matterm-server: " << error.what() << '\n';
        return 1;
    }
}
