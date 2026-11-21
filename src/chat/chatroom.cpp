#include "chatroom.h"

#include <algorithm>
#include <sstream>

namespace chat {
namespace {

bool parseMinutes(const std::string &s, Millis &out) {
    if (s.empty() || s.size() > 6) {
        return false;
    }
    Millis v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    out = v * 60 * 1000;
    return v > 0;
}

} // namespace

ChatRoom::ChatRoom(RoomConfig config)
    : config_(config) {
    ring_.reserve(config_.ringSlots);
}

void ChatRoom::send(Actions &out, ConnId id, const ServerLine &line) {
    out.push_back({id, formatServerLine(line), false});
}

void ChatRoom::sendError(Actions &out,
                         ConnId id,
                         ErrorCode code,
                         const char *text) {
    ServerLine l;
    l.kind = ServerLine::Kind::Error;
    l.code = code;
    l.text = text;
    send(out, id, l);
}

void ChatRoom::sendAll(Actions &out, const ServerLine &line, ConnId except) {
    const std::string frame = formatServerLine(line);
    for (const auto &[id, c] : conns_) {
        if (c.joined && id != except) {
            out.push_back({id, frame, false});
        }
    }
}

std::size_t ChatRoom::joinedCount() const {
    std::size_t n = 0;
    for (const auto &entry : conns_) {
        n += entry.second.joined;
    }
    return n;
}

ConnId ChatRoom::findByName(const std::string &name) const {
    const std::string key = nameSkeleton(name);
    for (const auto &[id, c] : conns_) {
        if (c.joined && nameSkeleton(c.name) == key) {
            return id;
        }
    }
    return 0;
}

void ChatRoom::clearDraft(Actions &out, ConnId id, Conn &c) {
    c.draft.clear();
    c.dirty = false;
    if (c.shown) {
        c.shown = false;
        ServerLine gone;
        gone.kind = ServerLine::Kind::Gone;
        gone.name = c.name;
        sendAll(out, gone, id);
    }
}

void ChatRoom::drop(Actions &out, ConnId id, Millis, bool announce) {
    const auto it = conns_.find(id);
    if (it == conns_.end()) {
        return;
    }
    Conn &c = it->second;
    if (auto s = perSource_.find(c.source); s != perSource_.end()) {
        if (--s->second == 0) {
            perSource_.erase(s);
        }
    }
    const bool wasJoined = c.joined;
    const std::string name = c.name;
    const bool wasShown = c.shown;
    conns_.erase(it);
    if (wasJoined && announce) {
        if (wasShown) {
            ServerLine gone;
            gone.kind = ServerLine::Kind::Gone;
            gone.name = name;
            sendAll(out, gone, id);
        }
        ServerLine leave;
        leave.kind = ServerLine::Kind::Leave;
        leave.name = name;
        sendAll(out, leave, id);
    }
}

namespace {
void disconnectWith(Actions &out, ConnId id, const std::string &errorFrame) {
    if (!errorFrame.empty()) {
        out.push_back({id, errorFrame, false});
    }
    out.push_back({id, {}, true});
}
} // namespace

void ChatRoom::violation(Actions &out, ConnId id, Millis now, const char *) {
    const auto it = conns_.find(id);
    if (it == conns_.end()) {
        return;
    }
    Conn &c = it->second;
    ++violations_;
    if (now - c.violationStart > config_.violationWindow) {
        c.violations = 0;
        c.violationStart = now;
    }
    if (++c.violations >= config_.violationsToKick) {
        setUntil(bans_, c.source, now + config_.kickBan);
        ServerLine e;
        e.kind = ServerLine::Kind::Error;
        e.code = ErrorCode::Banned;
        e.text = "too many mistakes, try again later";
        disconnectWith(out, id, formatServerLine(e));
        drop(out, id, now, true);
    }
}

bool ChatRoom::takeToken(Bucket &b, double burst, Millis refill, Millis now) {
    b.tokens = std::min(burst, b.tokens + double(now - b.at) / double(refill));
    b.at = now;
    if (b.tokens < 1.0) {
        return false;
    }
    b.tokens -= 1.0;
    return true;
}

// A bounded table: when it is full the entry that ends first makes room.
void ChatRoom::setUntil(std::map<std::string, Millis> &table,
                        const std::string &source,
                        Millis until) {
    if (table.size() >= config_.maxTableEntries && !table.count(source)) {
        auto first = table.begin();
        for (auto it = table.begin(); it != table.end(); ++it) {
            if (it->second < first->second) {
                first = it;
            }
        }
        table.erase(first);
    }
    table[source] = until;
}

void ChatRoom::expireRing(Millis now) {
    std::size_t drop = 0;
    while (drop < ring_.size() && now - ring_[drop].time >= config_.ttl) {
        ++drop;
    }
    if (drop > 0) {
        ring_.erase(ring_.begin(), ring_.begin() + std::ptrdiff_t(drop));
    }
}

bool ChatRoom::lockBubble(
    Actions &out, ConnId id, Conn &c, Millis now, bool explicitLock) {
    const std::string text = trimSpaces(c.draft);
    if (text.empty() || !checkMessage(text).ok()) {
        clearDraft(out, id, c);
        return true;
    }
    const auto mute = mutes_.find(c.source);
    if (mute != mutes_.end() && mute->second > now) {
        c.bubbleLocked = true;
        clearDraft(out, id, c);
        return true;
    }
    // The room as a whole has a budget too.
    recentMessages_.erase(
        std::remove_if(recentMessages_.begin(),
                       recentMessages_.end(),
                       [&](Millis t) { return now - t >= 60 * 1000; }),
        recentMessages_.end());
    // The room's cap is nobody's mistake: no violation for hitting it.
    if (recentMessages_.size() >= config_.messagesPerMinute) {
        sendError(out, id, ErrorCode::Rate, "the room is busy, try again");
        return false;
    }
    // The rate is per source, whatever the number of its connections.
    auto bucket = messageBuckets_.try_emplace(c.source, Bucket{0.0, now}).first;
    if (!takeToken(
            bucket->second, config_.messageBurst, config_.messageRefill, now)) {
        sendError(out, id, ErrorCode::Rate, "slow down");
        if (explicitLock) {
            violation(out, id, now, "rate");
        }
        return false;
    }
    recentMessages_.push_back(now);
    expireRing(now);
    if (ring_.size() >= config_.ringSlots && !ring_.empty()) {
        ring_.erase(ring_.begin());
    }
    Entry entry;
    entry.serial = nextSerial_++;
    entry.time = now;
    entry.name = c.name;
    entry.text = text;
    ring_.push_back(entry);
    ++messages_;
    ServerLine msg;
    msg.kind = ServerLine::Kind::Msg;
    msg.serial = entry.serial;
    msg.name = c.name;
    msg.text = text;
    sendAll(out, msg);
    ServerLine ack;
    ack.kind = ServerLine::Kind::Ack;
    ack.bubble = c.bubble;
    ack.serial = entry.serial;
    ack.text = text;
    send(out, id, ack);
    c.bubbleLocked = true;
    clearDraft(out, id, c);
    return true;
}

void ChatRoom::join(
    Actions &out, ConnId id, Conn &c, const std::string &name, Millis now) {
    c.joined = true;
    c.name = name;
    c.joinedAt = now;
    messageBuckets_.try_emplace(c.source,
                                Bucket{double(config_.messageBurst), now});
    ServerLine welcome;
    welcome.kind = ServerLine::Kind::Welcome;
    welcome.name = name;
    send(out, id, welcome);
    // The names, in lines that stay inside the frame limit.
    ServerLine users;
    users.kind = ServerLine::Kind::Users;
    std::size_t bytes = 0;
    for (const auto &[otherId, other] : conns_) {
        if (!other.joined) {
            continue;
        }
        if (bytes + other.name.size() + 1 > 800) {
            send(out, id, users);
            users.names.clear();
            bytes = 0;
        }
        users.names.push_back(other.name);
        bytes += other.name.size() + 1;
        (void)otherId;
    }
    send(out, id, users);
    expireRing(now);
    for (const auto &e : ring_) {
        ServerLine h;
        h.kind = ServerLine::Kind::Hist;
        h.serial = e.serial;
        h.age = std::uint32_t(std::max<Millis>(0, (now - e.time) / 1000));
        h.name = e.name;
        h.text = e.text;
        send(out, id, h);
    }
    ServerLine ready;
    ready.kind = ServerLine::Kind::Ready;
    send(out, id, ready);
    ServerLine online;
    online.kind = ServerLine::Kind::Online;
    online.count = std::uint32_t(joinedCount());
    send(out, id, online);
    ServerLine joined;
    joined.kind = ServerLine::Kind::Join;
    joined.name = name;
    sendAll(out, joined, id);
}

Actions ChatRoom::connected(ConnId id, const std::string &source, Millis now) {
    now_ = now;
    Actions out;
    auto refuse = [&](ErrorCode code, const char *text) {
        ServerLine e;
        e.kind = ServerLine::Kind::Error;
        e.code = code;
        e.text = text;
        disconnectWith(out, id, formatServerLine(e));
    };
    if (conns_.count(id)) {
        return out; // A repeated id: ignore.
    }
    if (const auto ban = bans_.find(source);
        ban != bans_.end() && ban->second > now) {
        refuse(ErrorCode::Banned, "you are not allowed in right now");
        return out;
    }
    if (locked_) {
        refuse(ErrorCode::Full, "the room is closed for now");
        return out;
    }
    {
        if (connectBuckets_.size() >= config_.maxTableEntries &&
            !connectBuckets_.count(source)) {
            connectBuckets_.erase(connectBuckets_.begin());
        }
        auto bucket =
            connectBuckets_
                .try_emplace(source, Bucket{double(config_.connectBurst), now})
                .first;
        if (!takeToken(bucket->second,
                       config_.connectBurst,
                       config_.connectRefill,
                       now)) {
            refuse(ErrorCode::Rate, "slow down");
            return out;
        }
    }
    std::size_t unhelloed = 0;
    for (const auto &entry : conns_) {
        unhelloed += !entry.second.joined;
    }
    if (conns_.size() >= config_.maxConnections ||
        perSource_[source] >= config_.maxPerSource ||
        unhelloed >= config_.maxUnhelloed) {
        if (perSource_[source] == 0) {
            perSource_.erase(source);
        }
        refuse(ErrorCode::Full, "the room is full");
        return out;
    }
    ++perSource_[source];
    Conn c;
    c.source = source;
    c.connectedAt = now;
    c.violationStart = now;
    conns_.emplace(id, std::move(c));
    return out;
}

Actions ChatRoom::line(ConnId id, std::string_view frame, Millis now) {
    now_ = now;
    Actions out;
    const auto it = conns_.find(id);
    if (it == conns_.end()) {
        return out;
    }
    Conn &c = it->second;
    const ClientParse parsed = parseClientLine(frame);
    if (parsed.error != ProtoError::None) {
        sendError(out, id, ErrorCode::BadLine, "not understood");
        violation(out, id, now, "parse");
        return out;
    }
    const ClientLine &l = parsed.line;
    if (!c.joined && l.kind != ClientLine::Kind::Hello) {
        sendError(out, id, ErrorCode::BadLine, "say hello first");
        violation(out, id, now, "order");
        return out;
    }
    switch (l.kind) {
    case ClientLine::Kind::Hello:
        if (c.joined) {
            sendError(out, id, ErrorCode::BadLine, "already here");
            violation(out, id, now, "hello");
        }
        else if (findByName(l.name)) {
            sendError(out, id, ErrorCode::NameTaken, "that name is in use");
            violation(out, id, now, "name");
        }
        else {
            join(out, id, c, l.name, now);
        }
        break;
    case ClientLine::Kind::Nick: {
        const ConnId owner = findByName(l.name);
        if (now - c.lastNick < config_.nickInterval) {
            sendError(out, id, ErrorCode::Rate, "slow down");
            violation(out, id, now, "nick");
        }
        else if (owner != 0 && owner != id) {
            sendError(out, id, ErrorCode::NameTaken, "that name is in use");
            violation(out, id, now, "name");
        }
        else if (l.name != c.name) {
            c.lastNick = now;
            clearDraft(out, id, c);
            ServerLine leave;
            leave.kind = ServerLine::Kind::Leave;
            leave.name = c.name;
            sendAll(out, leave, id);
            c.name = l.name;
            ServerLine welcome;
            welcome.kind = ServerLine::Kind::Welcome;
            welcome.name = c.name;
            send(out, id, welcome);
            ServerLine joined;
            joined.kind = ServerLine::Kind::Join;
            joined.name = c.name;
            sendAll(out, joined, id);
        }
        break;
    }
    case ClientLine::Kind::Draft: {
        if (now - c.draftSecondStart >= 1000) {
            c.draftSecondStart = now;
            c.draftsThisSecond = 0;
        }
        if (++c.draftsThisSecond > config_.draftFloodPerSecond) {
            violation(out, id, now, "flood");
            break;
        }
        const auto mute = mutes_.find(c.source);
        if (mute != mutes_.end() && mute->second > now) {
            break; // A muted visitor types into the void.
        }
        if (l.bubble < c.bubble || (l.bubble == c.bubble && c.bubbleLocked)) {
            break; // Late or duplicate.
        }
        if (l.bubble > c.bubble) {
            if (!c.bubbleLocked && !trimSpaces(c.draft).empty()) {
                if (!lockBubble(out, id, c, now, false)) {
                    clearDraft(out, id, c); // Rate limited: that text is lost.
                }
            }
            else {
                clearDraft(out, id, c);
            }
            c.bubble = l.bubble;
            c.bubbleLocked = false;
        }
        c.draft = l.text;
        c.changedAt = now;
        if (c.draft.empty()) {
            clearDraft(out, id, c);
        }
        else {
            c.dirty = true;
        }
        break;
    }
    case ClientLine::Kind::Lock:
        if (l.bubble == c.bubble && !c.bubbleLocked) {
            lockBubble(out, id, c, now, true);
        }
        break;
    }
    return out;
}

Actions ChatRoom::badFrame(ConnId id, Millis now) {
    now_ = now;
    Actions out;
    if (!conns_.count(id)) {
        return out;
    }
    ServerLine e;
    e.kind = ServerLine::Kind::Error;
    e.code = ErrorCode::BadLine;
    e.text = "text frames only";
    disconnectWith(out, id, formatServerLine(e));
    drop(out, id, now, true);
    return out;
}

Actions ChatRoom::backlog(ConnId id, std::size_t bytes, Millis now) {
    now_ = now;
    Actions out;
    if (conns_.count(id) && bytes > config_.maxBacklogBytes) {
        disconnectWith(out, id, {});
        drop(out, id, now, true);
    }
    return out;
}

Actions ChatRoom::closed(ConnId id, Millis now) {
    now_ = now;
    Actions out;
    drop(out, id, now, true);
    return out;
}

Actions ChatRoom::tick(Millis now) {
    now_ = now;
    Actions out;
    expireRing(now);
    for (auto it = bans_.begin(); it != bans_.end();) {
        it = it->second <= now ? bans_.erase(it) : std::next(it);
    }
    // Buckets that are full again hold nothing worth keeping.
    for (auto it = messageBuckets_.begin(); it != messageBuckets_.end();) {
        const bool idle =
            !perSource_.count(it->first) &&
            it->second.tokens + double(now - it->second.at) /
                                    double(config_.messageRefill) >=
                config_.messageBurst;
        it = idle ? messageBuckets_.erase(it) : std::next(it);
    }
    for (auto it = connectBuckets_.begin(); it != connectBuckets_.end();) {
        const bool full =
            it->second.tokens +
                double(now - it->second.at) / double(config_.connectRefill) >=
            config_.connectBurst;
        it = full ? connectBuckets_.erase(it) : std::next(it);
    }
    for (auto it = mutes_.begin(); it != mutes_.end();) {
        it = it->second <= now ? mutes_.erase(it) : std::next(it);
    }
    std::vector<ConnId> ids;
    for (const auto &entry : conns_) {
        ids.push_back(entry.first);
    }
    for (const ConnId id : ids) {
        const auto it = conns_.find(id);
        if (it == conns_.end()) {
            continue;
        }
        Conn &c = it->second;
        if (!c.joined) {
            if (now - c.connectedAt >= config_.helloDeadline) {
                ServerLine e;
                e.kind = ServerLine::Kind::Error;
                e.code = ErrorCode::BadLine;
                e.text = "say hello";
                disconnectWith(out, id, formatServerLine(e));
                drop(out, id, now, false);
            }
        }
        else if (!c.bubbleLocked && !trimSpaces(c.draft).empty() &&
                 now - c.changedAt >= config_.lockBackstop) {
            if (!lockBubble(out, id, c, now, false)) {
                // Try again in a second.
                c.changedAt = now - config_.lockBackstop + 1000;
            }
        }
    }
    // Live drafts: the most recently active typers are shown to the others.
    std::vector<std::pair<Millis, ConnId>> active;
    for (const auto &[id, c] : conns_) {
        if (c.joined && !c.draft.empty()) {
            active.emplace_back(c.changedAt, id);
        }
    }
    std::sort(active.begin(), active.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    for (std::size_t i = 0; i < active.size(); ++i) {
        const ConnId id = active[i].second;
        Conn &c = conns_.at(id);
        if (i < config_.maxTypersShown) {
            const bool due =
                c.dirty && now - c.forwardedAt >= config_.draftForward;
            if (due || (!c.shown && c.dirty)) {
                ServerLine d;
                d.kind = ServerLine::Kind::Draft;
                d.name = c.name;
                d.text = c.draft;
                sendAll(out, d, id);
                c.dirty = false;
                c.forwardedAt = now;
                c.shown = true;
            }
            else if (!c.shown) {
                c.dirty =
                    true; // Came back into view: send it at the next due time.
            }
        }
        else if (c.shown) {
            c.shown = false;
            c.dirty = true;
            ServerLine gone;
            gone.kind = ServerLine::Kind::Gone;
            gone.name = c.name;
            sendAll(out, gone, id);
        }
    }
    const auto hidden =
        std::uint32_t(active.size() > config_.maxTypersShown
                          ? active.size() - config_.maxTypersShown
                          : 0);
    if (hidden != lastTypers_) {
        lastTypers_ = hidden;
        ServerLine t;
        t.kind = ServerLine::Kind::Typers;
        t.count = hidden;
        sendAll(out, t);
    }
    const auto online = std::uint32_t(joinedCount());
    if (online != lastOnline_ &&
        now - onlineSentAt_ >= config_.onlineInterval) {
        lastOnline_ = online;
        onlineSentAt_ = now;
        ServerLine o;
        o.kind = ServerLine::Kind::Online;
        o.count = online;
        sendAll(out, o);
    }
    return out;
}

std::optional<Millis> ChatRoom::nextDeadline() const {
    std::optional<Millis> best;
    auto consider = [&](Millis t) {
        if (!best || t < *best) {
            best = t;
        }
    };
    for (const auto &[id, c] : conns_) {
        (void)id;
        if (!c.joined) {
            consider(c.connectedAt + config_.helloDeadline);
            continue;
        }
        if (!c.bubbleLocked && !c.draft.empty()) {
            consider(c.changedAt + config_.lockBackstop);
        }
        if (!c.draft.empty() && (c.dirty || !c.shown)) {
            consider(c.forwardedAt + config_.draftForward);
        }
    }
    if (joinedCount() != lastOnline_) {
        consider(onlineSentAt_ + config_.onlineInterval);
    }
    return best;
}

ChatRoom::Stats ChatRoom::stats() const {
    Stats s;
    s.connections = conns_.size();
    s.online = joinedCount();
    s.ring = ring_.size();
    s.bans = bans_.size();
    s.messages = messages_;
    s.violations = violations_;
    return s;
}

Actions ChatRoom::admin(std::string_view command,
                        Millis now,
                        std::string &reply) {
    now_ = now;
    Actions out;
    std::istringstream words{std::string(command)};
    std::string verb;
    words >> verb;
    std::string rest;
    std::getline(words, rest);
    if (!rest.empty() && rest.front() == ' ') {
        rest.erase(0, 1);
    }
    auto firstWord = [](const std::string &s) {
        return s.substr(0, s.find(' '));
    };
    auto secondWord = [](const std::string &s) {
        const auto sp = s.find(' ');
        return sp == std::string::npos ? std::string() : s.substr(sp + 1);
    };
    auto banDisconnect = [&](const std::string &source, const char *text) {
        std::vector<ConnId> victims;
        for (const auto &[id, c] : conns_) {
            if (c.source == source) {
                victims.push_back(id);
            }
        }
        for (const ConnId id : victims) {
            ServerLine e;
            e.kind = ServerLine::Kind::Error;
            e.code = ErrorCode::Banned;
            e.text = text;
            disconnectWith(out, id, formatServerLine(e));
            drop(out, id, now, true);
        }
        return victims.size();
    };
    if (verb == "list") {
        std::ostringstream s;
        for (const auto &[id, c] : conns_) {
            s << id << ' ' << (c.joined ? c.name : "(no name yet)") << ' '
              << (now - c.connectedAt) / 1000 << "s " << c.source
              << (c.draft.empty() ? "" : " typing") << '\n';
        }
        s << conns_.size() << " connection(s)\n";
        reply = s.str();
    }
    else if (verb == "kick") {
        const ConnId id = findByName(firstWord(rest));
        if (id == 0) {
            reply = "no such name\n";
        }
        else {
            ServerLine e;
            e.kind = ServerLine::Kind::Error;
            e.code = ErrorCode::Banned;
            e.text = "removed by a moderator";
            disconnectWith(out, id, formatServerLine(e));
            drop(out, id, now, true);
            reply = "kicked\n";
        }
    }
    else if (verb == "ban") {
        const std::string target = firstWord(rest);
        Millis duration = 60 * 60 * 1000;
        const std::string minutes = secondWord(rest);
        if (!minutes.empty() && !parseMinutes(minutes, duration)) {
            reply = "usage: ban NAME|SOURCE [minutes]\n";
            return out;
        }
        std::string source = target;
        if (const ConnId id = findByName(target); id != 0) {
            source = conns_.at(id).source;
        }
        else if (!perSource_.count(target)) {
            // An unknown name: treat it as a source key (an address).
            source = target;
        }
        if (source.empty()) {
            reply = "usage: ban NAME|SOURCE [minutes]\n";
            return out;
        }
        setUntil(bans_, source, now + duration);
        const auto n =
            banDisconnect(source, "you are not allowed in right now");
        reply = "banned " + source + ", " + std::to_string(n) +
                " connection(s) closed\n";
    }
    else if (verb == "unban") {
        reply = bans_.erase(firstWord(rest)) ? "unbanned\n" : "not banned\n";
    }
    else if (verb == "mute" || verb == "unmute") {
        const ConnId id = findByName(firstWord(rest));
        if (id == 0) {
            reply = "no such name\n";
        }
        else if (verb == "unmute") {
            mutes_.erase(conns_.at(id).source);
            reply = "unmuted\n";
        }
        else {
            Millis duration = 10 * 60 * 1000;
            const std::string minutes = secondWord(rest);
            if (!minutes.empty() && !parseMinutes(minutes, duration)) {
                reply = "usage: mute NAME [minutes]\n";
                return out;
            }
            setUntil(mutes_, conns_.at(id).source, now + duration);
            clearDraft(out, id, conns_.at(id));
            reply = "muted\n";
        }
    }
    else if (verb == "delete") {
        std::uint64_t serial = 0;
        const std::string s = firstWord(rest);
        bool numeric = !s.empty() && s.size() < 18;
        for (const char ch : s) {
            numeric = numeric && ch >= '0' && ch <= '9';
            serial = serial * 10 + std::uint64_t(ch - '0');
        }
        const auto it =
            std::find_if(ring_.begin(), ring_.end(), [&](const Entry &e) {
                return e.serial == serial;
            });
        if (!numeric || it == ring_.end()) {
            reply = "no such message\n";
        }
        else {
            ring_.erase(it);
            ServerLine h;
            h.kind = ServerLine::Kind::Hide;
            h.serial = serial;
            sendAll(out, h);
            reply = "deleted\n";
        }
    }
    else if (verb == "clear") {
        for (const auto &e : ring_) {
            ServerLine h;
            h.kind = ServerLine::Kind::Hide;
            h.serial = e.serial;
            sendAll(out, h);
        }
        reply = std::to_string(ring_.size()) + " message(s) removed\n";
        ring_.clear();
    }
    else if (verb == "lock" || verb == "unlock") {
        locked_ = verb == "lock";
        reply = locked_ ? "locked: no new connections\n" : "unlocked\n";
    }
    else if (verb == "say") {
        if (!checkMessage(rest).ok()) {
            reply = "that text is not allowed in the chat\n";
        }
        else {
            ServerLine n;
            n.kind = ServerLine::Kind::Notice;
            n.text = rest;
            sendAll(out, n);
            reply = "sent\n";
        }
    }
    else if (verb == "stats") {
        const Stats s = stats();
        std::ostringstream o;
        o << "connections " << s.connections << ", online " << s.online
          << ", ring " << s.ring << ", messages " << s.messages
          << ", violations " << s.violations << ", bans " << s.bans
          << (locked_ ? ", LOCKED" : "") << '\n';
        reply = o.str();
    }
    else if (verb == "help" || verb.empty()) {
        reply =
            "list | kick NAME | ban NAME|SOURCE [min] | unban SOURCE | "
            "mute NAME [min] | unmute NAME | delete SERIAL | clear | lock | "
            "unlock | say TEXT | stats\n";
    }
    else {
        reply = "unknown command (try help)\n";
    }
    return out;
}

} // namespace chat
