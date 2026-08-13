#pragma once

#include "project_attachment.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {

enum class AgentPresetSummarySource {
    resolved,
    not_yet_published,
    stale,
    unavailable,
};

// One exact published allowed reference string, with independent
// active/default badges derived by exact equality against the same
// artifact's own `preset.active`/`preset.default`.
struct AgentPresetRef {
    std::string ref;
    bool is_active = false;
    bool is_default = false;
};

// The active preset's own narrow effective facts. Never populated for an
// inactive allowed ref: this projection never opens one.
struct AgentPresetEffective {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::int64_t> context_limit;
    std::vector<std::string> capability_names; // sorted only for stable display
};

// One stateless read-only projection of the selected Agent's own kernel-
// published `<selected>/system/manifest.resolved.json`
// (`schema=lingtai.manifest.resolved/v1`, `source=kernel`). Meaningful
// fields are populated only when `source` is `resolved` or `stale`.
struct AgentPresetSummary {
    AgentPresetSummarySource source = AgentPresetSummarySource::unavailable;
    std::optional<std::string> active_ref;
    std::optional<std::string> default_ref;
    std::vector<AgentPresetRef> allowed; // preserves published order
    AgentPresetEffective effective;
    std::optional<std::string> generated_at;
};

// Reads only the exact v1 envelope and narrow allowlisted paths of the
// selected Agent's own resolved-manifest artifact: root `schema`,
// `schema_version`, `source`, `generated_at`; root `preset.active/default/
// allowed`, preserving exact strings and order; `manifest.llm.provider/
// model`; `manifest.context_limit`; and top-level names from
// `manifest.capabilities`. A supported complete envelope with an artifact
// mtime not older than the selected Agent's own no-follow `init.json`
// mtime projects `resolved`; a supported complete envelope strictly older
// than that mtime projects `stale`; a missing artifact (or missing
// `system` component) projects `not_yet_published`; anything unsafe,
// unreadable, oversized, malformed, unsupported, or incomplete projects
// `unavailable` with no partial fields. `init.json` is descriptor-opened
// only for that one mtime comparison -- its content is never read or
// parsed, and no allowed ref is ever dereferenced. Every walked path
// component is opened one no-follow leaf at a time. Stateless: every call
// is one independent observation, and it writes nothing.
[[nodiscard]] AgentPresetSummary read_agent_preset_summary(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

} // namespace lingtai::desktop
