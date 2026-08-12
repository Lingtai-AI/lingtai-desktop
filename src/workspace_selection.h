#pragma once

#include "project_attachment.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace lingtai::desktop {

enum class AgentSelectionResult {
    selected,
    no_active_project,
    invalid_directory_key,
};

// C1 is the sole owner of active workspace and Agent-directory selection
// truth. C5 proposes transitions only through this model.
class WorkspaceSelectionState final {
public:
    explicit WorkspaceSelectionState(
        std::size_t maximum_recent_projects) noexcept;

    void activate_project(ProjectAttachment attachment);
    void close_workspace() noexcept;
    [[nodiscard]] AgentSelectionResult select_agent(
        std::filesystem::path directory_key);
    void clear_agent_selection() noexcept;
    void remove_recent_project(
        const std::filesystem::path &canonical_root);

    [[nodiscard]] const std::optional<ProjectAttachment> &active_project()
        const noexcept;
    [[nodiscard]] const std::optional<std::filesystem::path> &
        selected_agent_directory_key() const noexcept;
    [[nodiscard]] const std::vector<std::filesystem::path> &
        recent_project_roots() const noexcept;

private:
    std::size_t maximum_recent_projects_;
    std::optional<ProjectAttachment> active_project_;
    std::optional<std::filesystem::path> selected_agent_directory_key_;
    std::vector<std::filesystem::path> recent_project_roots_;
};

} // namespace lingtai::desktop
