#include "direct_conversation_history.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::DirectConversationHistory;
using lingtai::desktop::DirectConversationRoute;
using lingtai::desktop::read_direct_conversation;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const fs::path &path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "fixture parent must be created: " + path.string());
    auto stream = std::ofstream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture must be written: " + path.string());
}

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// Exact byte/type image of the fixture, so "reading never writes" is proven
// against the real tree rather than asserted.
std::map<std::string, std::string> tree_snapshot(const fs::path &root) {
    auto result = std::map<std::string, std::string>();
    if (!fs::exists(root)) return result;
    for (const auto &entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied)) {
        const auto key = entry.path().lexically_relative(root).generic_string();
        const auto status = entry.symlink_status();
        if (fs::is_symlink(status)) {
            result[key] = "symlink:" + fs::read_symlink(entry.path()).string();
        } else if (fs::is_directory(status)) {
            result[key] = "directory";
        } else if (fs::is_regular_file(status)) {
            result[key] = "file:" + read_file(entry.path());
        } else {
            result[key] = "other";
        }
    }
    return result;
}

// The real kernel envelope shape, including fields this slice ignores and a
// misleading `body` that must never be read in place of `message`.
std::string envelope(
        std::string_view from,
        std::string_view to,
        std::string_view subject,
        std::string_view message,
        std::string_view timestamp_key,
        std::string_view timestamp,
        std::string_view identity_agent_id = "") {
    auto identity = std::string();
    if (!identity_agent_id.empty()) {
        identity = R"(,"identity":{"agent_id":")"
            + std::string(identity_agent_id)
            + R"(","agent_name":"x","address":"x","state":"active"})";
    }
    return std::string(R"({"from":")") + std::string(from)
        + R"(","to":[")" + std::string(to)
        + R"("],"subject":")" + std::string(subject)
        + R"(","message":")" + std::string(message)
        + R"(","body":"MISLEADING LEGACY BODY FIELD")"
        + R"(,"type":"normal","mode":"peer")" + identity
        + R"(,"_mailbox_id":"ignored-in-favour-of-directory-name")"
        + R"(,"attachments":["../escape/should-never-be-touched"])"
        + R"(,")" + std::string(timestamp_key) + R"(":")"
        + std::string(timestamp) + R"("})";
}

DirectConversationRoute route_for(const fs::path &project_root) {
    DirectConversationRoute route;
    route.human_directory_key = "human";
    route.target_directory_key = "telegram-bot";
    route.human_address = "human";
    route.target_address = "telegram-bot";
    route.thread_key.project_root = project_root;
    route.thread_key.target_agent_id = "20260712-191609-d0c8";
    return route;
}

fs::path mailbox_of(const fs::path &project_root) {
    return project_root / ".lingtai" / "human" / "mailbox";
}

std::vector<std::string> ids_of(const DirectConversationHistory &history) {
    auto result = std::vector<std::string>();
    for (const auto &message : history.messages) result.push_back(message.id);
    return result;
}

// The whole reader behavior on real kernel entries: an incoming and an outgoing
// message both render from `message`, in chronological order, while mail that
// belongs to another conversation stays out and nothing is written.
void verify_incoming_and_outgoing_pair(const fs::path &sandbox) {
    const auto project = sandbox / "pair";
    const auto mailbox = mailbox_of(project);
    write_file(mailbox / "inbox" / "20260807T184852-0d13" / "message.json",
        envelope("telegram-bot", "human", "Slice done",
            "Ted, the slice is complete.", "received_at",
            "2026-08-07T18:48:52Z", "20260712-191609-d0c8"));
    write_file(mailbox / "sent" / "20260807T190000-aa01" / "message.json",
        envelope("human", "telegram-bot", "Re: Slice done",
            "Thanks, merging tomorrow.", "sent_at", "2026-08-07T19:00:00Z"));
    // Another Agent's mail sits in the same human mailbox.
    write_file(mailbox / "inbox" / "20260807T185000-zz99" / "message.json",
        envelope("codex", "human", "Unrelated", "Another conversation.",
            "received_at", "2026-08-07T18:50:00Z"));

    const auto before = tree_snapshot(project);
    const auto history = read_direct_conversation(route_for(project));
    require(tree_snapshot(project) == before,
        "reading the conversation must not write to the project");

    require(ids_of(history) == std::vector<std::string>{
                "20260807T184852-0d13", "20260807T190000-aa01"},
        "exactly the direct pair renders, earliest first");
    require(history.skipped == 0,
        "another conversation's mail is absent, not an error");
    require(!history.messages[0].outgoing && history.messages[1].outgoing,
        "inbox entries are incoming and sent entries are outgoing");
    require(history.messages[0].timestamp == "2026-08-07T18:48:52Z"
            && history.messages[1].timestamp == "2026-08-07T19:00:00Z",
        "each row keeps its own envelope timestamp");
    require(history.messages[0].subject == "Slice done", "subject must render");
    require(history.messages[0].text == "Ted, the slice is complete."
            && history.messages[1].text == "Thanks, merging tomorrow.",
        "each body must be read from `message`, never `body`");
}

