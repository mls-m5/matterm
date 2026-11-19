#include "config.h"
#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sodium.h>
#include <string_view>
#include <termios.h>
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
           "matterm-server --setup-credentials (alias --set-password) [--config PATH]\n"
           "  [--credentials PATH]   prompts twice on a terminal (empty = generate one), else reads "
           "stdin\n"
           "Settings come from built-in defaults, then the config file "
           "(default\n"
           "$XDG_CONFIG_HOME/matterm/server.json), then these flags. See "
           "docs/SERVER_CONFIG.md.\n";
}
std::string readLine(const char *prompt, bool hide) {
    termios saved{};
    const bool changed =
        hide && ::tcgetattr(0, &saved) == 0 &&
        [&] {
            auto quiet = saved;
            quiet.c_lflag &= ~tcflag_t(ECHO);
            return ::tcsetattr(0, TCSAFLUSH, &quiet) == 0;
        }();
    std::cerr << prompt << std::flush;
    std::string line;
    char c;
    while (std::cin.get(c) && c != '\n') {
        if (line.size() == 256)
            break;
        line += c;
    }
    if (changed)
        ::tcsetattr(0, TCSAFLUSH, &saved);
    if (hide)
        std::cerr << '\n';
    return line;
}
// 32 symbols from a 31-letter alphabet without look-alikes (about 158 bits),
// in four dash-separated groups so it is easy to copy by eye.
std::string generatePassword() {
    constexpr std::string_view alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    std::string password;
    for (int i = 0; i < 32; ++i) {
        if (i != 0 && i % 8 == 0)
            password += '-';
        password += alphabet[randombytes_uniform(alphabet.size())];
    }
    return password;
}
// Interactive: ask twice without echo. Piped: one line from stdin.
std::string readPassword() {
    if (::isatty(0)) {
        auto first = readLine(
            "Password (24-256 characters; leave empty to generate one): ", true);
        if (first.empty()) {
            auto generated = generatePassword();
            std::cerr << "Generated password (shown once, store it in a "
                         "password manager):\n";
            std::cout << generated << std::endl;
            return generated;
        }
        auto second = readLine("Again: ", true);
        const bool same = first == second;
        sodium_memzero(second.data(), second.size());
        if (!same) {
            sodium_memzero(first.data(), first.size());
            throw std::invalid_argument("The passwords differ");
        }
        return first;
    }
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
// Without static_dir, look for the web client built next to the server or in
// a repository build tree: <exe dir>/site, then build-web/site in the exe
// directory and up to five parents.
std::string findStaticDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
    auto dir = exe.parent_path();
    for (int level = 0; level < 6 && !dir.empty(); ++level) {
        for (const char *name : {"site", "build-web/site"}) {
            const auto candidate = dir / name;
            if (fs::exists(candidate / "index.html", ec))
                return candidate.string();
        }
        if (dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return {};
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
        if (settings.server.staticDir.empty()) {
            settings.server.staticDir = findStaticDir();
            if (!settings.server.staticDir.empty())
                std::clog << "matterm-server: serving the web client from "
                          << settings.server.staticDir << '\n';
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
