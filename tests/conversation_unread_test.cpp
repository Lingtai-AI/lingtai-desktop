#include "conversation_unread.h"

#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

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

void test_first_observation_seeds_history_at_zero() {
    ConversationUnreadSession session;
    const auto history = sample_history();
    const auto project = std::filesystem::path("/projects/one");
    expect(session.observe(project, "alpha", history, false),
        "first observation creates the session cursor");
    const auto snapshot = session.snapshot(
        project, std::vector<std::string>{"alpha"});
    expect(snapshot.total == 0 && !snapshot.counts.contains("alpha"),
        "historical inbound is seeded as read on first observation");
}

void test_new_inbound_increments_only_its_agent() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    static_cast<void>(session.observe(project, "beta", initial, false));

    auto grown = initial;
    grown.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "new",
    });
    expect(session.observe(project, "alpha", grown, false),
        "a newer inbound observation changes unread state");
    expect(!session.observe(project, "alpha", grown, false),
        "an equal-tip duplicate observation is idempotent");
    const auto snapshot = session.snapshot(
        project, std::vector<std::string>{"alpha", "beta"});
    expect(snapshot.total == 1 && snapshot.counts.at("alpha") == 1
            && !snapshot.counts.contains("beta"),
        "new inbound increments only the observed Agent");
}

void test_viewed_observation_catches_up() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    auto grown = initial;
    grown.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "new",
    });
    static_cast<void>(session.observe(project, "alpha", grown, false));

    expect(session.observe(project, "alpha", grown, true),
        "viewing the latest accepted history advances the read cursor");
    const auto snapshot = session.snapshot(
        project, std::vector<std::string>{"alpha"});
    expect(snapshot.total == 0 && snapshot.counts.empty(),
        "a genuine view clears the current unread count");
}

void test_older_duplicate_history_cannot_regress_state() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    auto newest = initial;
    newest.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "fourth",
    });
    newest.messages.push_back({
        .id = "a5",
        .outgoing = false,
        .timestamp = "2026-01-01T10:05:00Z",
        .text = "fifth",
    });
    static_cast<void>(session.observe(project, "alpha", newest, false));

    expect(!session.observe(project, "alpha", initial, true),
        "an older duplicate-window observation is ignored even when viewed");
    const auto snapshot = session.snapshot(
        project, std::vector<std::string>{"alpha"});
    expect(snapshot.total == 2 && snapshot.counts.at("alpha") == 2,
        "older history cannot regress the accepted unread count or cursor");
}

void test_duplicate_project_requests_count_agent_once() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    auto grown = initial;
    grown.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "new",
    });
    static_cast<void>(session.observe(project, "alpha", grown, false));
    const auto requests = std::vector<OpenProjectUnreadRequest>{
        {.canonical_project_root = project, .valid_agent_keys = {"alpha"}},
        {.canonical_project_root = project, .valid_agent_keys = {"alpha"}},
    };
    expect(session.total_for(requests) == 1,
        "duplicate windows for one Project/Agent count only once");
    expect(session.snapshot(
            project, std::vector<std::string>{"alpha", "alpha"}).total == 1,
        "duplicate valid-Agent keys count only once in a Project snapshot");
}

void test_outgoing_and_receipt_adjacent_changes_do_not_count() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    auto changed = initial;
    changed.messages.push_back({
        .id = "human-2",
        .outgoing = true,
        .timestamp = "2026-01-01T10:05:00Z",
        .text = "outgoing",
    });
    changed.skipped = 1;
    changed.skipped_attachments = 2;
    expect(!session.observe(project, "alpha", changed, false),
        "outgoing and receipt-adjacent metadata do not change inbound state");
    expect(session.snapshot(project, std::vector<std::string>{"alpha"}).total
            == 0,
        "outgoing and reaction-receipt-adjacent data do not count");
}

