#pragma once

#include "agent_manifest_discovery_test_seam.h"
#include "agent_roster_presence.h"

#include <string>

namespace lingtai::desktop::testing {
enum class AgentHeartbeatFileReadKind {
    absent, read, unsafe_symlink, not_regular, unreadable, io_error,
    too_large, container_unavailable, container_changed, source_changed };

struct AgentHeartbeatFileRead {
    AgentHeartbeatFileReadKind kind = AgentHeartbeatFileReadKind::io_error;
    std::string bytes;
    std::error_code error;
};

class AgentRosterPresenceFilesystem :
        public AgentManifestDiscoveryFilesystem {
public:
    virtual AgentHeartbeatFileRead read_heartbeat(
        const std::filesystem::path &root,
        const std::filesystem::path &directory_key) const = 0;
};

class AgentRosterPresenceClock {
public:
    virtual ~AgentRosterPresenceClock() = default;
    virtual double wall_now_seconds() const = 0;
};

// Real descriptor reader with injected wall time.
[[nodiscard]] AgentRosterSnapshot project_agent_roster(
    const ProjectAttachment &attachment,
    const AgentRosterPresenceClock &clock) noexcept;

// Deterministic boundary outcomes; the algorithm still selects safe keys.
[[nodiscard]] AgentRosterSnapshot project_agent_roster(
    const ProjectAttachment &attachment,
    const AgentRosterPresenceFilesystem &filesystem,
    const AgentRosterPresenceClock &clock) noexcept;

} // namespace lingtai::desktop::testing
