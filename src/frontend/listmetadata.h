#pragma once

// Action-scoped, read-only list metadata collection. Socket I/O and a monotonic
// clock are injected: no terminal is attached and no background poller lives
// here. Prefix continuation handles the daemon's unchanged 16-KiB frame cap.
#include "ttylist.h"
#include "version.h"
#include <functional>

namespace frontend::ttymode {
struct ListMetadataTools {
    std::function<std::optional<mtd::ControlReply>(const mtd::ControlRequest &,
                                                   int)>
        ask;
    std::function<std::int64_t()> nowMs;
};

inline ListEntry detailedListEntry(const mtd::SessionEntry &listed,
                                   const mtd::SessionDetail &detail,
                                   const std::string &own,
                                   bool includePreview) {
    mtd::SessionInfo legacy;
    legacy.id = detail.id;
    legacy.name = detail.name;
    legacy.cwd = detail.cwd;
    legacy.busy = detail.foregroundBusy.value_or(false);
    legacy.command = detail.command;
    auto entry = makeListEntry(listed, &legacy, own);
    entry.foregroundBusy = detail.foregroundBusy;
    if (!detail.foregroundBusy)
        entry.what.clear();
    else if (*detail.foregroundBusy && detail.command.empty())
        entry.what = "program unknown";
    entry.outputAgeMs = detail.outputAgeMs;
    if (includePreview && detail.preview &&
        mtd::validPreviewText(*detail.preview)) {
        entry.preview = detail.preview;
    }
    return entry;
}

inline std::vector<ListEntry> readListMetadata(
    const std::vector<mtd::SessionEntry> &listed,
    const mtd::Version &version,
    bool includePreview,
    const std::string &own,
    const ListMetadataTools &tools,
    int budgetMs = 1500) {
    std::vector<ListEntry> result;
    result.reserve(listed.size());
    for (const auto &entry : listed)
        result.push_back(makeListEntry(entry, nullptr, own));
    if (!tools.ask || !tools.nowMs || version.format != 1 ||
        version.breaking != 3)
        return result;
    const auto started = tools.nowMs();
    const auto timeout = [&] {
        const auto elapsed = std::max<std::int64_t>(0, tools.nowMs() - started);
        return int(std::max<std::int64_t>(
            0, std::min<std::int64_t>(500, budgetMs - elapsed)));
    };
    if (version.additive < 5) {
        for (std::size_t index = 0; index < listed.size(); ++index) {
            const int left = timeout();
            if (left == 0)
                break;
            const auto answer =
                tools.ask(mtd::GetSessionInfo{listed[index].id}, left);
            const auto *info =
                answer ? std::get_if<mtd::SessionInfo>(&*answer) : nullptr;
            if (!info || info->id != listed[index].id)
                break;
            result[index] = makeListEntry(listed[index], info, own);
        }
        return result;
    }
    std::size_t offset = 0;
    while (offset < listed.size()) {
        const int left = timeout();
        if (left == 0)
            break;
        mtd::GetSessionDetails request;
        request.includePreview = includePreview;
        for (std::size_t i = offset;
             i < listed.size() && request.ids.size() < mtd::kMaxDetailIds;
             ++i) {
            request.ids.push_back(listed[i].id);
        }
        std::string encoded;
        if (!mtd::encode(mtd::ControlRequest(request), encoded))
            break;
        const auto answer = tools.ask(request, left);
        const auto *details =
            answer ? std::get_if<mtd::SessionDetails>(&*answer) : nullptr;
        if (!details || details->processedCount == 0 ||
            details->processedCount > request.ids.size() ||
            details->rows.size() > details->processedCount)
            break;
        const auto prefixEnd = request.ids.begin() + details->processedCount;
        std::vector<std::string> seen;
        bool valid = true;
        for (const auto &row : details->rows) {
            if (std::find(request.ids.begin(), prefixEnd, row.id) ==
                    prefixEnd ||
                std::find(seen.begin(), seen.end(), row.id) != seen.end()) {
                valid = false;
                break;
            }
            seen.push_back(row.id);
        }
        if (!valid)
            break; // Do not apply a partial hostile/ambiguous reply.
        const auto measuredAt = tools.nowMs();
        for (const auto &row : details->rows) {
            const auto found =
                std::find(request.ids.begin(), prefixEnd, row.id);
            const auto index =
                offset + std::size_t(found - request.ids.begin());
            result[index] =
                detailedListEntry(listed[index], row, own, includePreview);
            if (result[index].outputAgeMs)
                result[index].outputAgeMeasuredAtMs = measuredAt;
        }
        offset +=
            details->processedCount; // Includes requested IDs that disappeared.
    }
    return result;
}
} // namespace frontend::ttymode
