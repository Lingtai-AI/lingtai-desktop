#pragma once

#include "direct_conversation_route.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lingtai::desktop {

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
};

// Reads only the immediate `inbox`, `outbox`, and `sent` entries under
// `<route project root>/.lingtai/<route human directory key>/mailbox`, and
// only their immediate `message.json` files. It follows no symlink, reads no
// attachment, and writes nothing.
[[nodiscard]] DirectConversationHistory read_direct_conversation(
    const DirectConversationRoute &route) noexcept;

} // namespace lingtai::desktop
