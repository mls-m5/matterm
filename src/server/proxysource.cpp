#include "proxysource.h"
#include <stdexcept>
#include <vector>

namespace server {
namespace {
namespace ip = boost::asio::ip;
ip::address normalize(ip::address address) {
    if (address.is_v6() && address.to_v6().is_v4_mapped())
        return ip::make_address_v4(ip::v4_mapped, address.to_v6());
    return address;
}
std::optional<ip::address> parse(std::string_view text) {
    if (text.empty() || text.find('%') != std::string_view::npos)
        return {};
    boost::system::error_code ec;
    auto result = ip::make_address(std::string(text), ec);
    return ec ? std::nullopt : std::optional<ip::address>(normalize(result));
}
} // namespace
ProxySource::ProxySource(const std::string &trusted, unsigned hops)
    : hops_(hops) {
    if (hops > 16)
        throw std::invalid_argument("trusted_proxy_hops: must be in 0..16");
    if (trusted.empty() || trusted == "none")
        return;
    if (trusted == "any") {
        any_ = true;
        return;
    }
    trusted_ = parse(trusted);
    if (!trusted_)
        throw std::invalid_argument(
            "trusted_proxy: must be none, any, or an unscoped IP address");
}
bool ProxySource::trusts(ip::address peer) const {
    return any_ || (trusted_ && normalize(peer) == *trusted_);
}
std::optional<ip::address> ProxySource::resolve(
    ip::address peer,
    std::optional<std::string_view> forwardedFor,
    bool duplicate) const {
    if (!trusts(peer))
        return normalize(peer);
    if (duplicate)
        return {};
    if (!forwardedFor)
        return normalize(peer);
    auto text = *forwardedFor;
    if (text.size() > 8192)
        return {};
    std::vector<ip::address> addresses;
    while (true) {
        const auto comma = text.find(',');
        auto item = text.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.remove_suffix(1);
        const auto address = parse(item);
        if (!address || addresses.size() == 32)
            return {};
        addresses.push_back(*address);
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix(comma + 1);
    }
    if (addresses.size() <= hops_)
        return {};
    return addresses[addresses.size() - 1 - hops_];
}
} // namespace server
