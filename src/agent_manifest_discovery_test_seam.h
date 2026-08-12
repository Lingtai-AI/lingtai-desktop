#pragma once

#include "agent_manifest_discovery.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
namespace lingtai::desktop::testing {
struct AgentManifestDirectoryListing {
    enum class Kind { complete, missing, not_directory, unsafe_symlink,
        unreadable, io_error };
    Kind kind = Kind::io_error;
    std::vector<std::filesystem::path> entries;
    std::error_code error;
};
enum class AgentManifestFileReadKind { candidate_absent,
    candidate_not_directory, candidate_unsafe_symlink, candidate_unreadable,
    candidate_io_error, manifest_absent, read, unsafe_symlink, not_regular,
    unreadable, io_error, too_large };

struct AgentManifestFileRead { AgentManifestFileReadKind kind = AgentManifestFileReadKind::io_error;
    std::string bytes; std::error_code error; };

class AgentManifestDiscoveryFilesystem {
public:
    virtual ~AgentManifestDiscoveryFilesystem() = default;
    virtual AgentManifestDirectoryListing list_directory(
        const std::filesystem::path &path) const = 0;
    virtual AgentManifestFileRead read_manifest(
        const std::filesystem::path &root,
        const std::filesystem::path &directory_key) const = 0;
};

// Deterministic error/race seam; the algorithm still selects every path.
[[nodiscard]] AgentManifestDiscoveryReport discover_agent_manifests(
    const ProjectAttachment &attachment,
    const AgentManifestDiscoveryFilesystem &filesystem) noexcept;

} // namespace lingtai::desktop::testing
