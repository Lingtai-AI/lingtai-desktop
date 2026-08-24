#include "direct_conversation_attachment_actions.h"

#include "posix_descriptor_primitives.h"

#include <algorithm>
#include <array>
#include <sys/stat.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

[[nodiscard]] bool accepted_folder(const std::string &folder) {
    constexpr auto folders = std::array{"inbox", "sent", "outbox"};
    return std::ranges::find(folders, folder) != folders.end();
}

[[nodiscard]] bool same_identity(
        const DirectConversationAttachment &left,
        const DirectConversationAttachment &right) {
    return left.display_filename == right.display_filename
        && left.byte_size == right.byte_size
        && left.media_kind == right.media_kind
        && left.device_id == right.device_id
        && left.inode_id == right.inode_id;
}

} // namespace

std::optional<fs::path> revalidate_direct_conversation_attachment(
        const DirectConversationRoute &route,
        const DirectConversationAttachmentRequest &request) noexcept {
    try {
        // Refresh first: an outbox entry may now be in sent, and attachment
        // order or identity may have changed since the document was painted.
        const auto history = read_direct_conversation(route);
        const auto message = std::ranges::find_if(
            history.messages,
            [&](const auto &candidate) {
                return candidate.id == request.message_id;
            });
        if (message == history.messages.end()
            || request.attachment_index >= message->attachments.size()) {
            return std::nullopt;
        }
        const auto &current = message->attachments[request.attachment_index];
        if (!same_identity(current, request.presented)
            || !accepted_folder(message->mailbox_folder)
            || !posix::safe_leaf(route.human_directory_key)
            || !posix::safe_leaf(fs::path(message->id))
            || !posix::safe_leaf(fs::path(current.display_filename))) {
            return std::nullopt;
        }

        const auto root = posix::open_root_directory(route.project_root);
        if (root.get() < 0) return std::nullopt;
        const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
        if (lingtai.get() < 0) return std::nullopt;
        const auto human = posix::open_directory_component(
            lingtai.get(), route.human_directory_key);
        if (human.get() < 0) return std::nullopt;
        const auto mailbox = posix::open_directory_component(human.get(), "mailbox");
        if (mailbox.get() < 0) return std::nullopt;
        const auto folder = posix::open_directory_component(
            mailbox.get(), message->mailbox_folder);
        if (folder.get() < 0) return std::nullopt;
        const auto entry = posix::open_directory_component(folder.get(), message->id);
        if (entry.get() < 0) return std::nullopt;
        const auto attachments = posix::open_directory_component(
            entry.get(), "attachments");
        if (attachments.get() < 0) return std::nullopt;
        const auto file = posix::open_regular_file_component(
            attachments.get(), current.display_filename);
        if (file.get() < 0) return std::nullopt;
        struct stat opened {};
        if (::fstat(file.get(), &opened) != 0 || opened.st_size < 0
            || static_cast<std::uint64_t>(opened.st_dev) != current.device_id
            || static_cast<std::uint64_t>(opened.st_ino) != current.inode_id
            || static_cast<std::uint64_t>(opened.st_size) != current.byte_size) {
            return std::nullopt;
        }
        return (route.project_root / ".lingtai"
            / route.human_directory_key / "mailbox"
            / message->mailbox_folder / message->id / "attachments"
            / current.display_filename).lexically_normal();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace lingtai::desktop
