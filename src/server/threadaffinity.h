#pragma once
#include <atomic>
#include <thread>

namespace server {
// Debug aid for classes that are deliberately not thread safe. The first
// thread that calls check() becomes the owner; every other thread fails the
// check. Use as `assert(affinity_.check());` so release builds pay nothing.
class ThreadAffinity {
public:
    bool check() noexcept {
        const auto self = std::this_thread::get_id();
        auto expected = std::thread::id{};
        return owner_.compare_exchange_strong(expected, self) ||
               expected == self;
    }

private:
    std::atomic<std::thread::id> owner_{std::thread::id{}};
};
} // namespace server
