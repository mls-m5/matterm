#include "previewcache.h"
#include "control.h"
#include "tty/utf8.h"
#include "vtparser.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <list>
#include <map>
#include <sodium.h>
#include <utility>

namespace mtd {
namespace {
using protocol::Time;
struct Fragment {
    std::array<char32_t, kMaxPreviewScalars> cells{};
    std::size_t start = 0, count = 0;
    bool unsafe = false, hasText = false;
    void clear() {
        if (count || unsafe)
            sodium_memzero(cells.data(), sizeof cells);
        start = count = 0;
        unsafe = hasText = false;
    }
    void append(char32_t cp) {
        if (!validPreviewScalar(cp)) {
            unsafe = true;
            return;
        }
        if (count == cells.size())
            start = (start + 1) % cells.size();
        else
            ++count;
        cells[(start + count - 1) % cells.size()] = cp;
        if (cp != U' ')
            hasText = true;
    }
    std::optional<std::string> text() const {
        if (unsafe)
            return std::nullopt;
        std::size_t first = 0, last = count;
        while (first < last && cells[(start + first) % cells.size()] == U' ')
            ++first;
        while (last > first && cells[(start + last - 1) % cells.size()] == U' ')
            --last;
        std::string out;
        for (auto i = first; i < last; ++i)
            tty::appendUtf8(out, cells[(start + i) % cells.size()]);
        return out;
    }
};
struct Entry {
    Fragment current, previous;
    std::unique_ptr<VtParser> parser;
    std::uint64_t offset;
    Time touched;
    std::list<std::string>::iterator recency;
    Entry(std::uint64_t at, bool known, Time now)
        : offset(at)
        , touched(now) {
        parser = std::make_unique<VtParser>(VtParser::TextOnly{
            [this](std::u32string_view text) {
                for (auto cp : text) {
                    if (cp == U'\r' || cp == U'\n') {
                        if (current.hasText || current.unsafe)
                            previous = current;
                        current.clear();
                    }
                    else
                        current.append(cp == U'\t' ? U' ' : cp);
                }
            },
            known});
    }
    ~Entry() {
        current.clear();
        previous.clear();
    }
    std::optional<std::string> text() const {
        return current.hasText || current.unsafe ? current.text()
                                                 : previous.text();
    }
};
Time expiry(Time touched) {
    return touched > Time::max() - PreviewCache::kExpiry
               ? Time::max()
               : touched + PreviewCache::kExpiry;
}
} // namespace
struct PreviewCache::Impl {
    using Entries = std::map<std::string, std::unique_ptr<Entry>>;
    Entries entries;
    std::list<std::string>
        recency; // Request order, independent of clock precision.
    Entries::iterator erase(Entries::iterator it) {
        recency.erase(it->second->recency);
        return entries.erase(it);
    }
    std::optional<Time> budgetAt;
    std::size_t used = 0;
    std::uint64_t decoded = 0;
};
PreviewCache::PreviewCache()
    : impl_(std::make_unique<Impl>()) {}
PreviewCache::~PreviewCache() = default;
void PreviewCache::erase(const std::string &id) {
    const auto it = impl_->entries.find(id);
    if (it != impl_->entries.end())
        impl_->erase(it);
}
void PreviewCache::clear() {
    impl_->entries.clear();
    impl_->recency.clear();
} // Does not refill the global budget.
void PreviewCache::expire(Time now) {
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
        if (now < it->second->touched || now >= expiry(it->second->touched))
            it = impl_->erase(it);
        else
            ++it;
    }
}
std::optional<Time> PreviewCache::nextExpiry() const {
    std::optional<Time> next;
    for (const auto &item : impl_->entries) {
        const auto at = expiry(item.second->touched);
        if (!next || at < *next)
            next = at;
    }
    return next;
}
std::size_t PreviewCache::size() const {
    return impl_->entries.size();
}
std::uint64_t PreviewCache::decodedBytes() const {
    return impl_->decoded;
}
std::optional<std::string> PreviewCache::get(const std::string &id,
                                             std::uint64_t oldest,
                                             std::uint64_t end,
                                             const Read &read,
                                             Time now,
                                             bool includePreview) {
    expire(now);
    if (!includePreview || !read || oldest > end) {
        erase(id);
        return std::nullopt;
    }
    if (!impl_->budgetAt || now < *impl_->budgetAt ||
        now - *impl_->budgetAt >= Time(1000)) {
        impl_->budgetAt = now;
        impl_->used = 0;
    }
    auto found = impl_->entries.find(id);
    if (found != impl_->entries.end() &&
        (found->second->offset < oldest || found->second->offset > end ||
         end - found->second->offset > 2 * kReadBytes)) {
        impl_->erase(found);
        found = impl_->entries.end();
    }
    if (found == impl_->entries.end()) {
        if (impl_->entries.size() >= kMaxEntries) {
            impl_->erase(impl_->entries.find(impl_->recency.front()));
        }
        // A large backlog cannot catch a flood within the decode budget.
        // Destroy ALL old fragments/parser/UTF-8 state and scan only the most
        // recent bounded tail, conservatively unsynchronized. Even if offset
        // zero is retained, LF/BEL never make this arbitrary suffix safe.
        const auto at =
            oldest == 0 && end <= 2 * kReadBytes
                ? 0
                : std::max(oldest,
                           end - std::min<std::uint64_t>(end, kReadBytes));
        auto entry = std::make_unique<Entry>(at, at == 0, now);
        impl_->recency.push_back(id);
        entry->recency = std::prev(impl_->recency.end());
        try {
            found = impl_->entries.emplace(id, std::move(entry)).first;
        }
        catch (...) {
            impl_->recency.pop_back();
            throw;
        }
    }
    auto &entry = *found->second;
    entry.touched = now;
    impl_->recency.splice(impl_->recency.end(), impl_->recency, entry.recency);
    const auto available = kBytesPerSecond - impl_->used;
    const auto count = std::size_t(
        std::min<std::uint64_t>({end - entry.offset, kReadBytes, available}));
    if (count) {
        const auto bytes = read(entry.offset, count);
        if (!bytes || bytes->size() != count) {
            erase(id);
            return std::nullopt;
        }
        impl_->used += count;
        impl_->decoded += count;
        entry.parser->feed(*bytes);
        entry.offset += count;
    }
    if (!entry.parser->textSynchronized()) {
        entry.current.clear();
        entry.previous.clear();
        return std::nullopt;
    }
    if (entry.offset != end)
        return std::nullopt;
    return entry.text();
}
} // namespace mtd
