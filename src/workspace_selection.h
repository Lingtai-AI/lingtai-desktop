#pragma once

#include "project_attachment.h"

#include <filesystem>
#include <optional>

namespace lingtai::desktop {

enum class AgentSelectionResult {
    selected,
    no_active_project,
    invalid_directory_key,
};

// C1 is the sole owner of active workspace and Agent-directory selection
// truth, and the sole same-root/root-switch transition owner: activating a
// project clears the Agent selection exactly when the canonical root changes.
// C5 proposes transitions only through this model.
class WorkspaceSelectionState final {
public:
    WorkspaceSelectionState() noexcept = default;

    void activate_project(ProjectAttachment attachment);
    [[nodiscard]] AgentSelectionResult select_agent(
        std::filesystem::path directory_key);
    void clear_agent_selection() noexcept;

    [[nodiscard]] const std::optional<ProjectAttachment> &active_project()
        const noexcept;
    [[nodiscard]] const std::optional<std::filesystem::path> &
        selected_agent_directory_key() const noexcept;

private:
    std::optional<ProjectAttachment> active_project_;
    std::optional<std::filesystem::path> selected_agent_directory_key_;
};

} // namespace lingtai::desktop
