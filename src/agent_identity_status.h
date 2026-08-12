#pragma once
#include "agent_roster_presence.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>
namespace lingtai::desktop { class ProjectAttachment;
enum class AgentStatusObservationState { read_this_scan, absent,
    rejected_unsafe, observed_unavailable };
enum class AgentStatusDiagnosticKind { none, unsafe_symlink, not_regular,
    unreadable, io_error, too_large, container_unavailable, container_changed,
    source_changed, invalid_json, not_object };
struct AgentStatusSource {
    std::filesystem::path path, relative_path;
    AgentStatusObservationState observation = AgentStatusObservationState::observed_unavailable;
    AgentStatusDiagnosticKind diagnostic = AgentStatusDiagnosticKind::io_error;
    std::error_code system_error;
    std::optional<double> modified_at_seconds, observed_at_seconds;
    std::optional<std::size_t> byte_count;
};
struct AgentStatusRuntimeFacts { std::optional<std::string> state;
    std::optional<bool> running; std::optional<std::int64_t> pid; std::optional<double> state_changed_at;
    std::optional<double> last_progress_at, no_progress_seconds;
};
struct AgentStatusActiveTurnFacts {
    std::optional<std::string> kind; std::optional<std::string> id;
    std::optional<double> started_at, elapsed_seconds;
};
struct AgentStatusContextFacts {
    std::int64_t window_size = 0;
    std::optional<std::int64_t> system_tokens, tools_tokens, history_tokens;
    std::optional<std::int64_t> total_tokens, fixed_tokens, growing_tokens;
    std::optional<double> usage_percent;
};
struct AgentStatusFacts {
    std::optional<std::string> agent_id; AgentStatusRuntimeFacts runtime;
    std::optional<AgentStatusActiveTurnFacts> active_turn;
    std::optional<AgentStatusContextFacts> context;
};
enum class AgentManifestStatusMtimeOrder { status_newer, manifest_newer,
    same_time, unassessable };
enum class AgentManifestStatusAgreement { agree, disagree, unassessable };
struct AgentManifestStatusRelation {
    AgentManifestStatusMtimeOrder mtime_order = AgentManifestStatusMtimeOrder::unassessable;
    AgentManifestStatusAgreement agent_id = AgentManifestStatusAgreement::unassessable;
    AgentManifestStatusAgreement state = AgentManifestStatusAgreement::unassessable;
};
struct AgentIdentityStatusItem {
    std::optional<AgentStatusFacts> status; AgentStatusSource status_source;
    AgentManifestStatusRelation relation;
};
struct AgentIdentityStatusSnapshot {
    AgentRosterSnapshot roster;
    // Index-parallel to roster.discovery.items exactly when complete.
    std::vector<AgentIdentityStatusItem> detail_by_item;
    std::optional<double> observed_at_seconds;
    bool projection_complete = false;
};
// One read-only composite: one discovery and one shared wall-clock sample.
[[nodiscard]] AgentIdentityStatusSnapshot project_agent_identity_status(
    const ProjectAttachment &attachment) noexcept;
} // namespace lingtai::desktop
