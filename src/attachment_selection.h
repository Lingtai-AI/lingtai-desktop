#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace lingtai::desktop {

inline constexpr std::uint64_t kAttachmentPerFileLimitBytes =
    25ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kAttachmentTotalLimitBytes =
    100ULL * 1024ULL * 1024ULL;

enum class AttachmentMediaKind { image, file };

enum class AttachmentRejectionReason {
    missing,
    not_regular,
    unreadable,
    per_file_limit,
    total_limit,
    duplicate,
    local_failure,
};

struct AcceptedAttachment {
    std::filesystem::path source_path;
    std::string display_filename;
    std::uint64_t byte_size = 0;
    AttachmentMediaKind media_kind = AttachmentMediaKind::file;
};

struct RejectedAttachment {
    std::filesystem::path input_path;
    AttachmentRejectionReason reason =
        AttachmentRejectionReason::local_failure;
    std::error_code system_error;
};

struct AttachmentSelectionResult {
    std::vector<AcceptedAttachment> accepted;
    std::vector<RejectedAttachment> rejected;
    std::uint64_t accepted_bytes = 0;
};

// Establishes current local facts for a selection. This is not publication
// authorization: the publisher must revalidate every source before copying.
[[nodiscard]] AttachmentSelectionResult preflight_attachments(
    const std::vector<std::filesystem::path> &selected_paths) noexcept;

} // namespace lingtai::desktop
