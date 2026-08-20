#include "conversation_unread.h"

#include <iostream>
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

DirectConversationHistory sample_history() {
    DirectConversationHistory history;
    history.messages = {
        {.id = "a1", .outgoing = false, .timestamp = "2026-01-01T10:00:00Z",
            .text = "hi"},
        {.id = "b1", .outgoing = true, .timestamp = "2026-01-01T10:01:00Z",
            .text = "hello"},
        {.id = "a2", .outgoing = false, .timestamp = "2026-01-01T10:02:00Z",
            .text = "again"},
        {.id = "a3", .outgoing = false, .timestamp = "2026-01-01T10:03:00Z",
            .text = "third"},
    };
    return history;
}

void test_tip_and_count() {
    const auto history = sample_history();
    const auto tip = tip_inbound_cursor(history);
    expect(tip.id == "a3" && tip.timestamp == "2026-01-01T10:03:00Z",
        "tip is the latest inbound message");
    expect(count_inbound_after_cursor(history, tip) == 0,
        "nothing is after the tip");

    ConversationReadCursor after_first{
        "2026-01-01T10:00:00Z", "a1"};
    expect(count_inbound_after_cursor(history, after_first) == 2,
        "two inbound messages follow the first inbound");
}

void test_session_catch_up_and_unseen() {
    ConversationUnreadState state;
    const auto history = sample_history();
    expect(state.unseen_inbound_count("alpha", history) == 0,
        "missing cursor reports zero (caller seeds first)");

    state.catch_up("alpha", history);
    expect(state.unseen_inbound_count("alpha", history) == 0,
        "catch-up clears unseen against the same history");

    DirectConversationHistory grown = history;
    grown.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "new",
    });
    expect(state.unseen_inbound_count("alpha", grown) == 1,
        "one new inbound after catch-up is unseen");

    state.catch_up("alpha", grown);
    expect(state.unseen_inbound_count("alpha", grown) == 0,
        "catch-up after reading clears the badge again");

    state.clear();
    expect(!state.has_cursor("alpha"),
        "clear drops every cursor");
}

} // namespace

int main() {
    test_tip_and_count();
    test_session_catch_up_and_unseen();
    if (failures != 0) {
        std::cerr << failures << " conversation_unread failure(s)\n";
        return 1;
    }
    std::cout << "conversation_unread: OK\n";
    return 0;
}
