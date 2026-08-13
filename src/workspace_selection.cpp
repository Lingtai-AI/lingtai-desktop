#include "workspace_selection.h"

#include <utility>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

bool is_safe_directory_key(const fs::path &key) {
    return !key.empty() && !key.is_absolute() && !key.has_root_name()
        && !key.has_root_directory() && key == key.filename()
        && key != "." && key != ".."
        && key.native().find(fs::path::value_type{})
            == fs::path::string_type::npos;
}

} // namespace

void WorkspaceSelectionState::activate_project(ProjectAttachment attachment) {
    const auto same_root = active_project_
        && active_project_->root() == attachment.root();
    active_project_ = std::move(attachment);
    if (!same_root) {
        selected_agent_directory_key_.reset();
    }
}

AgentSelectionResult WorkspaceSelectionState::select_agent(
        std::filesystem::path directory_key) {
    if (!active_project_) {
        return AgentSelectionResult::no_active_project;
    }
    if (!is_safe_directory_key(directory_key)) {
        return AgentSelectionResult::invalid_directory_key;
    }
    selected_agent_directory_key_ = std::move(directory_key);
    return AgentSelectionResult::selected;
}

void WorkspaceSelectionState::clear_agent_selection() noexcept {
    selected_agent_directory_key_.reset();
}

const std::optional<ProjectAttachment> &
WorkspaceSelectionState::active_project() const noexcept {
    return active_project_;
}

const std::optional<std::filesystem::path> &
WorkspaceSelectionState::selected_agent_directory_key() const noexcept {
    return selected_agent_directory_key_;
}

} // namespace lingtai::desktop
