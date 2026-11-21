#pragma once

// What a browser page remembers of its tabs and panes across a reload
// (docs/TABS_PLAN.md phase 3): the shape of every tab's pane tree, the split
// ratios and the server session id of each pane, as one short string for
// sessionStorage. Pure data and a strict parser: stored text is untrusted (an
// older page or another script may have written it), so anything that is not
// exactly this format, or breaks a limit, is refused as a whole.
//
//   text   := "v1" ";" active { ";" node }          one node per tab
//   node   := "L" id32                                 a pane: its session id
//           | "H" ratio "(" node "," node ")"          side by side, left first
//           | "V" ratio "(" node "," node ")"          stacked, upper first
//   id32   := 32 lower-case hex digits      ratio := 1..999 (per mille)
//
// `active` is the index of the active tab. The snapshot holds ids, never
// terminal output, titles or tokens.
#include "tabtypes.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frontend {

struct SnapshotNode {
    bool leaf = true;
    std::string session; // Leaf: the session id.
    SplitDirection direction = SplitDirection::SideBySide;
    int permille = 500; // Split: the first child's share.
    std::unique_ptr<SnapshotNode> first, second;

    static std::unique_ptr<SnapshotNode> pane(std::string id) {
        auto n = std::make_unique<SnapshotNode>();
        n->session = std::move(id);
        return n;
    }
    static std::unique_ptr<SnapshotNode> split(
        SplitDirection d,
        int permille,
        std::unique_ptr<SnapshotNode> a,
        std::unique_ptr<SnapshotNode> b) {
        auto n = std::make_unique<SnapshotNode>();
        n->leaf = false;
        n->direction = d;
        n->permille = permille;
        n->first = std::move(a);
        n->second = std::move(b);
        return n;
    }
};

struct LayoutSnapshot {
    static constexpr std::size_t kMaxTabs = 16;
    static constexpr std::size_t kMaxPanesPerTab = 8;
    static constexpr std::size_t kMaxPanes = 16; // In the whole window.
    static constexpr std::size_t kMaxText = 4096;

    std::size_t active = 0;
    std::vector<std::unique_ptr<SnapshotNode>> tabs;

    static bool validId(std::string_view id) {
        if (id.size() != 32) {
            return false;
        }
        for (const char c : id) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    static std::size_t paneCount(const SnapshotNode &n) {
        return n.leaf ? 1 : paneCount(*n.first) + paneCount(*n.second);
    }
    // The session ids of all panes, tab by tab, left/upper first.
    std::vector<std::string> sessions() const {
        std::vector<std::string> out;
        for (const auto &t : tabs) {
            collect(*t, out);
        }
        return out;
    }

    // Within the limits, valid ids, each id once, ratios in range.
    bool valid() const {
        if (tabs.empty() || tabs.size() > kMaxTabs || active >= tabs.size()) {
            return false;
        }
        std::size_t total = 0;
        std::vector<std::string> seen;
        for (const auto &t : tabs) {
            if (!t || !nodeValid(*t, 0, seen)) {
                return false;
            }
            const auto n = paneCount(*t);
            total += n;
            if (n > kMaxPanesPerTab || total > kMaxPanes) {
                return false;
            }
        }
        return true;
    }

    std::string serialize() const {
        std::string out = "v1;" + std::to_string(active);
        for (const auto &t : tabs) {
            out += ';';
            write(*t, out);
        }
        return out;
    }

    // Nothing for text that is not exactly this format or not valid().
    static std::optional<LayoutSnapshot> parse(std::string_view text) {
        if (text.size() > kMaxText || text.substr(0, 3) != "v1;") {
            return std::nullopt;
        }
        text.remove_prefix(3);
        LayoutSnapshot snapshot;
        std::size_t digits = 0;
        while (digits < text.size() && text[digits] >= '0' &&
               text[digits] <= '9') {
            ++digits;
        }
        if (digits == 0 || digits > 2) {
            return std::nullopt;
        }
        snapshot.active =
            std::size_t(std::stoi(std::string(text.substr(0, digits))));
        text.remove_prefix(digits);
        while (!text.empty()) {
            if (text.front() != ';' || snapshot.tabs.size() >= kMaxTabs) {
                return std::nullopt;
            }
            text.remove_prefix(1);
            auto node = readNode(text, 0);
            if (!node) {
                return std::nullopt;
            }
            snapshot.tabs.push_back(std::move(node));
        }
        if (!snapshot.valid()) {
            return std::nullopt;
        }
        return snapshot;
    }

private:
    static constexpr int kMaxDepth = 8;

    static void collect(const SnapshotNode &n, std::vector<std::string> &out) {
        if (n.leaf) {
            out.push_back(n.session);
        }
        else {
            collect(*n.first, out);
            collect(*n.second, out);
        }
    }
    static bool nodeValid(const SnapshotNode &n,
                          int depth,
                          std::vector<std::string> &seen) {
        if (depth > kMaxDepth) {
            return false;
        }
        if (n.leaf) {
            if (!validId(n.session)) {
                return false;
            }
            for (const auto &s : seen) {
                if (s == n.session) {
                    return false;
                }
            }
            seen.push_back(n.session);
            return true;
        }
        return n.permille >= 1 && n.permille <= 999 && n.first && n.second &&
               nodeValid(*n.first, depth + 1, seen) &&
               nodeValid(*n.second, depth + 1, seen);
    }
    static void write(const SnapshotNode &n, std::string &out) {
        if (n.leaf) {
            out += 'L';
            out += n.session;
            return;
        }
        out += n.direction == SplitDirection::SideBySide ? 'H' : 'V';
        out += std::to_string(n.permille);
        out += '(';
        write(*n.first, out);
        out += ',';
        write(*n.second, out);
        out += ')';
    }
    static std::unique_ptr<SnapshotNode> readNode(std::string_view &text,
                                                  int depth) {
        if (depth > kMaxDepth || text.empty()) {
            return nullptr;
        }
        const char kind = text.front();
        text.remove_prefix(1);
        if (kind == 'L') {
            if (text.size() < 32) {
                return nullptr;
            }
            auto node = SnapshotNode::pane(std::string(text.substr(0, 32)));
            text.remove_prefix(32);
            return validId(node->session) ? std::move(node) : nullptr;
        }
        if (kind != 'H' && kind != 'V') {
            return nullptr;
        }
        std::size_t digits = 0;
        while (digits < text.size() && text[digits] >= '0' &&
               text[digits] <= '9') {
            ++digits;
        }
        if (digits == 0 || digits > 3 || text.size() <= digits ||
            text[digits] != '(') {
            return nullptr;
        }
        const int permille = std::stoi(std::string(text.substr(0, digits)));
        text.remove_prefix(digits + 1);
        auto a = readNode(text, depth + 1);
        if (!a || text.empty() || text.front() != ',') {
            return nullptr;
        }
        text.remove_prefix(1);
        auto b = readNode(text, depth + 1);
        if (!b || text.empty() || text.front() != ')') {
            return nullptr;
        }
        text.remove_prefix(1);
        return SnapshotNode::split(kind == 'H' ? SplitDirection::SideBySide
                                               : SplitDirection::Stacked,
                                   permille,
                                   std::move(a),
                                   std::move(b));
    }
};

} // namespace frontend
