#pragma once

#include <filesystem>
#include <optional>
#include <system_error>

namespace lingtai::desktop {

enum class ProjectPathFailure {
    none,
    selection_not_found,
    selection_not_directory,
    invalid_relative_path,
    absolute_path_forbidden,
    parent_traversal_forbidden,
    target_not_found,
    target_not_directory,
    outside_project,
    filesystem_error,
};

struct ProjectPathResult {
    std::filesystem::path path;
    ProjectPathFailure failure = ProjectPathFailure::none;
    std::error_code system_error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return failure == ProjectPathFailure::none;
    }
};

struct ProjectAttachmentResult;

class ProjectAttachment final {
public:
    [[nodiscard]] const std::filesystem::path &root() const noexcept;
    [[nodiscard]] ProjectPathResult resolve(
        const std::filesystem::path &relative_path) const noexcept;

private:
    explicit ProjectAttachment(std::filesystem::path canonical_root);

    std::filesystem::path root_;

    friend ProjectAttachmentResult attach_project(
        const std::filesystem::path &) noexcept;
};

struct ProjectAttachmentResult {
    std::optional<ProjectAttachment> attachment;
    ProjectPathFailure failure = ProjectPathFailure::none;
    std::error_code system_error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return attachment.has_value();
    }
};

[[nodiscard]] ProjectAttachmentResult attach_project(
    const std::filesystem::path &selected_directory) noexcept;

} // namespace lingtai::desktop
