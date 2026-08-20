#include "message_reactions.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace lingtai::desktop;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_receipt_monotonic() {
    MessageReactionStore store;
    store.set_receipt("m1", ReceiptStage::received);
    auto bag = store.get("m1");
    expect(bag.list.size() == 1
            && bag.list[0].id.key == "receipt.received"
            && reaction_glyph(bag.list[0].id) == "📧",
        "received stamps the mail glyph");

    store.set_receipt("m1", ReceiptStage::seen);
    bag = store.get("m1");
    expect(bag.list.size() == 1
            && bag.list[0].id.key == "receipt.received",
        "seen is catalog-only and must not replace received");

    store.set_receipt("m1", ReceiptStage::replied);
    bag = store.get("m1");
    expect(bag.list.size() == 1
            && bag.list[0].id.key == "receipt.replied"
            && reaction_glyph(bag.list[0].id) == "✔️",
        "replied replaces received on the same message");

    store.set_receipt("m1", ReceiptStage::received);
    bag = store.get("m1");
    expect(bag.list.size() == 1
            && bag.list[0].id.key == "receipt.replied",
        "receipts are monotonic and never move backwards");
}

void test_peer_reactions_allowed() {
    MessageReactionStore store;
    store.set_receipt("m1", ReceiptStage::received);
    store.upsert_peer_reaction("m1", ReactionId{"👍"}, "alpha");
    store.upsert_peer_reaction("m1", ReactionId{"👍"}, "beta");
    store.upsert_peer_reaction("m1", ReactionId{"👍"}, "alpha");
    store.upsert_peer_reaction("m1", ReactionId{"receipt.received"}, "alpha");

    const auto bag = store.get("m1");
    expect(bag.list.size() == 2, "receipt stays first; one peer chip aggregates");
    expect(bag.list[0].source == ReactionSource::system_receipt
            && bag.list[0].id.key == "receipt.received",
        "system receipt remains the leading chip");
    expect(bag.list[1].source == ReactionSource::peer
            && bag.list[1].id.key == "👍"
            && bag.list[1].count == 2,
        "two distinct peer reactors share one emoji count");
}

void test_history_upgrade_only_existing_receipts() {
    MessageReactionStore store;
    std::vector<DirectConversationMessage> messages = {
        {.id = "out-1", .outgoing = true, .text = "hello"},
        {.id = "in-1", .outgoing = false, .text = "hi"},
        {.id = "out-2", .outgoing = true, .text = "again"},
    };

    sync_receipts_from_history(store, messages);
    expect(store.get("out-1").empty() && store.get("out-2").empty(),
        "history alone must not invent receipts");

    store.set_receipt("out-1", ReceiptStage::received);
    sync_receipts_from_history(store, messages);
    expect(store.get("out-1").list.size() == 1
            && store.get("out-1").list[0].id.key == "receipt.replied",
        "a later inbound upgrades an existing receipt to replied");
    expect(store.get("out-2").empty(),
        "outgoing without a later inbound stays without a invented receipt");
}

void test_session_clear() {
    MessageReactionStore store;
    store.set_receipt("m1", ReceiptStage::received);
    store.upsert_peer_reaction("m2", ReactionId{"🎉"}, "alpha");
    store.clear();
    expect(store.all().empty(), "clear drops the whole session map");
}

} // namespace

int main() {
    test_receipt_monotonic();
    test_peer_reactions_allowed();
    test_history_upgrade_only_existing_receipts();
    test_session_clear();
    if (failures != 0) {
        std::cerr << failures << " message reaction assertion(s) failed\n";
        return 1;
    }
    std::cout << "MESSAGE_REACTIONS_OK\n";
    return 0;
}
