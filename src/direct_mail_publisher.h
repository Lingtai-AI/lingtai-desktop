#pragma once

#include "direct_conversation_route.h"

#include <string>

namespace lingtai::desktop {

// Two states only: the mail either reached the human outbox or it did not.
// The kernel poller's later pickup, delivery, and any reply are outside this
// slice; nothing here observes or infers them.
enum class DirectMailSendResult { queued, failed_local };

// Publishes one plain-text human outbox entry for the route's target, using
// the same final-directory-then-atomic-JSON pattern the current Go TUI
// pseudo-agent sender uses: an exclusively created `outbox/<id>` directory,
// then one write-temp-then-rename of `message.json` inside it. It never
// writes the target inbox or human `sent/`, never stages outside `outbox/`,
// and never retries or reuses an id across calls. `text` must already be
// validated non-empty by the caller; this function performs no UI-level
// validation.
[[nodiscard]] DirectMailSendResult send_direct_mail(
    const DirectConversationRoute &route, const std::string &text) noexcept;

} // namespace lingtai::desktop
