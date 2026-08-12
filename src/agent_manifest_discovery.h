#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
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

struct AgentManifestLlmFacts {
    std::optional<std::string> provider, model, base_url, api_compat;
    std::optional<std::int64_t> context_limit;
};

enum class AgentManifestCapabilityEvidenceKind { absent, parsed,
    partially_parsed, invalid };

struct AgentManifestCapabilityFacts {
    AgentManifestCapabilityEvidenceKind evidence = AgentManifestCapabilityEvidenceKind::absent;
    // Parsed names retain manifest order and duplicates as source evidence.
    std::vector<std::string> manifest_names;
    // Intrinsics first, then de-duplicated manifest names.
    std::vector<std::string> display_names;
};

struct AgentManifestIdentityFacts {
    std::optional<std::string> agent_id, true_name, nickname, address, state;
    AgentManifestLlmFacts llm;
    AgentManifestCapabilityFacts capabilities;
};

struct AgentManifestSource { std::filesystem::path path;
    AgentManifestObservationState observation = AgentManifestObservationState::observed_unavailable;
    AgentManifestDiagnosticKind diagnostic = AgentManifestDiagnosticKind::io_error;
    std::error_code system_error;
    std::filesystem::path relative_path;
    std::optional<double> modified_at_seconds;
    std::optional<double> observed_at_seconds;
    std::optional<std::size_t> byte_count;
};

struct AgentManifestDiscoveryItem {
    // Lossless immediate name; authoritative even without agent_id.
    std::filesystem::path directory_key;
    std::filesystem::path directory_path;
    AgentManifestKind manifest_kind = AgentManifestKind::malformed;
    AgentRole role = AgentRole::unknown;
    AgentManifestSource manifest_source;
    std::optional<AgentManifestIdentityFacts> identity;
};

struct AgentManifestDiscoveryReport { AgentManifestScanSource scan;
    std::vector<AgentManifestScanDiagnostic> scan_diagnostics;
    std::vector<AgentManifestDiscoveryItem> items; };

// Reads only <canonical attachment root>/.lingtai and its immediate children.
[[nodiscard]] AgentManifestDiscoveryReport discover_agent_manifests(
    const ProjectAttachment &attachment) noexcept;

} // namespace lingtai::desktop
