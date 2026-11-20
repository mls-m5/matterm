#pragma once
#include <boost/asio/ip/address.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace server {
// Trust is an exact peer IP, not a client subnet. "none" disables forwarding;
// "any" is only safe with external network isolation, never on a public port.
class ProxySource {
public:
    explicit ProxySource(const std::string &trusted = "none",
                         unsigned hops = 0);
    bool trusts(boost::asio::ip::address peer) const;
    // Absent XFF uses the peer. A present malformed/ambiguous list from a
    // trusted peer is refused; headers from an untrusted peer are ignored.
    std::optional<boost::asio::ip::address> resolve(
        boost::asio::ip::address peer,
        std::optional<std::string_view> forwardedFor,
        bool duplicate = false) const;

private:
    std::optional<boost::asio::ip::address> trusted_;
    bool any_ = false;
    unsigned hops_;
};
} // namespace server
