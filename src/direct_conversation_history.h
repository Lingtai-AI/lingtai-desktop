#pragma once

#include "attachment_selection.h"
#include "direct_conversation_route.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lingtai::desktop {

// Read-only metadata established from a descriptor-opened regular file owned
// by one mailbox entry. It is a history observation, not authorization for a
// later open/reveal action, which must revalidate the path again.
struct DirectConversationAttachment {
    std::filesystem::path local_path;
    std::string display_filename;
    std::uint64_t byte_size = 0;
    AttachmentMediaKind media_kind = AttachmentMediaKind::file;
};

// One accepted direct message, already reduced to exactly what the selected-
// Agent conversation surface renders. Delivery, processing, reply chains, and
// unread state are deliberately not modelled: nothing here is inferred.
struct DirectConversationMessage {
    // The mailbox entry directory basename: the stable, displayed message ID.
    std::string id;
    bool outgoing = false;
    // The kernel's own envelope timestamp, preserved exactly as written.
    std::string timestamp;
    std::string subject;
    // The envelope's `message` field. The kernel never names this `body`.
    std::string text;
    // Valid entries retain the envelope array's order, including duplicates.
    std::vector<DirectConversationAttachment> attachments;
};

// A message.json larger than this is rejected unread; a conversation entry has
// no legitimate reason to approach it.
inline constexpr std::size_t direct_message_byte_limit = std::size_t{1} << 20;

struct DirectConversationHistory {
    std::vector<DirectConversationMessage> messages;
    // One generic count of entries that were unsafe, unreadable, or not a
    // well-formed envelope. Well-formed mail belonging to another conversation
    // is simply absent and is never counted here.
    std::size_t skipped = 0;
    // Invalid attachment elements/fields are counted independently and never
    // turn an otherwise valid envelope into a skipped message.
    std::size_t skipped_attachments = 0;
};

// Reads only the immediate `inbox`, `outbox`, and `sent` entries under
// `<route project root>/.lingtai/<route human directory key>/mailbox`, and
// only their immediate `message.json` files. It follows no symlink, reads no
// attachment content, and writes nothing. Attachment metadata is rooted to
// and descriptor-validated beneath each current mailbox entry.
[[nodiscard]] DirectConversationHistory read_direct_conversation(
    const DirectConversationRoute &route) noexcept;

} // namespace lingtai::desktop