void test_same_agent_key_is_independent_across_projects() {
    ConversationUnreadSession session;
    const auto first = std::filesystem::path("/projects/one");
    const auto second = std::filesystem::path("/projects/two");
    const auto initial = sample_history();
    static_cast<void>(session.observe(first, "alpha", initial, false));
    static_cast<void>(session.observe(second, "alpha", initial, false));
    auto grown = initial;
    grown.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "first project only",
    });
    static_cast<void>(session.observe(first, "alpha", grown, false));
    const auto requests = std::vector<OpenProjectUnreadRequest>{
        {.canonical_project_root = first, .valid_agent_keys = {"alpha"}},
        {.canonical_project_root = second, .valid_agent_keys = {"alpha"}},
    };
    expect(session.total_for(requests) == 1
            && session.snapshot(first, std::vector<std::string>{"alpha"}).total
                == 1
            && session.snapshot(second, std::vector<std::string>{"alpha"}).total
                == 0,
        "the same Agent key has independent cursors in different Projects");
}

void test_close_excludes_without_erasing_reopen_cursor() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    expect(session.total_for(std::vector<OpenProjectUnreadRequest>{}) == 0,
        "a Project with no open window is excluded from the process total");

    auto after_close = initial;
    after_close.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "arrived while closed",
    });
    static_cast<void>(session.observe(project, "alpha", after_close, false));
    const auto reopened = std::vector<OpenProjectUnreadRequest>{
        {.canonical_project_root = project, .valid_agent_keys = {"alpha"}},
    };
    expect(session.total_for(reopened) == 1,
        "same-session reopen counts mail after the preserved cursor");
}

void test_removed_agent_is_filtered_and_readd_preserves_state() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    auto grown = initial;
    grown.messages.push_back({
        .id = "a4",
        .outgoing = false,
        .timestamp = "2026-01-01T10:04:00Z",
        .text = "new",
    });
    static_cast<void>(session.observe(project, "alpha", grown, false));
    expect(session.snapshot(project, std::vector<std::string>{}).total == 0,
        "a removed Agent is filtered from the current Project snapshot");
    expect(session.snapshot(project, std::vector<std::string>{"alpha"}).total
            == 1,
        "same-session re-add exposes the preserved Agent cursor state");
}

void test_equal_timestamps_are_ordered_by_message_id() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    auto initial = DirectConversationHistory{};
    initial.messages.push_back({
        .id = "same-a",
        .outgoing = false,
        .timestamp = "2026-01-01T10:00:00Z",
        .text = "first",
    });
    static_cast<void>(session.observe(project, "alpha", initial, false));
    auto grown = initial;
    grown.messages.push_back({
        .id = "same-b",
        .outgoing = false,
        .timestamp = "2026-01-01T10:00:00Z",
        .text = "second",
    });
    static_cast<void>(session.observe(project, "alpha", grown, false));
    expect(session.snapshot(project, std::vector<std::string>{"alpha"}).total
            == 1,
        "equal timestamps use the message ID as the cursor tie-break");
}

void test_total_addition_saturates() {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    expect(saturating_unread_add(maximum - 2, 7) == maximum,
        "unread total addition saturates instead of wrapping");
    expect(saturating_unread_add(40, 2) == 42,
        "ordinary unread total addition remains exact");
}

void test_clear_resets_process_session() {
    ConversationUnreadSession session;
    const auto project = std::filesystem::path("/projects/one");
    const auto initial = sample_history();
    static_cast<void>(session.observe(project, "alpha", initial, false));
    session.clear();
    expect(session.observe(project, "alpha", initial, false),
        "clear removes the prior cursor so the next observation reseeds");
    expect(session.snapshot(project, std::vector<std::string>{"alpha"}).total
            == 0,
        "the reseeded history is read after clearing the session");
}

} // namespace

int main() {
    test_tip_and_count();
    test_first_observation_seeds_history_at_zero();
    test_new_inbound_increments_only_its_agent();
    test_viewed_observation_catches_up();
    test_older_duplicate_history_cannot_regress_state();
    test_duplicate_project_requests_count_agent_once();
    test_outgoing_and_receipt_adjacent_changes_do_not_count();
    test_same_agent_key_is_independent_across_projects();
    test_close_excludes_without_erasing_reopen_cursor();
    test_removed_agent_is_filtered_and_readd_preserves_state();
    test_equal_timestamps_are_ordered_by_message_id();
    test_total_addition_saturates();
    test_clear_resets_process_session();
    if (failures != 0) {
        std::cerr << failures << " conversation_unread failure(s)\n";
        return 1;
    }
    std::cout << "conversation_unread: OK\n";
    return 0;
}
