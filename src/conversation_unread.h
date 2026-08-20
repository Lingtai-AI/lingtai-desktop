#pragma once

#include "direct_conversation_history.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace lingtai::desktop {

// Session cursor for human-read inbound mail in one Agent conversation.
// Ordering matches DirectConversationHistory: timestamp, then message id.
struct ConversationReadCursor {
    std::string timestamp;
    std::string id;
};

// Session-only human unread state. Cleared on project close. Distinct from
// MessageReactionStore receipts (those track agent-seen outbound mail).
class ConversationUnreadState {
public:
    void clear() noexcept;

    [[nodiscard]] bool has_cursor(const std::string &agent_key) const;

    // Advances the cursor to the latest inbound message (or an empty tip when
    // the thread has no inbound mail). Used when the conversation is open and
    // when seeding a newly attached project so historical mail is not badged.
    void catch_up(
        const std::string &agent_key,
        const DirectConversationHistory &history);

    // Inbound messages strictly after the stored cursor. Missing cursor means
    // the caller should catch_up first (treated as zero unseen here).
    [[nodiscard]] std::size_t unseen_inbound_count(
        const std::string &agent_key,
        const DirectConversationHistory &history) const;

private:
    std::unordered_map<std::string, ConversationReadCursor> cursors_;
};

[[nodiscard]] ConversationReadCursor tip_inbound_cursor(
    const DirectConversationHistory &history) noexcept;

[[nodiscard]] bool inbound_after_cursor(
    const DirectConversationMessage &message,
    const ConversationReadCursor &cursor) noexcept;

[[nodiscard]] std::size_t count_inbound_after_cursor(
    const DirectConversationHistory &history,
    const ConversationReadCursor &cursor) noexcept;

} // namespace lingtai::desktop
