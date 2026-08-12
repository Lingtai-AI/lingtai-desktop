#pragma once

#include "agent_manifest_discovery.h"

#include <optional>

namespace lingtai::desktop {
class ProjectAttachment;

enum class AgentPresenceKind {
    unknown, alive_human, alive, stale, missing, invalid, unavailable };

enum class AgentHeartbeatObservationState {
    not_applicable, read_this_scan, absent, rejected_unsafe,
    observed_unavailable };

enum class AgentHeartbeatDiagnosticKind {
    none, invalid_number, nonfinite, future, unsafe_symlink, not_regular,
    unreadable, io_error, too_large, container_unavailable,
    container_changed, source_changed };

struct AgentHeartbeatSource {
    std::filesystem::path path;
    AgentHeartbeatObservationState observation =
        AgentHeartbeatObservationState::not_applicable;
    AgentHeartbeatDiagnosticKind diagnostic =
        AgentHeartbeatDiagnosticKind::none;
    std::error_code system_error;
};

struct AgentRosterPresenceItem {
    AgentPresenceKind presence = AgentPresenceKind::unknown;
    std::optional<double> timestamp_seconds;
    std::optional<double> age_seconds;
    AgentHeartbeatSource heartbeat_source;
};

struct AgentRosterSnapshot {
    AgentManifestDiscoveryReport discovery;
    std::vector<AgentRosterPresenceItem> presence_by_item;
    bool projection_complete = false;
};

// One read-only snapshot: discovery once, then one wall-clock observation.
[[nodiscard]] AgentRosterSnapshot project_agent_roster(
    const ProjectAttachment &attachment) noexcept;

} // namespace lingtai::desktop
