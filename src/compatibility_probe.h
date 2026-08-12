#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace lingtai::desktop {
class ProjectAttachment;
enum class CompatibilityDisposition { compatible, degraded, blocked };
enum class CompatibilityFindingKind {
    probe_failed,
    no_agent_requested,
    agent_unavailable,
    receipt_missing, receipt_unreadable, receipt_malformed,
    receipt_schema_unsupported, receipt_version_unsupported,
    resolved_missing, resolved_unreadable, resolved_malformed,
    resolved_schema_unsupported, resolved_version_unsupported,
    resolved_source_unsupported, resolved_manifest_invalid,
    resolved_stale, resolved_freshness_unavailable,
    init_missing, init_unreadable, init_malformed, init_not_object,
    init_manifest_missing, init_manifest_not_object,
    capabilities_shape_unknown, legacy_bash_alias, capability_conflict,
};
struct CompatibilityFinding { CompatibilityFindingKind kind =
    CompatibilityFindingKind::probe_failed; };
struct InstallReceiptFacts { std::string install_method, stamped_version,
    resolved_commit, kernel_source, kernel_version; };
struct ResolvedManifestFacts { bool fresh = true; };
enum class CapabilityAlias { none, shell_only, bash_only, equal, conflict,
    shape_unknown };
struct RawInitFacts { CapabilityAlias capability_alias = CapabilityAlias::none; };
struct CompatibilityReport {
    CompatibilityDisposition disposition = CompatibilityDisposition::blocked;
    bool commands_allowed = false;
    std::vector<CompatibilityFinding> findings;
    std::optional<InstallReceiptFacts> install_receipt;
    std::optional<ResolvedManifestFacts> resolved_manifest;
    std::optional<RawInitFacts> raw_init;
};
// No home/project receipt is inferred; no requested agent is Degraded/fail-closed.
[[nodiscard]] CompatibilityReport probe_compatibility(
    const ProjectAttachment &attachment,
    const std::optional<std::filesystem::path> &agent_relative_directory,
    const std::filesystem::path &install_receipt_path) noexcept;
} // namespace lingtai::desktop
