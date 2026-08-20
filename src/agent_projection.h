#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {
class ProjectAttachment;

enum class AgentManifestKind { valid, malformed, unsafe };
enum class AgentRole { unknown, human, main, agent };

// A concise read-failure classification, not source provenance: no path,
// mtime, byte count, or observed time is retained anywhere in this header.
enum class AgentManifestDiagnosticKind {
    none, unsafe_symlink, unreadable, invalid_json, not_object };

enum class AgentPresenceKind {
    unknown, alive_human, alive, stale, missing, invalid, unavailable };

struct AgentLlmFacts {
    std::optional<std::string> provider, model, base_url, api_compat;
    std::optional<std::int64_t> context_limit;
};

struct AgentCapabilityFacts {
    // Intrinsics first, then de-duplicated manifest-declared names in
    // manifest order. Raw manifest evidence is not retained.
    std::vector<std::string> display_names;
};

struct AgentIdentityFacts {
    std::optional<std::string> agent_id, true_name, nickname, address, state;
    AgentLlmFacts llm;
    AgentCapabilityFacts capabilities;
};

struct AgentActiveTurnFacts {
    std::optional<std::string> kind, id;
    std::optional<double> started_at, elapsed_seconds;
};

struct AgentContextFacts {
    std::int64_t window_size = 0;
    std::optional<std::int64_t> system_tokens, tools_tokens, history_tokens,
        total_tokens, fixed_tokens, growing_tokens;
    std::optional<double> usage_percent;
};

struct AgentRuntimeFacts {
    std::optional<std::string> state;
    std::optional<bool> running;
    std::optional<std::int64_t> pid;
    std::optional<double> state_changed_at, last_progress_at,
        no_progress_seconds;
    std::optional<AgentActiveTurnFacts> active_turn;
    std::optional<AgentContextFacts> context;
};

struct AgentRow {
    // Lossless immediate directory name; authoritative even without agent_id.
    std::filesystem::path directory_key, directory_path;
    AgentManifestKind manifest_kind = AgentManifestKind::malformed;
    AgentManifestDiagnosticKind manifest_diagnostic =
        AgentManifestDiagnosticKind::unreadable;
    AgentRole role = AgentRole::unknown;
    std::optional<AgentIdentityFacts> identity;
    AgentPresenceKind presence = AgentPresenceKind::unknown;
    std::optional<double> heartbeat_age_seconds;
    std::optional<AgentRuntimeFacts> status;
    // Resolved lifecycle label shared with the TUI mail refresh path:
    // manifest state while heartbeat-fresh, otherwise suspended or refreshing.
    std::string lifecycle_state;
};

enum class AgentScanState { complete, unavailable };

struct AgentSnapshot {
    AgentScanState scan = AgentScanState::unavailable;
    // Deterministic, locale-independent ascending order by directory_key; one
    // unsafe or unreadable sibling never hides a later healthy one.
    std::vector<AgentRow> items;
};

// One read-only composite projection of <canonical attachment root>/.lingtai
// and its immediate children: safe-leaf discovery, manifest identity, role,
// heartbeat presence, and current status, all from one shared wall-clock
// sample. No component beyond the immediate child is ever traversed, and no
// intermediate symlink is ever followed.
[[nodiscard]] AgentSnapshot project_agents(
    const ProjectAttachment &attachment) noexcept;

} // namespace lingtai::desktop
