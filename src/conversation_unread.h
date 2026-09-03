#pragma once

#include "direct_conversation_history.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace lingtai::desktop {

// Session cursor for human-read inbound mail in one Agent conversation.
// Ordering matches DirectConversationHistory: timestamp, then message id.
struct ConversationReadCursor {
    std::string timestamp;
    std::string id;
};

struct ProjectUnreadSnapshot final {
    std::unordered_map<std::string, std::size_t> counts;
    std::size_t total = 0;
};

struct OpenProjectUnreadRequest final {
    std::filesystem::path canonical_project_root;
    std::vector<std::string> valid_agent_keys;
};

[[nodiscard]] std::size_t saturating_unread_add(
    std::size_t left, std::size_t right) noexcept;

// Pure, process-session unread ownership keyed by an already-canonical Project
// root and the Agent directory key used by conversation routing.
class ConversationUnreadSession final {
public:
    [[nodiscard]] bool observe(
        const std::filesystem::path &canonical_project_root,
        const std::string &agent_key,
        const DirectConversationHistory &history,
        bool viewed);

    [[nodiscard]] ProjectUnreadSnapshot snapshot(
        const std::filesystem::path &canonical_project_root,
        std::span<const std::string> valid_agent_keys) const;

    [[nodiscard]] std::size_t total_for(
        std::span<const OpenProjectUnreadRequest> open_projects) const;

    void clear() noexcept;

private:
    struct ConversationState final {
        ConversationReadCursor read_cursor;
        ConversationReadCursor observation_tip;
        std::size_t unread = 0;
    };

    std::unordered_map<std::filesystem::path,
        std::unordered_map<std::string, ConversationState>> projects_;
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
