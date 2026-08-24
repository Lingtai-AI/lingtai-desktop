#pragma once

#include "direct_conversation_history.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lingtai::desktop {

// Stable reaction identity. Display glyphs come from the catalog; keys stay
// independent of emoji so receipts and peer reactions share one bag.
struct ReactionId {
    std::string key;

    [[nodiscard]] bool empty() const noexcept { return key.empty(); }
    explicit operator bool() const noexcept { return !empty(); }

    friend bool operator==(const ReactionId &, const ReactionId &) = default;
};

enum class ReactionSource {
    system_receipt,
    peer,
};

struct MessageReaction {
    ReactionId id;
    int count = 1;
    bool mine = false;
    ReactionSource source = ReactionSource::peer;
    // Peer reactor key when known (agent directory key / address); empty for
    // system receipts and anonymous peer aggregates.
    std::string reactor;
};

struct MessageReactions {
    std::vector<MessageReaction> list;

    [[nodiscard]] bool empty() const noexcept { return list.empty(); }
};

enum class ReceiptStage {
    none,
    received,
    seen,
    replied,
};

struct ReactionDef {
    ReactionId id;
    const char *glyph = "";
    ReactionSource source = ReactionSource::peer;
    bool system_only = false;
};

[[nodiscard]] const ReactionDef *reaction_def(const ReactionId &id) noexcept;
[[nodiscard]] ReactionId receipt_reaction_id(ReceiptStage stage) noexcept;
[[nodiscard]] ReceiptStage receipt_stage_for(const ReactionId &id) noexcept;
[[nodiscard]] std::string reaction_glyph(const ReactionId &id);

// Session-scoped reaction bags keyed by mailbox message id. Never written to
// message.json; cleared when the conversation session ends.
class MessageReactionStore {
public:
    void clear() noexcept;
    [[nodiscard]] MessageReactions get(const std::string &message_id) const;
    [[nodiscard]] const std::unordered_map<std::string, MessageReactions> &
        all() const noexcept { return by_message_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // Monotonic system receipt on a Human message: received → seen → replied.
    void set_receipt(const std::string &message_id, ReceiptStage stage);

    // Peer reaction slot: same id from the same reactor is idempotent; a new
    // reactor increments count. System receipt chips are left untouched.
    void upsert_peer_reaction(
        const std::string &message_id,
        const ReactionId &id,
        const std::string &reactor_key);

private:
    std::unordered_map<std::string, MessageReactions> by_message_;
    std::uint64_t revision_ = 0;
};

// Upgrades existing receipts to replied when a later inbound message exists.
// Does not invent received/seen from history alone (session-only received
// comes from a successful local send). inspected_messages is an optional
// deterministic work counter: one history row inspected by the reverse pass
// contributes one.
void sync_receipts_from_history(
    MessageReactionStore &store,
    const std::vector<DirectConversationMessage> &messages,
    std::size_t *inspected_messages = nullptr);

// Upgrades an existing received receipt to seen when that mailbox id was
// named in a committed notification_block_injected email lane. Does not
// invent receipts.
void sync_seen_from_injected(
    MessageReactionStore &store,
    const std::unordered_set<std::string> &injected_ids);

} // namespace lingtai::desktop
