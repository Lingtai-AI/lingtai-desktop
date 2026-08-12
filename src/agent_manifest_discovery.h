#pragma once

#include <filesystem>
#include <system_error>
#include <vector>
namespace lingtai::desktop {
class ProjectAttachment;

enum class AgentManifestScanState { complete, root_missing, root_not_directory,
    root_unsafe_symlink, root_unreadable, root_io_error };

struct AgentManifestScanSource { std::filesystem::path path;
    AgentManifestScanState state = AgentManifestScanState::root_io_error;
    std::error_code system_error; };

enum class AgentManifestScanDiagnosticKind { child_unsafe_symlink,
    child_unreadable, child_io_error };

struct AgentManifestScanDiagnostic {
    AgentManifestScanDiagnosticKind kind = AgentManifestScanDiagnosticKind::child_io_error;
    std::filesystem::path path; std::error_code system_error; };

enum class AgentManifestKind { valid, malformed, unsafe };
enum class AgentRole { unknown, human, main, agent };

enum class AgentManifestObservationState { read_this_scan,
    observed_unavailable, rejected_unsafe };

enum class AgentManifestDiagnosticKind { none, unsafe_symlink, not_regular,
    unreadable, io_error, invalid_json, not_object, too_large };

struct AgentManifestSource { std::filesystem::path path;
    AgentManifestObservationState observation = AgentManifestObservationState::observed_unavailable;
    AgentManifestDiagnosticKind diagnostic = AgentManifestDiagnosticKind::io_error;
    std::error_code system_error;
};

struct AgentManifestDiscoveryItem {
    // Lossless immediate name; authoritative even without agent_id.
    std::filesystem::path directory_key;
    std::filesystem::path directory_path;
    AgentManifestKind manifest_kind = AgentManifestKind::malformed;
    AgentRole role = AgentRole::unknown;
    AgentManifestSource manifest_source;
};

struct AgentManifestDiscoveryReport { AgentManifestScanSource scan;
    std::vector<AgentManifestScanDiagnostic> scan_diagnostics;
    std::vector<AgentManifestDiscoveryItem> items; };

// Reads only <canonical attachment root>/.lingtai and its immediate children.
[[nodiscard]] AgentManifestDiscoveryReport discover_agent_manifests(
    const ProjectAttachment &attachment) noexcept;

} // namespace lingtai::desktop
