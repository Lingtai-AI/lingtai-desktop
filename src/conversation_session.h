#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {

// Mirrors TUI mail verbose levels (ctrl+o): off, thinking (footers + compact
// tools/thinking), extended (full tool bodies).
enum class ConversationVerboseLevel {
    off,
    thinking,
    extended,
};

[[nodiscard]] ConversationVerboseLevel cycle_conversation_verbose_level(
    ConversationVerboseLevel level) noexcept;

[[nodiscard]] const char *conversation_verbose_level_label(
    ConversationVerboseLevel level) noexcept;

struct SessionTokenUsage {
    std::int64_t input = 0;
    std::int64_t output = 0;
    std::int64_t cached = 0;
    std::int64_t api_duration_ms = 0;
    bool estimated = false;
};

// One parsed events.jsonl row the conversation verbose stream may render.
struct ConversationSessionEntry {
    std::string timestamp;
    std::string type;
    std::string body;
    std::string api_call_id;
    std::string reasoning;
    std::optional<SessionTokenUsage> token_usage;
};

// Token footer text (TUI mail.token_usage_footer parity).
[[nodiscard]] std::string format_token_usage_footer(
    const SessionTokenUsage &usage);

[[nodiscard]] std::optional<ConversationSessionEntry> parse_conversation_session_line(
    std::string_view json_line);

// Reads the selected Agent's logs/events.jsonl tail in file order. Only the
// newest tail bytes and entry cap are loaded so periodic refresh stays cheap.
// Symlinks and oversize files are rejected; partial trailing lines are ignored.
[[nodiscard]] std::vector<ConversationSessionEntry> read_conversation_session_events(
    const std::filesystem::path &project_root,
    const std::filesystem::path &agent_directory_key) noexcept;

// True when the selected Agent has a nonempty events.jsonl log (cheap stat).
[[nodiscard]] bool conversation_session_log_present(
    const std::filesystem::path &project_root,
    const std::filesystem::path &agent_directory_key) noexcept;

struct SessionLogStat {
    bool present = false;
    std::time_t mtime = 0;
    std::int64_t size = 0;
};

[[nodiscard]] SessionLogStat conversation_session_log_stat(
    const std::filesystem::path &project_root,
    const std::filesystem::path &agent_directory_key) noexcept;

// Body text for one verbose event at the given detail level.
[[nodiscard]] std::string conversation_verbose_event_body(
    const ConversationSessionEntry &entry,
    ConversationVerboseLevel level);

[[nodiscard]] bool conversation_verbose_event_visible(
    const ConversationSessionEntry &entry,
    ConversationVerboseLevel level) noexcept;

[[nodiscard]] bool conversation_api_group_separator_before(
    const ConversationSessionEntry *previous,
    const ConversationSessionEntry &current) noexcept;

} // namespace lingtai::desktop
