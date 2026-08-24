#include "message_reactions.h"
#include "injected_mail_journal.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_set>

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
            && bag.list[0].id.key == "receipt.seen"
            && reaction_glyph(bag.list[0].id) == "👀",
        "seen replaces received after injection");

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

void test_history_receipt_reconciliation_is_linear_and_preserves_semantics() {
    MessageReactionStore store;
    std::vector<DirectConversationMessage> messages;
    constexpr auto kWorstPatternOutgoing = std::size_t{2048};
    messages.reserve(kWorstPatternOutgoing + 6);
    for (auto index = std::size_t{0}; index != kWorstPatternOutgoing; ++index) {
        messages.push_back({
            .id = "prefix-" + std::to_string(index),
            .outgoing = true,
            .text = "prefix",
        });
        if (index % 2 == 0) {
            store.set_receipt(messages.back().id, ReceiptStage::received);
        }
    }
    store.set_receipt("prefix-0", ReceiptStage::replied);
    messages.push_back({.id = "in-1", .outgoing = false, .text = "reply"});
    messages.push_back({.id = "between", .outgoing = true, .text = "again"});
    store.set_receipt("between", ReceiptStage::seen);
    messages.push_back({.id = "", .outgoing = true, .text = "empty id"});
    messages.push_back({.id = "in-2", .outgoing = false, .text = "reply again"});
    messages.push_back({.id = "tail", .outgoing = true, .text = "last"});
    store.set_receipt("tail", ReceiptStage::received);

    std::vector<std::string> order_before;
    order_before.reserve(messages.size());
    for (const auto &message : messages) order_before.push_back(message.id);

    auto inspected_messages = std::size_t{0};
    sync_receipts_from_history(store, messages, &inspected_messages);

    expect(inspected_messages == messages.size(),
        "history receipt reconciliation inspects each message exactly once");
    expect(store.get("prefix-0").list[0].id.key == "receipt.replied",
        "an already replied receipt remains replied");
    expect(store.get("prefix-2").list[0].id.key == "receipt.replied",
        "an eligible existing receipt is upgraded to replied");
    expect(store.get("prefix-1").empty(),
        "an eligible message without a receipt stays without one");
    expect(store.get("between").list[0].id.key == "receipt.replied",
        "alternating boundaries upgrade an outgoing before the next inbound");
    expect(store.get("").empty(), "an empty message id is ignored");
    expect(store.get("tail").list[0].id.key == "receipt.received",
        "an outgoing after the last inbound is not upgraded");

    std::vector<std::string> order_after;
    order_after.reserve(messages.size());
    for (const auto &message : messages) order_after.push_back(message.id);
    expect(order_after == order_before,
        "history receipt reconciliation preserves message order");
}

void test_seen_from_injected_ids_only_existing_receipts() {
    MessageReactionStore store;
    std::unordered_set<std::string> injected{"out-1", "never-sent"};
    sync_seen_from_injected(store, injected);
    expect(store.get("out-1").empty() && store.get("never-sent").empty(),
        "injection alone must not invent receipts");

    store.set_receipt("out-1", ReceiptStage::received);
    store.set_receipt("out-2", ReceiptStage::received);
    store.set_receipt("out-3", ReceiptStage::replied);
    sync_seen_from_injected(store, injected);
    expect(store.get("out-1").list.size() == 1
            && store.get("out-1").list[0].id.key == "receipt.seen",
        "injected id upgrades an existing received receipt");
    expect(store.get("out-2").list[0].id.key == "receipt.received",
        "uninjected existing receipt stays received");
    expect(store.get("out-3").list[0].id.key == "receipt.replied",
        "replied is not moved back to seen");
}

void test_injected_mail_ids_from_persistent_lane() {
    std::unordered_set<std::string> ids;
    collect_injected_mail_ids_from_event_json(
        R"({"type":"tool_call","_meta":{"agent_meta":{"notifications":{"persistent":{"email":{"email_ids":["nope"]}}}}}})",
        ids);
    expect(ids.empty(), "non-injection events must not yield mail ids");

    collect_injected_mail_ids_from_event_json(
        R"({"type":"notification_block_injected","_meta":{"agent_meta":{"notifications":{"persistent":{"email":{"email_ids":["mail-1"],"emails":[{"id":"mail-2"}]}}}}}})",
        ids);
    expect(ids.count("mail-1") == 1 && ids.count("mail-2") == 1,
        "persistent.email ids from a committed injection are collected");
}

void test_injected_mail_journal_tails_only_new_lines() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / ("lingtai-injected-mail-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const auto logs = root / ".lingtai" / "worker" / "logs";
    fs::create_directories(logs);
    const auto journal_path = logs / "events.jsonl";
    {
        std::ofstream out(journal_path);
        out << R"({"type":"notification_block_injected","_meta":{"agent_meta":{"notifications":{"persistent":{"email":{"email_ids":["old"]}}}}}})" << '\n';
    }

    InjectedMailJournal journal;
    journal.poll(root, "worker");
    expect(journal.ids().empty() && journal.revision() == 0,
        "first poll skips bytes already in events.jsonl");

    {
        std::ofstream out(journal_path, std::ios::app);
        out << R"({"type":"notification_block_injected","_meta":{"agent_meta":{"notifications":{"persistent":{"email":{"email_ids":["new"]}}}}}})" << '\n';
    }
    journal.poll(root, "worker");
    const auto seen_revision = journal.revision();
    journal.poll(root, "worker");
    expect(journal.ids().count("new") == 1 && journal.ids().count("old") == 0
            && seen_revision == 1 && journal.revision() == seen_revision,
        "later polls collect only newly appended ids and idle polls are "
        "revision-idempotent");
    journal.reset();
    expect(journal.ids().empty() && journal.revision() == seen_revision + 1,
        "resetting visible injected ids advances revision once");
    fs::remove_all(root);
}

void test_session_clear() {
    MessageReactionStore store;
    const auto empty_revision = store.revision();
    store.clear();
    expect(store.revision() == empty_revision,
        "clear-on-empty is revision-idempotent");
    store.set_receipt("m1", ReceiptStage::received);
    const auto received_revision = store.revision();
    store.set_receipt("m1", ReceiptStage::received);
    store.set_receipt("m1", ReceiptStage::none);
    expect(store.revision() == received_revision,
        "duplicate/downgrade receipt calls do not advance revision");
    store.set_receipt("m1", ReceiptStage::seen);
    expect(store.revision() == received_revision + 1,
        "a real receipt upgrade advances revision once");
    store.upsert_peer_reaction("m2", ReactionId{"🎉"}, "alpha");
    const auto reaction_revision = store.revision();
    store.upsert_peer_reaction("m2", ReactionId{"🎉"}, "alpha");
    expect(store.revision() == reaction_revision,
        "a duplicate peer reaction is revision-idempotent");
    store.clear();
    expect(store.all().empty() && store.revision() == reaction_revision + 1,
        "a real clear drops the map and advances revision once");
}

} // namespace

int main() {
    test_receipt_monotonic();
    test_peer_reactions_allowed();
    test_history_receipt_reconciliation_is_linear_and_preserves_semantics();
    test_seen_from_injected_ids_only_existing_receipts();
    test_injected_mail_ids_from_persistent_lane();
    test_injected_mail_journal_tails_only_new_lines();
    test_session_clear();
    if (failures != 0) {
        std::cerr << failures << " message reaction assertion(s) failed\n";
        return 1;
    }
    std::cout << "MESSAGE_REACTIONS_OK\n";
    return 0;
}
