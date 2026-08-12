#include "workspace_selection.h"

#include <algorithm>
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

WorkspaceSelectionState::WorkspaceSelectionState(
        std::size_t maximum_recent_projects) noexcept
: maximum_recent_projects_(maximum_recent_projects) {
}

void WorkspaceSelectionState::activate_project(ProjectAttachment attachment) {
    auto root = attachment.root();
    const auto same_root = active_project_
        && active_project_->root() == root;
    active_project_ = std::move(attachment);
    if (!same_root) {
        selected_agent_directory_key_.reset();
    }

    std::erase(recent_project_roots_, root);
    if (maximum_recent_projects_ == 0) {
        return;
    }
    recent_project_roots_.insert(
        recent_project_roots_.begin(), std::move(root));
    if (recent_project_roots_.size() > maximum_recent_projects_) {
        recent_project_roots_.resize(maximum_recent_projects_);
    }
}

void WorkspaceSelectionState::close_workspace() noexcept {
    selected_agent_directory_key_.reset();
    active_project_.reset();
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

void WorkspaceSelectionState::remove_recent_project(
        const std::filesystem::path &canonical_root) {
    std::erase(recent_project_roots_, canonical_root);
}

const std::optional<ProjectAttachment> &
WorkspaceSelectionState::active_project() const noexcept {
    return active_project_;
}

const std::optional<std::filesystem::path> &
WorkspaceSelectionState::selected_agent_directory_key() const noexcept {
    return selected_agent_directory_key_;
}

const std::vector<std::filesystem::path> &
WorkspaceSelectionState::recent_project_roots() const noexcept {
    return recent_project_roots_;
}

} // namespace lingtai::desktop
