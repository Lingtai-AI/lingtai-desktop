#include "attachment_selection.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

struct SourceIdentity {
    dev_t device = 0;
    ino_t inode = 0;

    bool operator==(const SourceIdentity &) const = default;
};

[[nodiscard]] AttachmentRejectionReason path_rejection(
        const std::error_code &error) {
    if (error == std::errc::no_such_file_or_directory
        || error == std::errc::not_a_directory) {
        return AttachmentRejectionReason::missing;
    }
    if (error == std::errc::permission_denied
        || error == std::errc::operation_not_permitted) {
        return AttachmentRejectionReason::unreadable;
    }
    return AttachmentRejectionReason::local_failure;
}

[[nodiscard]] AttachmentMediaKind classify_media_kind(
        const fs::path &display_path) {
    auto extension = display_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    static constexpr std::array<const char *, 6> kImageExtensions = {
        ".gif", ".heic", ".jpeg", ".jpg", ".png", ".webp",
    };
    return std::find(kImageExtensions.begin(), kImageExtensions.end(), extension)
            != kImageExtensions.end()
        ? AttachmentMediaKind::image
        : AttachmentMediaKind::file;
}

void reject(
        AttachmentSelectionResult &result,
        const fs::path &input_path,
        AttachmentRejectionReason reason,
        std::error_code error = {}) {
    result.rejected.push_back({input_path, reason, error});
}

} // namespace

AttachmentSelectionResult preflight_attachments(
        const std::vector<fs::path> &selected_paths) noexcept {
    try {
        AttachmentSelectionResult result;
        result.accepted.reserve(selected_paths.size());
        result.rejected.reserve(selected_paths.size());
        std::vector<SourceIdentity> seen_sources;
        seen_sources.reserve(selected_paths.size());

        for (const auto &selected_path : selected_paths) {
            std::error_code error;
            const auto source_path = fs::canonical(selected_path, error);
            if (error) {
                reject(result, selected_path, path_rejection(error), error);
                continue;
            }

            const auto descriptor = ::open(source_path.c_str(),
                O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
            if (descriptor < 0) {
                error = std::error_code(errno, std::generic_category());
                reject(result, selected_path, path_rejection(error), error);
                continue;
            }

            struct stat status {};
            const auto stat_result = ::fstat(descriptor, &status);
            const auto stat_error = stat_result == 0
                ? std::error_code{}
                : std::error_code(errno, std::generic_category());
            ::close(descriptor);
            if (stat_result != 0) {
                reject(result, selected_path,
                    AttachmentRejectionReason::local_failure, stat_error);
                continue;
            }
            if (!S_ISREG(status.st_mode)) {
                reject(result, selected_path,
                    AttachmentRejectionReason::not_regular);
                continue;
            }
            if (status.st_size < 0) {
                reject(result, selected_path,
                    AttachmentRejectionReason::local_failure,
                    std::make_error_code(std::errc::value_too_large));
                continue;
            }

            const auto identity = SourceIdentity{status.st_dev, status.st_ino};
            if (std::find(seen_sources.begin(), seen_sources.end(), identity)
                != seen_sources.end()) {
                reject(result, selected_path,
                    AttachmentRejectionReason::duplicate);
                continue;
            }
            seen_sources.push_back(identity);

            const auto byte_size = static_cast<std::uint64_t>(status.st_size);
            if (byte_size > kAttachmentPerFileLimitBytes) {
                reject(result, selected_path,
                    AttachmentRejectionReason::per_file_limit);
                continue;
            }
            if (byte_size > kAttachmentTotalLimitBytes - result.accepted_bytes) {
                reject(result, selected_path,
                    AttachmentRejectionReason::total_limit);
                continue;
            }

            result.accepted.push_back({
                source_path,
                selected_path.filename().string(),
                byte_size,
                classify_media_kind(selected_path),
            });
            result.accepted_bytes += byte_size;
        }
        return result;
    } catch (...) {
        // Allocation and path-encoding failures cannot produce a trustworthy
        // partial selection. Fail closed and let the caller keep text-only
        // sending available.
        return {};
    }
}

} // namespace lingtai::desktop
