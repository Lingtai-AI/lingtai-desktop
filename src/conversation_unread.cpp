#include "conversation_unread.h"

namespace lingtai::desktop {
namespace {

[[nodiscard]] bool cursor_less(
        const ConversationReadCursor &a,
        const ConversationReadCursor &b) noexcept {
    if (a.timestamp != b.timestamp) {
        return a.timestamp < b.timestamp;
    }
    return a.id < b.id;
}

} // namespace

ConversationReadCursor tip_inbound_cursor(
        const DirectConversationHistory &history) noexcept {
    auto tip = ConversationReadCursor{};
    for (const auto &message : history.messages) {
        if (message.outgoing) {
            continue;
        }
        const auto candidate = ConversationReadCursor{
            message.timestamp, message.id};
        if (tip.timestamp.empty() && tip.id.empty()) {
            tip = candidate;
            continue;
        }
        if (cursor_less(tip, candidate)) {
            tip = candidate;
        }
    }
    return tip;
}

bool inbound_after_cursor(
        const DirectConversationMessage &message,
        const ConversationReadCursor &cursor) noexcept {
    if (message.outgoing) {
        return false;
    }
    // Empty cursor means "before any mail" — every inbound counts.
    if (cursor.timestamp.empty() && cursor.id.empty()) {
        return true;
    }
    return cursor_less(cursor, ConversationReadCursor{
        message.timestamp, message.id});
}

std::size_t count_inbound_after_cursor(
        const DirectConversationHistory &history,
        const ConversationReadCursor &cursor) noexcept {
    auto count = std::size_t{0};
    for (const auto &message : history.messages) {
        if (inbound_after_cursor(message, cursor)) {
            ++count;
        }
    }
    return count;
}

void ConversationUnreadState::clear() noexcept {
    cursors_.clear();
}

bool ConversationUnreadState::has_cursor(
        const std::string &agent_key) const {
    return cursors_.contains(agent_key);
}

void ConversationUnreadState::catch_up(
        const std::string &agent_key,
        const DirectConversationHistory &history) {
    cursors_[agent_key] = tip_inbound_cursor(history);
}

std::size_t ConversationUnreadState::unseen_inbound_count(
        const std::string &agent_key,
        const DirectConversationHistory &history) const {
    const auto found = cursors_.find(agent_key);
    if (found == cursors_.end()) {
        return 0;
    }
    return count_inbound_after_cursor(history, found->second);
}

} // namespace lingtai::desktop
