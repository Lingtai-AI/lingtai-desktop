#pragma once

#include "project_attachment.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {

enum class AgentActivityToolStatus { unknown, success, error };

// One projected Agent Activity row: either public Agent diary text or one
// tool call, optionally completed by its matching tool_result. Nothing else
// from the source event is retained -- no raw JSON, args, or results.
struct AgentActivityRow {
    bool is_tool = false;
    std::string text;        // Agent diary text, bounded plain text; empty for a tool row
    std::string tool_name;   // bounded plain text; empty for a diary row
    std::string tool_action; // bounded plain text; empty when absent
    AgentActivityToolStatus tool_status = AgentActivityToolStatus::unknown;
    std::optional<double> tool_elapsed_ms;
    // A small bounded UTC display timestamp, already formatted; empty when
    // the source `ts` was absent, not a finite number, or finite but outside
    // the representable `time_t` range.
    std::string timestamp_text;
};

struct AgentActivitySnapshot {
    // False only for a missing/unreadable/unsafe source; an existing but
    // currently empty log is available with zero rows.
    bool available = false;
    std::vector<AgentActivityRow> rows; // file byte order, latest 100 max
    // One coarse count of malformed/invalid-UTF-8/non-object complete rows.
    // Unknown or filtered-but-valid rows are never counted here.
    std::size_t skipped = 0;
};

// Reads one bounded binary suffix (at most 512 KiB) of the selected Agent's
// own `<root>/.lingtai/<selected key>/logs/events.jsonl`, discards a leading
// mid-record fragment when the read did not begin at byte zero, and accepts
// only complete LF-terminated top-level JSON objects. Projects only the
// public `diary` / `tool_call` / `tool_result` allowlist, in file order,
// keeping at most the latest 100 rows. Stateless: every call is one
// independent snapshot with no durable cursor. Every walked path component is
// no-follow, and the final input must be a regular file; a missing,
// unreadable, or unsafe source reduces only to `available == false`. Writes
// nothing.
[[nodiscard]] AgentActivitySnapshot read_agent_activity(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

} // namespace lingtai::desktop
