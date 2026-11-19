#pragma once

#include <functional>
#include <string_view>

// Views are valid only during the callback. Copy them before retaining them.
// Callbacks must not re-enter the parser. No OS actions are performed by it.
struct VtEvents {
    std::function<void(std::string_view)> reply;
    std::function<void(std::string_view)> title;
};
