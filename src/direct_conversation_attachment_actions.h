#pragma once

#include "direct_conversation_history.h"
#include "direct_conversation_route.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace lingtai::desktop {

// One presentation-originated request. The identity facts are observations
// captured when the card was rendered, never authority on their own.
struct DirectConversationAttachmentRequest {
    std::string message_id;
    std::size_t attachment_index = 0;
    DirectConversationAttachment presented;
};

// Resolves the message again from the current route, then descriptor-walks the
// current entry's attachments directory no-follow and compares the reopened
// regular file with every presented identity fact. Only that final successful
// check yields the path supplied immediately to the shell's external action.
[[nodiscard]] std::optional<std::filesystem::path>
revalidate_direct_conversation_attachment(
    const DirectConversationRoute &route,
    const DirectConversationAttachmentRequest &request) noexcept;

} // namespace lingtai::desktop
