#include "project_attachment.h"

#include <utility>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

ProjectPathFailure resolution_failure(const std::error_code &error) {
    if (error == std::errc::no_such_file_or_directory) {
        return ProjectPathFailure::target_not_found;
    }
    if (error == std::errc::not_a_directory) {
        return ProjectPathFailure::target_not_directory;
    }
    return ProjectPathFailure::filesystem_error;
}

ProjectPathFailure attachment_failure_type(const std::error_code &error) {
    if (error == std::errc::no_such_file_or_directory) {
        return ProjectPathFailure::selection_not_found;
    }
    if (error == std::errc::not_a_directory) {
        return ProjectPathFailure::selection_not_directory;
    }
    return ProjectPathFailure::filesystem_error;
}

bool is_contained(const fs::path &candidate, const fs::path &root) {
    auto candidate_part = candidate.begin();
    for (auto root_part = root.begin(); root_part != root.end();
         ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end()
            || *candidate_part != *root_part) {
            return false;
        }
    }
    return true;
}

ProjectPathResult path_failure(
        ProjectPathFailure failure,
        std::error_code error = {}) {
    return {.path = {}, .failure = failure, .system_error = error};
}

ProjectAttachmentResult attachment_failure(
        ProjectPathFailure failure,
        std::error_code error = {}) {
    return {
        .attachment = std::nullopt,
        .failure = failure,
        .system_error = error,
    };
}

std::error_code unknown_filesystem_error() {
    return std::make_error_code(std::errc::io_error);
}

} // namespace

ProjectAttachment::ProjectAttachment(fs::path canonical_root)
: root_(std::move(canonical_root)) {
}

const fs::path &ProjectAttachment::root() const noexcept {
    return root_;
}

ProjectPathResult ProjectAttachment::resolve(
        const fs::path &relative_path) const noexcept {
    try {
        if (relative_path.empty()) {
            return path_failure(ProjectPathFailure::invalid_relative_path);
        }
        if (relative_path.is_absolute()
            || relative_path.has_root_name()
            || relative_path.has_root_directory()) {
            return path_failure(ProjectPathFailure::absolute_path_forbidden);
        }
        auto has_target_component = false;
        for (const auto &part : relative_path) {
            if (part == "..") {
                return path_failure(
                    ProjectPathFailure::parent_traversal_forbidden);
            }
            if (part != "." && !part.empty()) {
                has_target_component = true;
            }
        }
        if (!has_target_component) {
            return path_failure(ProjectPathFailure::invalid_relative_path);
        }

        std::error_code error;
        const auto resolved = fs::canonical(root_ / relative_path, error);
        if (error) {
            return path_failure(resolution_failure(error), error);
        }
        if (!is_contained(resolved, root_)) {
            return path_failure(ProjectPathFailure::outside_project);
        }
        return {
            .path = resolved,
            .failure = ProjectPathFailure::none,
            .system_error = {},
        };
    } catch (const fs::filesystem_error &error) {
        return path_failure(ProjectPathFailure::filesystem_error, error.code());
    } catch (...) {
        return path_failure(
            ProjectPathFailure::filesystem_error,
            unknown_filesystem_error());
    }
}

ProjectAttachmentResult attach_project(
        const fs::path &selected_directory) noexcept {
    try {
        std::error_code error;
        const auto status = fs::status(selected_directory, error);
        if (error) {
            return attachment_failure(attachment_failure_type(error), error);
        }
        if (!fs::exists(status)) {
            return attachment_failure(
                ProjectPathFailure::selection_not_found);
        }
        if (!fs::is_directory(status)) {
            return attachment_failure(
                ProjectPathFailure::selection_not_directory);
        }

        auto root = fs::canonical(selected_directory, error);
        if (error) {
            return attachment_failure(attachment_failure_type(error), error);
        }
        return {
            .attachment = ProjectAttachment(std::move(root)),
            .failure = ProjectPathFailure::none,
            .system_error = {},
        };
    } catch (const fs::filesystem_error &error) {
        return attachment_failure(
            ProjectPathFailure::filesystem_error,
            error.code());
    } catch (...) {
        return attachment_failure(
            ProjectPathFailure::filesystem_error,
            unknown_filesystem_error());
    }
}

} // namespace lingtai::desktop