// Membership is exactly envelope based: one recipient may be written as a bare
// string, and an incoming entry naming a different Agent identity is not this
// conversation even though its addresses match.
void verify_exact_direct_membership(const fs::path &sandbox) {
    const auto project = sandbox / "membership";
    const auto mailbox = mailbox_of(project);
    write_file(mailbox / "inbox" / "wrong-agent-id" / "message.json",
        envelope("telegram-bot", "human", "Other", "Other.",
            "received_at", "2026-08-07T10:03:00Z", "some-other-agent-id"));
    write_file(mailbox / "sent" / "scalar-recipient" / "message.json",
        R"({"from":"human","to":"telegram-bot","subject":"Kept",)"
        R"("message":"Kept single-string recipient.",)"
        R"("sent_at":"2026-08-07T10:07:00Z"})");

    const auto history = read_direct_conversation(route_for(project));
    require(ids_of(history) == std::vector<std::string>{"scalar-recipient"},
        "a bare-string `to` is one recipient, and a mismatched incoming "
        "identity belongs to another Agent's conversation");
    require(history.messages[0].text == "Kept single-string recipient.",
        "the scalar-recipient row renders its own message");
    require(history.skipped == 0,
        "well-formed mail for another conversation is absent, not an error");
}

// The kernel moves outbox/<id> to sent/<id>, so the same ID can be observed
// twice; sent is the one that renders. Both folders must also keep ordering on
// their own stamps -- `deliver_at` while pending, `sent_at` once moved -- so a
// dropped stamp key would show up here as a skipped entry or the wrong copy.
void verify_outbox_and_sent_collapse(const fs::path &sandbox) {
    const auto project = sandbox / "collapse";
    const auto mailbox = mailbox_of(project);
    write_file(mailbox / "outbox" / "20260807T120000-dup1" / "message.json",
        envelope("human", "telegram-bot", "Pending", "OUTBOX COPY",
            "deliver_at", "2026-08-07T12:00:00Z"));
    write_file(mailbox / "sent" / "20260807T120000-dup1" / "message.json",
        envelope("human", "telegram-bot", "Pending", "SENT COPY",
            "sent_at", "2026-08-07T12:00:05Z"));

    const auto history = read_direct_conversation(route_for(project));
    require(history.messages.size() == 1,
        "one outgoing ID in outbox and sent must render exactly once");
    require(history.messages[0].text == "SENT COPY",
        "the sent copy is preferred over the outbox copy");
    require(history.skipped == 0,
        "a collapsed duplicate is not an error, and a pending outbox entry "
        "orders on its own deliver_at rather than being dropped");
}

// One bad neighbor is counted generically and never hides a valid neighbor.
void verify_bad_neighbors_are_skipped(const fs::path &sandbox) {
    const auto project = sandbox / "neighbors";
    const auto mailbox = mailbox_of(project);
    write_file(mailbox / "inbox" / "valid-neighbor" / "message.json",
        envelope("telegram-bot", "human", "Valid", "Still visible.",
            "received_at", "2026-08-07T10:00:00Z"));
    write_file(mailbox / "inbox" / "malformed" / "message.json",
        R"({"from":"telegram-bot","to":["human"],"message":)");
    write_file(mailbox / "inbox" / "oversize" / "message.json",
        std::string(R"({"from":"telegram-bot","to":["human"],)"
            R"("received_at":"2026-08-07T10:02:00Z","message":")")
            + std::string((std::size_t{1} << 20) + 16, 'x') + R"("})");
    // An entry directory that is really a symlink is rejected, not followed.
    write_file(sandbox / "neighbors-outside" / "message.json",
        envelope("telegram-bot", "human", "Outside", "Must not be followed.",
            "received_at", "2026-08-07T10:03:00Z"));
    std::error_code link_error;
    fs::create_directory_symlink(sandbox / "neighbors-outside",
        mailbox / "inbox" / "symlinked-entry", link_error);
    require(!link_error, "symlinked entry fixture must be created");

    const auto before = tree_snapshot(project);
    const auto history = read_direct_conversation(route_for(project));
    require(tree_snapshot(project) == before,
        "rejecting bad neighbors must not write to the project");
    require(ids_of(history) == std::vector<std::string>{"valid-neighbor"},
        "one bad neighbor must never hide a valid neighbor");
    require(history.skipped == 3,
        "every unusable neighbor is counted once, generically");
    for (const auto &message : history.messages) {
        require(message.text.find("Must not be followed") == std::string::npos,
            "a symlinked entry must never be followed");
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <sandbox>\n";
        return 2;
    }
    const auto sandbox = fs::path(argv[1]);
    try {
        std::error_code error;
        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be reset");
        fs::create_directories(sandbox, error);
        require(!error, "sandbox must be created");

        verify_incoming_and_outgoing_pair(sandbox);
        verify_exact_direct_membership(sandbox);
        verify_outbox_and_sent_collapse(sandbox);
        verify_bad_neighbors_are_skipped(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "direct_conversation_history: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "direct_conversation_history: all checks passed\n";
    return 0;
}
