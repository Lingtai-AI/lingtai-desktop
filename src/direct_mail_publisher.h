#pragma once

#include "attachment_selection.h"
#include "direct_conversation_route.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace lingtai::desktop {

// Two states only: the mail either reached the human outbox or it did not.
// The kernel poller's later pickup, delivery, and any reply are outside this
// slice; nothing here observes or infers them.
enum class DirectMailSendResult { queued, failed_local };

enum class DirectMailFailureReason {
    none,
    empty_content,
    unsafe_route,
    mailbox_unavailable,
    attachment_invalid_source,
    attachment_missing,
    attachment_unreadable,
    attachment_symlink,
    attachment_not_regular,
    attachment_replaced,
    attachment_size_changed,
    attachment_per_file_limit,
    attachment_total_limit,
    unsafe_attachment_name,
    attachment_destination_failure,
    attachment_copy_failure,
    payload_failure,
    publish_failure,
    local_failure,
};

struct DirectMailAttachmentFailure {
    std::size_t index = 0;
    std::filesystem::path source_path;
};

struct DirectMailSendOutcome {
    DirectMailSendResult result = DirectMailSendResult::failed_local;
    DirectMailFailureReason failure_reason =
        DirectMailFailureReason::local_failure;
    std::optional<DirectMailAttachmentFailure> attachment_failure;
    std::error_code system_error;
    // Non-empty only when result is queued: the outbox leaf directory id.
    std::string message_id;
};

// Publishes one human outbox entry for the route's target, using
// the same final-directory-then-atomic-JSON pattern the current Go TUI
// pseudo-agent sender uses: an exclusively created `outbox/<id>` directory,
// then one write-temp-then-rename of `message.json` inside it. It never
// writes the target inbox or human `sent/`, never stages outside `outbox/`,
// and never reuses an id across calls. `text` is the caller-provided body;
// this function performs no UI-level trimming. Accepted attachment facts are
// revalidated and copied before
// `message.json` is published. An empty attachment vector preserves the exact
// text-only envelope; empty text is valid only with at least one attachment.
[[nodiscard]] DirectMailSendOutcome send_direct_mail(
    const DirectConversationRoute &route,
    const std::string &text,
    const std::vector<AcceptedAttachment> &attachments = {}) noexcept;

} // namespace lingtai::desktop
