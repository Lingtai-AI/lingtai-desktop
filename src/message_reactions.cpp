#include "message_reactions.h"

#include <algorithm>
#include <unordered_set>

namespace lingtai::desktop {
namespace {

constexpr auto kReceiptReceived = "receipt.received";
constexpr auto kReceiptSeen = "receipt.seen";
constexpr auto kReceiptReplied = "receipt.replied";

const ReactionDef kCatalog[] = {
    {{kReceiptReceived}, "📧", ReactionSource::system_receipt, true},
    {{kReceiptSeen}, "👀", ReactionSource::system_receipt, true},
    {{kReceiptReplied}, "✔️", ReactionSource::system_receipt, true},
};

[[nodiscard]] int receipt_rank(ReceiptStage stage) noexcept {
    switch (stage) {
    case ReceiptStage::none: return 0;
    case ReceiptStage::received: return 1;
    case ReceiptStage::seen: return 2;
    case ReceiptStage::replied: return 3;
    }
    return 0;
}

[[nodiscard]] ReceiptStage highest_receipt(
        const MessageReactions &bag) noexcept {
    auto best = ReceiptStage::none;
    for (const auto &entry : bag.list) {
        if (entry.source != ReactionSource::system_receipt) continue;
        const auto stage = receipt_stage_for(entry.id);
        if (receipt_rank(stage) > receipt_rank(best)) {
            best = stage;
        }
    }
    return best;
}

void erase_receipts(MessageReactions &bag) {
    bag.list.erase(
        std::remove_if(bag.list.begin(), bag.list.end(),
            [](const MessageReaction &entry) {
                return entry.source == ReactionSource::system_receipt;
            }),
        bag.list.end());
}

void ensure_receipt_first(MessageReactions &bag, ReceiptStage stage) {
    if (stage == ReceiptStage::none) {
        return;
    }
    erase_receipts(bag);
    MessageReaction receipt;
    receipt.id = receipt_reaction_id(stage);
    receipt.count = 1;
    receipt.mine = true;
    receipt.source = ReactionSource::system_receipt;
    bag.list.insert(bag.list.begin(), std::move(receipt));
}

} // namespace

const ReactionDef *reaction_def(const ReactionId &id) noexcept {
    if (id.empty()) return nullptr;
    for (const auto &entry : kCatalog) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

ReactionId receipt_reaction_id(ReceiptStage stage) noexcept {
    switch (stage) {
    case ReceiptStage::received: return {kReceiptReceived};
    case ReceiptStage::seen: return {kReceiptSeen};
    case ReceiptStage::replied: return {kReceiptReplied};
    case ReceiptStage::none: return {};
    }
    return {};
}

ReceiptStage receipt_stage_for(const ReactionId &id) noexcept {
    if (id.key == kReceiptReceived) return ReceiptStage::received;
    if (id.key == kReceiptSeen) return ReceiptStage::seen;
    if (id.key == kReceiptReplied) return ReceiptStage::replied;
    return ReceiptStage::none;
}

std::string reaction_glyph(const ReactionId &id) {
    if (const auto *def = reaction_def(id)) {
        return def->glyph;
    }
    // Peer emoji keys may be the glyph itself until a richer catalog exists.
    return id.key;
}

void MessageReactionStore::clear() noexcept {
    by_message_.clear();
}

MessageReactions MessageReactionStore::get(
        const std::string &message_id) const {
    const auto found = by_message_.find(message_id);
    if (found == by_message_.end()) return {};
    return found->second;
}

void MessageReactionStore::set_receipt(
        const std::string &message_id, ReceiptStage stage) {
    if (message_id.empty() || stage == ReceiptStage::none) return;

    auto &bag = by_message_[message_id];
    const auto current = highest_receipt(bag);
    if (receipt_rank(stage) < receipt_rank(current)) return;
    ensure_receipt_first(bag, stage);
}

void MessageReactionStore::upsert_peer_reaction(
        const std::string &message_id,
        const ReactionId &id,
        const std::string &reactor_key) {
    if (message_id.empty() || id.empty()) return;
    if (const auto *def = reaction_def(id); def && def->system_only) {
        return;
    }

    auto &bag = by_message_[message_id];
    for (auto &entry : bag.list) {
        if (entry.source != ReactionSource::peer || entry.id != id) {
            continue;
        }
        if (!reactor_key.empty() && entry.reactor == reactor_key) {
            return;
        }
        if (reactor_key.empty() && entry.reactor.empty()) {
            return;
        }
        ++entry.count;
        return;
    }

    MessageReaction peer;
    peer.id = id;
    peer.count = 1;
    peer.mine = false;
    peer.source = ReactionSource::peer;
    peer.reactor = reactor_key;
    bag.list.push_back(std::move(peer));
}

void sync_receipts_from_history(
        MessageReactionStore &store,
        const std::vector<DirectConversationMessage> &messages,
        std::size_t *inspected_messages) {
    if (inspected_messages) *inspected_messages = 0;
    auto has_later_inbound = false;
    for (auto cursor = messages.rbegin(); cursor != messages.rend(); ++cursor) {
        if (inspected_messages) ++*inspected_messages;
        const auto &message = *cursor;
        if (!message.outgoing) {
            has_later_inbound = true;
            continue;
        }
        if (!has_later_inbound || message.id.empty()) continue;
        const auto existing = store.get(message.id);
        if (highest_receipt(existing) == ReceiptStage::none) {
            // No session send receipt yet — do not invent received/seen from
            // history alone; only upgrade when a receipt already exists.
            continue;
        }
        store.set_receipt(message.id, ReceiptStage::replied);
    }
}

void sync_seen_from_injected(
        MessageReactionStore &store,
        const std::unordered_set<std::string> &injected_ids) {
    for (const auto &id : injected_ids) {
        if (id.empty()) continue;
        if (highest_receipt(store.get(id)) != ReceiptStage::received) continue;
        store.set_receipt(id, ReceiptStage::seen);
    }
}

} // namespace lingtai::desktop
