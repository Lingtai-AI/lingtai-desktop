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
    expect(journal.ids().empty(),
        "first poll skips bytes already in events.jsonl");

    {
        std::ofstream out(journal_path, std::ios::app);
        out << R"({"type":"notification_block_injected","_meta":{"agent_meta":{"notifications":{"persistent":{"email":{"email_ids":["new"]}}}}}})" << '\n';
    }
    journal.poll(root, "worker");
    expect(journal.ids().count("new") == 1 && journal.ids().count("old") == 0,
        "later polls collect only newly appended injected mail ids");
    fs::remove_all(root);
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
