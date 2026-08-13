#pragma once

#include "project_attachment.h"

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace lingtai::desktop {

// The local write outcome only: `requested` proves only that the local write
// returned, exactly like `LocalCommandCore.send_signal("sleep")`'s `sent`. It
// is never target acceptance, queueing, or a state transition.
enum class AgentSleepRequestResult { requested, failed_local };

// Requests sleep for exactly one selected Agent by creating or truncating
// `<accepted root>/.lingtai/<selected key>/.sleep` to zero bytes, walked
// descriptor-relative and no-follow from the accepted project root with the
// shared `posix_internal` containment primitives. Neither `.lingtai` nor the
// selected key's own directory is ever created; only the one `.sleep` leaf is
// created or truncated. An existing non-regular target at that exact leaf
// name is refused rather than replaced. Overwrites any existing marker,
// matching the canonical coalescing, non-queued, non-exactly-once semantics.
[[nodiscard]] AgentSleepRequestResult request_agent_sleep(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

// A byte-size/tail-LF boundary of the selected Agent's own
// `logs/events.jsonl`, captured strictly before the one `.sleep` write so a
// pre-existing partial or complete tail line can never be misattributed to
// this request. `available` is false only for a missing/unreadable/unsafe
// log source; that never blocks or strengthens the write itself.
struct AgentSleepEventBaseline {
    bool available = false;
    std::uintmax_t byte_size = 0;
    bool ends_with_newline = false;
};

[[nodiscard]] AgentSleepEventBaseline capture_agent_sleep_event_baseline(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

// Whether a complete LF-terminated `sleep_received(source="signal_file")`
// JSON row has appeared strictly after the captured baseline. Best-effort,
// bounded, and stateless: every call independently reopens the log and reads
// only a small operation-local cap of the bytes appended since the baseline.
// A pre-existing partial tail line that completes after the baseline is
// discarded rather than attributed, using the recorded tail-LF bit. There is
// no request ID and no exactly-once guarantee; a missed record only reduces
// to "not observed".
[[nodiscard]] bool observe_agent_sleep_received(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key,
    const AgentSleepEventBaseline &baseline) noexcept;

} // namespace lingtai::desktop
