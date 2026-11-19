#include "config.h"
#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void usage() {
    std::cout
        << "matterm-server [--config PATH] [--print-config] [--listen ADDR] "
           "[--port N]\n"
           "  [--origin URL] [--credentials PATH] [--static-dir DIR] "
           "[--allow-plain-http]\n"
           "  [-- PROGRAM ARG...]\n"
           "matterm-server --set-password [--config PATH] [--credentials PATH] "
           "< password-file\n"
           "Settings come from built-in defaults, then the config file "
           "(default\n"
           "$XDG_CONFIG_HOME/matterm/server.json), then these flags. See "
           "docs/SERVER_CONFIG.md.\n";
}
std::string readPassword() {
    std::string password;
    // Bounded stdin, never argv. Stop on LF; CR is a password byte.
    char c;
    while (std::cin.get(c) && c != '\n') {
        if (password.size() == 256)
            throw std::invalid_argument("Password too long");
        password += c;
    }
    return password;
}
bool exists(const std::string &path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (::geteuid() == 0)
            throw std::runtime_error("matterm-server refuses to run as root");
        server::initializeCrypto();
        const auto commandLine = server::parseCommandLine(
            std::vector<std::string>(argv + 1, argv + argc));
        if (commandLine.help) {
            usage();
            return 0;
        }
        auto settings = server::resolveSettings(commandLine, std::getenv);
        if (commandLine.setPassword) {
            if (settings.credentialsPath.empty())
                throw std::invalid_argument(
                    "--set-password needs --credentials PATH, a "
                    "\"credentials\" setting or a usable config directory");
            if (settings.credentialsDefaulted) {
                // The default location may not exist yet.
                const auto directory =
                    std::filesystem::path(settings.credentialsPath)
                        .parent_path();
                if (!exists(directory.string())) {
                    std::filesystem::create_directories(directory);
                    ::chmod(directory.c_str(), 0700);
                }
            }
            auto password = readPassword();
            server::savePassword(settings.credentialsPath, password);
            sodium_memzero(password.data(), password.size());
            return 0;
        }
        if (!settings.credentialsPath.empty()) {
            if (settings.credentialsDefaulted &&
                !exists(settings.credentialsPath))
                std::clog << "matterm-server: no credentials file at "
                          << settings.credentialsPath
                          << "; every login will fail (see --set-password)\n";
            else
                settings.server.passwordHash =
                    server::loadPasswordHash(settings.credentialsPath);
        }
        if (commandLine.printConfig) {
            std::cout << server::printConfig(settings);
            return 0;
        }
        boost::asio::io_context io;
        server::Server listener(io, std::move(settings.server));
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
