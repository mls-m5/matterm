#include "chatclient.h"

#include <algorithm>

namespace chat {

ChatClient::ChatClient(Config config)
    : config_(std::move(config))
    , name_(config_.name) {}

void ChatClient::setState(State s) {
    if (state_ == s) {
        return;
    }
    state_ = s;
    Event e;
    e.kind = Event::Kind::State;
    push(std::move(e));
}

void ChatClient::push(Event e) {
    if (events_.size() >= config_.maxEvents) {
        events_.erase(events_.begin());
    }
    events_.push_back(std::move(e));
}

std::vector<ChatClient::Event> ChatClient::takeEvents() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

void ChatClient::addHistory(Bubble b) {
    history_.push_back(std::move(b));
    while (history_.size() > config_.maxHistory) {
        history_.pop_front();
    }
}

ChatClient::OwnState ChatClient::ownState() const noexcept {
    if (draft_.empty()) {
        return OwnState::Idle;
    }
    return state_ == State::Joined ? OwnState::DraftSent : OwnState::NotSent;
}

ChatClient::Out ChatClient::connect(Millis) {
    Out out;
    if (state_ == State::Off || state_ == State::Closed) {
        typers_.clear();
        users_.clear();
        pendingUsers_.clear();
        hidden_ = 0;
        online_ = 0;
        reason_.clear();
        badLines_ = 0;
        sentAny_ = false;
        sentDraft_.clear();
        lockSent_ = false;
        setState(State::Connecting);
    }
    return out;
}

ChatClient::Out ChatClient::opened(Millis) {
    Out out;
    if (state_ == State::Connecting) {
        setState(State::Hello);
        out.send.push_back("hello\t" + name_);
    }
    return out;
}

ChatClient::Out ChatClient::hello(const std::string &name, Millis) {
    Out out;
    if (state_ == State::NeedName && checkName(name) == NameError::None) {
        name_ = name;
        setState(State::Hello);
        out.send.push_back("hello\t" + name_);
    }
    return out;
}

ChatClient::Out ChatClient::rename(const std::string &name, Millis) {
    Out out;
    if (state_ == State::Joined && checkName(name) == NameError::None) {
        out.send.push_back("nick\t" + name);
    }
    return out;
}

ChatClient::Out ChatClient::leave(Millis) {
    Out out;
    if (state_ != State::Off && state_ != State::Closed) {
        out.close = true;
        reason_ = "you left";
        setState(State::Closed);
        typers_.clear();
        users_.clear();
        hidden_ = 0;
        sentAny_ = false;
        lockSent_ = false;
    }
    return out;
}

ChatClient::Out ChatClient::closed(Millis) {
    Out out;
    if (state_ != State::Closed && state_ != State::Off) {
        if (reason_.empty()) {
            reason_ = "the connection closed";
        }
        setState(State::Closed);
    }
    typers_.clear();
    users_.clear();
    hidden_ = 0;
    sentAny_ = false;
    lockSent_ = false;
    return out;
}

void ChatClient::flushDraft(Out &out, Millis now, bool force) {
    if (state_ != State::Joined || lockSent_) {
        return;
    }
    if (sentAny_ && draft_ == sentDraft_) {
        return;
    }
    if (!sentAny_ && draft_.empty()) {
        return;
    }
    if (!force && now - draftSentAt_ < config_.draftInterval) {
        return;
    }
    out.send.push_back("draft\t" + std::to_string(bubble_) + "\t" + draft_);
    sentDraft_ = draft_;
    sentAny_ = true;
    draftSentAt_ = now;
}

void ChatClient::startNextBubble() {
    ++bubble_;
    if (bubble_ > kMaxBubble) {
        bubble_ = 1;
    }
    lockSent_ = false;
    sentAny_ = false;
    sentDraft_.clear();
}

bool ChatClient::edit(const std::string &text, Millis now, Out &out) {
    if (!checkDraft(text).ok()) {
        return false;
    }
    if (text != draft_) {
        draft_ = text;
        editedAt_ = now;
        flushDraft(out, now, false);
    }
    return true;
}

ChatClient::Out ChatClient::submit(Millis now) {
    Out out;
    if (state_ == State::Joined && !lockSent_ && !trimSpaces(draft_).empty()) {
        flushDraft(out, now, true);
        out.send.push_back("lock\t" + std::to_string(bubble_));
        lockSent_ = true;
    }
    return out;
}

ChatClient::Out ChatClient::tick(Millis now) {
    Out out;
    if (state_ != State::Joined) {
        return out;
    }
    if (!lockSent_ && !trimSpaces(draft_).empty() &&
        now - editedAt_ >= config_.idleLock) {
        return submit(now);
    }
    flushDraft(out, now, false);
    return out;
}

std::optional<Millis> ChatClient::nextDeadline() const {
    if (state_ != State::Joined || lockSent_) {
        return std::nullopt;
    }
    std::optional<Millis> best;
    auto consider = [&](Millis t) {
        if (!best || t < *best) {
            best = t;
        }
    };
    if (!trimSpaces(draft_).empty()) {
        consider(editedAt_ + config_.idleLock);
    }
    if (!(sentAny_ ? draft_ == sentDraft_ : draft_.empty())) {
        consider(draftSentAt_ + config_.draftInterval);
    }
    return best;
}

ChatClient::Out ChatClient::receive(std::string_view frame, Millis now) {
    Out out;
    if (state_ == State::Off || state_ == State::Closed) {
        return out;
    }
    const ServerParse parsed = parseServerLine(frame);
    if (parsed.error != ProtoError::None) {
        if (++badLines_ > config_.maxBadLines) {
            reason_ = "the server sent too much that was not understood";
            out.close = true;
            setState(State::Closed);
        }
        return out;
    }
    const ServerLine &l = parsed.line;
    switch (l.kind) {
    case ServerLine::Kind::Welcome:
        name_ = l.name;
        break;
    case ServerLine::Kind::Users:
        for (const auto &n : l.names) {
            if (pendingUsers_.size() < config_.maxUsers) {
                pendingUsers_.push_back(n);
            }
        }
        break;
    case ServerLine::Kind::Hist: {
        Bubble b;
        b.serial = l.serial;
        b.name = l.name;
        b.text = l.text;
        b.at = now - Millis(l.age) * 1000;
        b.own = l.name == name_;
        addHistory(b);
        Event e;
        e.kind = Event::Kind::Message;
        e.bubble = b;
        push(std::move(e));
        break;
    }
    case ServerLine::Kind::Ready:
        if (state_ == State::Hello) {
            users_ = pendingUsers_;
            pendingUsers_.clear();
            setState(State::Joined);
            flushDraft(out, now, true); // Text typed while connecting.
        }
        break;
    case ServerLine::Kind::Draft:
        if (l.name != name_) {
            auto it =
                std::find_if(typers_.begin(),
                             typers_.end(),
                             [&](const Typer &t) { return t.name == l.name; });
            if (it != typers_.end()) {
                it->text = l.text;
            }
            else if (typers_.size() < 16) {
                typers_.push_back({l.name, l.text});
            }
        }
        break;
    case ServerLine::Kind::Gone:
        typers_.erase(
            std::remove_if(typers_.begin(),
                           typers_.end(),
                           [&](const Typer &t) { return t.name == l.name; }),
            typers_.end());
        break;
    case ServerLine::Kind::Typers:
        hidden_ = l.count;
        break;
    case ServerLine::Kind::Online:
        online_ = l.count;
        break;
    case ServerLine::Kind::Msg: {
        typers_.erase(
            std::remove_if(typers_.begin(),
                           typers_.end(),
                           [&](const Typer &t) { return t.name == l.name; }),
            typers_.end());
        Bubble b;
        b.serial = l.serial;
        b.name = l.name;
        b.text = l.text;
        b.at = now;
        b.own = l.name == name_;
        addHistory(b);
        Event e;
        e.kind = Event::Kind::Message;
        e.bubble = b;
        push(std::move(e));
        break;
    }
    case ServerLine::Kind::Ack:
        if (l.bubble == bubble_ && state_ == State::Joined) {
            // The server posted `l.text`: take it off the start of the input
            // and keep what was typed meanwhile as the next bubble. If the
            // person edited inside the posted text, keep everything (a
            // duplicate is better than lost typing).
            std::string rest = draft_;
            const auto first = rest.find_first_not_of(' ');
            if (first != std::string::npos &&
                rest.compare(first, l.text.size(), l.text) == 0) {
                rest = rest.substr(first + l.text.size());
                const auto start = rest.find_first_not_of(' ');
                rest = start == std::string::npos ? std::string()
                                                  : rest.substr(start);
            }
            startNextBubble();
            draft_ = rest;
            editedAt_ = now;
            flushDraft(out, now, true);
        }
        break;
    case ServerLine::Kind::Join:
        if (std::find(users_.begin(), users_.end(), l.name) == users_.end() &&
            users_.size() < config_.maxUsers) {
            users_.push_back(l.name);
        }
        {
            Event e;
            e.kind = Event::Kind::Join;
            e.name = l.name;
            push(std::move(e));
        }
        break;
    case ServerLine::Kind::Leave:
        users_.erase(std::remove(users_.begin(), users_.end(), l.name),
                     users_.end());
        typers_.erase(
            std::remove_if(typers_.begin(),
                           typers_.end(),
                           [&](const Typer &t) { return t.name == l.name; }),
            typers_.end());
        {
            Event e;
            e.kind = Event::Kind::Leave;
            e.name = l.name;
            push(std::move(e));
        }
        break;
    case ServerLine::Kind::Hide: {
        history_.erase(std::remove_if(history_.begin(),
                                      history_.end(),
                                      [&](const Bubble &b) {
                                          return b.serial == l.serial;
                                      }),
                       history_.end());
        Event e;
        e.kind = Event::Kind::Hide;
        e.serial = l.serial;
        push(std::move(e));
        break;
    }
    case ServerLine::Kind::Notice: {
        Event e;
        e.kind = Event::Kind::Notice;
        e.text = l.text;
        push(std::move(e));
        break;
    }
    case ServerLine::Kind::Error: {
        Event e;
        e.kind = Event::Kind::Error;
        e.code = l.code;
        e.text = l.text;
        push(std::move(e));
        if ((l.code == ErrorCode::NameTaken || l.code == ErrorCode::BadName) &&
            state_ == State::Hello) {
            setState(State::NeedName);
            Event n;
            n.kind = Event::Kind::NameTaken;
            push(std::move(n));
        }
        else if (l.code == ErrorCode::Banned || l.code == ErrorCode::Full) {
            reason_ = l.text;
            out.close = true;
            setState(State::Closed);
        }
        else if (l.code == ErrorCode::Rate && lockSent_) {
            // The room did not take the lock; try again after another pause.
            lockSent_ = false;
            editedAt_ = now;
        }
        break;
    }
    }
    return out;
}

} // namespace chat
