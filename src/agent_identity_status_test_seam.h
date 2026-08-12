#pragma once
#include "agent_identity_status.h"
#include "agent_roster_presence_test_seam.h"
#include <optional>
#include <string>
#include <utility>
namespace lingtai::desktop::testing {
enum class AgentStatusFileReadKind { absent, read, unsafe_symlink, not_regular,
    unreadable, io_error, too_large, container_unavailable, container_changed,
    source_changed };
struct AgentStatusFileRead {
    AgentStatusFileReadKind kind = AgentStatusFileReadKind::io_error;
    std::string bytes; std::error_code error;
    std::optional<double> modified_at_seconds;
    AgentStatusFileRead() = default;
    AgentStatusFileRead(AgentStatusFileReadKind read_kind, std::string read_bytes,
        std::error_code read_error, std::optional<double> modified = std::nullopt)
        : kind(read_kind), bytes(std::move(read_bytes)), error(read_error),
          modified_at_seconds(modified) {}
};
class AgentIdentityStatusFilesystem : public AgentRosterPresenceFilesystem {
public:
    virtual AgentStatusFileRead read_status(const std::filesystem::path &root,
        const std::filesystem::path &directory_key) const = 0;
};
[[nodiscard]] AgentIdentityStatusSnapshot project_agent_identity_status(
    const ProjectAttachment &attachment,
    const AgentIdentityStatusFilesystem &filesystem,
    const AgentRosterPresenceClock &clock) noexcept;
} // namespace lingtai::desktop::testing
