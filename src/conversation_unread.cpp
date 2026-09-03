#include "conversation_unread.h"

#include <limits>
#include <unordered_set>

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

[[nodiscard]] bool cursor_equal(
        const ConversationReadCursor &a,
        const ConversationReadCursor &b) noexcept {
    return a.timestamp == b.timestamp && a.id == b.id;
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

std::size_t saturating_unread_add(
        std::size_t left,
        std::size_t right) noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    return right > maximum - left ? maximum : left + right;
}

bool ConversationUnreadSession::observe(
        const std::filesystem::path &canonical_project_root,
        const std::string &agent_key,
        const DirectConversationHistory &history,
        bool viewed) {
    auto &project = projects_[canonical_project_root];
    const auto tip = tip_inbound_cursor(history);
    const auto [conversation, inserted] = project.try_emplace(
        agent_key, ConversationState{
        .read_cursor = tip,
        .observation_tip = tip,
        .unread = 0,
    });
    if (inserted) {
        return true;
    }
    auto &state = conversation->second;
    if (cursor_less(tip, state.observation_tip)) {
        // A duplicate window can complete an older mailbox generation after a
        // newer one. It must not regress either the accepted tip or unread.
        return false;
    }
    const auto old_read_cursor = state.read_cursor;
    const auto old_observation_tip = state.observation_tip;
    const auto old_unread = state.unread;
    if (cursor_less(state.observation_tip, tip)) {
        state.observation_tip = tip;
    } else if (!viewed) {
        return false;
    }
    if (viewed && cursor_less(state.read_cursor, tip)) {
        state.read_cursor = tip;
    }
    state.unread = count_inbound_after_cursor(history, state.read_cursor);
    const auto changed = !cursor_equal(old_read_cursor, state.read_cursor)
        || !cursor_equal(old_observation_tip, state.observation_tip)
        || old_unread != state.unread;
    return changed;
}

ProjectUnreadSnapshot ConversationUnreadSession::snapshot(
        const std::filesystem::path &canonical_project_root,
        std::span<const std::string> valid_agent_keys) const {
    auto result = ProjectUnreadSnapshot{};
    const auto project = projects_.find(canonical_project_root);
    if (project == projects_.end()) {
        return result;
    }
    for (const auto &agent_key : valid_agent_keys) {
        const auto conversation = project->second.find(agent_key);
        if (conversation == project->second.end()
            || conversation->second.unread == 0) {
            continue;
        }
        const auto [unused, inserted] = result.counts.emplace(
            agent_key, conversation->second.unread);
        static_cast<void>(unused);
        if (inserted) {
            result.total = saturating_unread_add(
                result.total, conversation->second.unread);
        }
    }
    return result;
}

std::size_t ConversationUnreadSession::total_for(
        std::span<const OpenProjectUnreadRequest> open_projects) const {
    auto requested = std::unordered_map<std::filesystem::path,
        std::unordered_set<std::string>>{};
    for (const auto &open_project : open_projects) {
        auto &agents = requested[open_project.canonical_project_root];
        agents.insert(open_project.valid_agent_keys.begin(),
            open_project.valid_agent_keys.end());
    }
    auto total = std::size_t{0};
    for (const auto &[project_root, agent_keys] : requested) {
        const auto project = projects_.find(project_root);
        if (project == projects_.end()) {
            continue;
        }
        for (const auto &agent_key : agent_keys) {
            const auto conversation = project->second.find(agent_key);
            if (conversation == project->second.end()) {
                continue;
            }
            total = saturating_unread_add(
                total, conversation->second.unread);
        }
    }
    return total;
}

void ConversationUnreadSession::clear() noexcept {
    projects_.clear();
}

} // namespace lingtai::desktop
