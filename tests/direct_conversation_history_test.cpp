#include "direct_conversation_history.h"

#include "attachment_selection.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::AttachmentMediaKind;
using lingtai::desktop::DirectConversationHistory;
using lingtai::desktop::DirectConversationMessage;
using lingtai::desktop::DirectConversationRoute;
using lingtai::desktop::DirectMailboxDirectoryFingerprint;
using lingtai::desktop::DirectMailboxFingerprint;
using lingtai::desktop::DirectMailboxRequest;
using lingtai::desktop::DirectMailboxRoute;
using lingtai::desktop::DirectMailboxSnapshot;
using lingtai::desktop::DirectMailboxSnapshotIndex;
using lingtai::desktop::classify_attachment_media_kind;
using lingtai::desktop::direct_mailbox_fingerprint;
using lingtai::desktop::read_direct_conversation;
using lingtai::desktop::read_direct_mailbox_snapshot;

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
        + R"(,")" + std::string(timestamp_key) + R"(":")"
        + std::string(timestamp) + R"("})";
}

std::string with_attachments(std::string message, std::string_view value) {
    require(!message.empty() && message.back() == '}',
        "fixture envelope must be an object");
    message.pop_back();
    return message + R"(,"attachments":)" + std::string(value) + "}";
}

DirectConversationRoute route_for(const fs::path &project_root) {
    DirectConversationRoute route;
    route.human_directory_key = "human";
    route.target_directory_key = "telegram-bot";
    route.human_address = "human";
    route.target_address = "telegram-bot";
    route.project_root = project_root;
    route.target_agent_id = "20260712-191609-d0c8";
    return route;
}

DirectConversationRoute route_for_target(
        const fs::path &project_root,
        std::string target,
        std::string agent_id) {
    auto route = route_for(project_root);
    route.target_directory_key = target;
    route.target_address = std::move(target);
    route.target_agent_id = std::move(agent_id);
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
    require(history.messages[0].attachments.empty()
            && history.skipped_attachments == 0,
        "text-only collapse remains attachment-free with no attachment skips");
}

void verify_attachment_projection_and_current_entry_rooting(
        const fs::path &sandbox) {
    const auto project = sandbox / "attachment-projection";
    const auto mailbox = mailbox_of(project);
    const auto incoming = mailbox / "inbox" / "incoming-attachments";
    write_file(incoming / "attachments" / "photo.PNG", "image-bytes");
    write_file(incoming / "attachments" / "report.pdf", "report");
    write_file(incoming / "message.json", with_attachments(
        envelope("telegram-bot", "human", "Incoming files", "See both.",
            "received_at", "2026-08-07T09:00:00Z"),
        R"(["/future/sent/photo.PNG","nested/report.pdf","report.pdf"])"));

    const auto sent = mailbox / "sent" / "sent-attachment";
    write_file(sent / "attachments" / "notes.txt", "sent-notes");
    write_file(sent / "message.json", with_attachments(
        envelope("human", "telegram-bot", "Sent file", "Sent text.",
            "sent_at", "2026-08-07T09:01:00Z"),
        R"(["/already/sent/notes.txt"])"));

    const auto pending = mailbox / "outbox" / "pending-attachment";
    write_file(pending / "attachments" / "pending.webp", "webp");
    write_file(pending / "message.json", with_attachments(
        envelope("human", "telegram-bot", "Pending file", "Pending text.",
            "deliver_at", "2026-08-07T09:02:00Z"),
        R"(["/project/.lingtai/human/mailbox/sent/pending-attachment/attachments/pending.webp"])"));

    const auto large = incoming / "attachments" / "large.bin";
    write_file(large, "x");
    std::error_code resize_error;
    fs::resize_file(large,
        lingtai::desktop::kAttachmentPerFileLimitBytes + 1, resize_error);
    require(!resize_error, "large history attachment fixture must be resized");
    auto incoming_json = read_file(incoming / "message.json");
    incoming_json.pop_back();
    incoming_json.insert(incoming_json.size() - 1,
        R"(,"/somewhere/large.bin")");
    incoming_json.push_back('}');
    write_file(incoming / "message.json", incoming_json);

    const auto history = read_direct_conversation(route_for(project));
    require(ids_of(history) == std::vector<std::string>{
                "incoming-attachments", "sent-attachment", "pending-attachment"},
        "incoming, sent, and pending attachment rows retain history ordering");
    require(history.skipped == 0 && history.skipped_attachments == 0,
        "all valid attachment metadata projects without skips");

    const auto &incoming_message = history.messages[0];
    require(incoming_message.attachments.size() == 4,
        "every valid incoming attachment, including a duplicate, is retained");
    require(incoming_message.attachments[0].display_filename == "photo.PNG"
            && incoming_message.attachments[1].display_filename == "report.pdf"
            && incoming_message.attachments[2].display_filename == "report.pdf"
            && incoming_message.attachments[3].display_filename == "large.bin",
        "attachment JSON order and duplicate names are preserved");
    require(incoming_message.attachments[0].local_path
            == (incoming / "attachments" / "photo.PNG").lexically_normal()
            && incoming_message.attachments[1].local_path
            == (incoming / "attachments" / "report.pdf").lexically_normal(),
        "serialized parents are discarded and paths root under this inbox entry");
    require(incoming_message.attachments[0].byte_size == 11
            && incoming_message.attachments[1].byte_size == 6
            && incoming_message.attachments[3].byte_size
                == lingtai::desktop::kAttachmentPerFileLimitBytes + 1,
        "history reports opened-file sizes exactly and applies no send limits");
    require(incoming_message.attachments[0].media_kind
                == AttachmentMediaKind::image
            && incoming_message.attachments[0].media_kind
                == classify_attachment_media_kind("photo.PNG")
            && incoming_message.attachments[1].media_kind
                == classify_attachment_media_kind("report.pdf"),
        "history shares Commit 1's deterministic media classifier");

    require(history.messages[1].attachments.size() == 1
            && history.messages[1].attachments[0].local_path
                == (sent / "attachments" / "notes.txt").lexically_normal(),
        "sent metadata roots under the current sent entry");
    require(history.messages[2].attachments.size() == 1
            && history.messages[2].attachments[0].local_path
                == (pending / "attachments" / "pending.webp").lexically_normal(),
        "pre-pickup JSON naming future sent bytes roots to current outbox");
}

void verify_bad_attachments_preserve_messages_and_stay_contained(
        const fs::path &sandbox) {
    const auto project = sandbox / "bad-attachments";
    const auto outside = sandbox / "bad-attachments-outside";
    const auto mailbox = mailbox_of(project);
    const auto mixed = mailbox / "inbox" / "mixed";
    write_file(mixed / "attachments" / "good.txt", "good");
    write_file(outside / "outside.txt", "outside-secret");
    std::error_code error;
    fs::create_symlink(outside / "outside.txt",
        mixed / "attachments" / "symlink.txt", error);
    require(!error, "symlinked attachment fixture must be created");
    fs::create_directory(mixed / "attachments" / "folder", error);
    require(!error, "directory attachment fixture must be created");
    require(::mkfifo((mixed / "attachments" / "pipe").c_str(), 0600) == 0,
        "FIFO attachment fixture must be created");
    write_file(mixed / "message.json", with_attachments(
        envelope("telegram-bot", "human", "Mixed", "Text survives.",
            "received_at", "2026-08-07T10:00:00Z"),
        R"(["good.txt","missing.txt","/","..",17,"symlink.txt","folder","pipe","../../outside/outside.txt"])"));

    const auto symlinked_directory = mailbox / "inbox" / "symlinked-directory";
    write_file(symlinked_directory / "message.json", with_attachments(
        envelope("telegram-bot", "human", "Linked directory",
            "This text also survives.", "received_at",
            "2026-08-07T10:01:00Z"), R"(["outside.txt"])"));
    fs::create_directory_symlink(outside,
        symlinked_directory / "attachments", error);
    require(!error, "symlinked attachments directory fixture must be created");

    write_file(mailbox / "inbox" / "malformed-field" / "message.json",
        with_attachments(envelope("telegram-bot", "human", "Malformed field",
            "Malformed metadata does not hide me.", "received_at",
            "2026-08-07T10:02:00Z"), R"({"not":"an array"})"));

    const auto project_before = tree_snapshot(project);
    const auto outside_before = tree_snapshot(outside);
    const auto history = read_direct_conversation(route_for(project));
    require(tree_snapshot(project) == project_before
            && tree_snapshot(outside) == outside_before,
        "attachment projection must write neither mailbox nor outside bytes");
    require(ids_of(history) == std::vector<std::string>{
                "mixed", "symlinked-directory", "malformed-field"},
        "bad attachment metadata never hides otherwise valid message text");
    require(history.skipped == 0,
        "bad attachments never increment skipped-message accounting");
    require(history.skipped_attachments == 10,
        "each bad array element and each malformed field is observable");
    require(history.messages[0].text == "Text survives."
            && history.messages[0].attachments.size() == 1
            && history.messages[0].attachments[0].display_filename == "good.txt",
        "one good sibling survives missing, unsafe, non-string, linked, and "
        "non-regular attachment entries");
    require(history.messages[1].attachments.empty()
            && history.messages[2].attachments.empty(),
        "symlinked directories and malformed fields fail closed per message");
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

// An intermediate mailbox-path component (the human directory, before the
// final `mailbox`/`inbox`/entry components the reader already checks) must
// not be followed even though the OS otherwise resolves it transparently: a
// symlink there must not let an outside tree's mail be read as this
// conversation's history, and reading must never change the outside tree.
void verify_intermediate_symlink_no_outside_read(const fs::path &sandbox) {
    const auto project = sandbox / "symlink-read-project";
    const auto outside = sandbox / "symlink-read-outside";
    write_file(outside / "mailbox" / "inbox" / "20260807T200000-out1"
            / "message.json",
        envelope("telegram-bot", "human", "Outside",
            "Must never be read through an intermediate symlink.",
            "received_at", "2026-08-07T20:00:00Z"));

    std::error_code mkdir_error;
    fs::create_directories(project / ".lingtai", mkdir_error);
    require(!mkdir_error, "project .lingtai parent must be created");
    std::error_code link_error;
    fs::create_directory_symlink(
        outside, project / ".lingtai" / "human", link_error);
    require(!link_error, "intermediate human-directory symlink fixture "
        "must be created");

    const auto outside_before = tree_snapshot(outside);
    const auto history = read_direct_conversation(route_for(project));
    require(tree_snapshot(outside) == outside_before,
        "reading through a blocked intermediate symlink must never change "
        "outside state");
    require(ids_of(history).empty(),
        "a message reachable only through an intermediate mailbox-path "
        "symlink must never be accepted into the conversation");
}

void verify_shared_multi_route_projection(const fs::path &sandbox) {
    const auto project = sandbox / "shared";
    const auto mailbox = mailbox_of(project);
    const auto telegram_entry =
        mailbox / "inbox" / "20260807T180000-t001";
    write_file(telegram_entry / "attachments" / "report.txt", "bounded\n");
    write_file(telegram_entry / "message.json", with_attachments(
        envelope("telegram-bot", "human", "Telegram", "many matches",
            "received_at", "2026-08-07T18:00:00Z",
            "20260712-191609-d0c8"),
        R"(["report.txt","missing.txt",7])"));
    write_file(mailbox / "inbox" / "20260807T180100-c001" / "message.json",
        envelope("codex", "human", "Codex", "one match", "received_at",
            "2026-08-07T18:01:00Z"));
    write_file(mailbox / "inbox" / "20260807T180200-cc01" / "message.json",
        R"({"from":"telegram-bot","to":["human"],"cc":["codex"],)"
        R"("message":"copied","received_at":"2026-08-07T18:02:00Z"})");
    const auto outgoing = envelope("human", "telegram-bot", "Reply",
        "sent once", "sent_at", "2026-08-07T18:03:00Z");
    write_file(mailbox / "sent" / "20260807T180300-o001" / "message.json",
        outgoing);
    write_file(mailbox / "outbox" / "20260807T180300-o001" / "message.json",
        outgoing);
    write_file(mailbox / "inbox" / "20260807T180400-bad1" / "message.json",
        R"({"from":)");

    auto request = DirectMailboxRequest{{
        {"telegram-bot", route_for_target(project, "telegram-bot",
            "20260712-191609-d0c8")},
        {"codex", route_for_target(project, "codex", "codex-id")},
        {"quiet", route_for_target(project, "quiet", "quiet-id")},
    }};
    const auto before = tree_snapshot(project);
    const auto snapshot = read_direct_mailbox_snapshot(request);
    require(tree_snapshot(project) == before,
        "a shared mailbox projection must remain read-only");
    require(snapshot.histories.size() == 3,
        "every current route must receive a complete history");
    require(ids_of(snapshot.histories.at("telegram-bot"))
            == std::vector<std::string>{
                "20260807T180000-t001", "20260807T180300-o001"},
        "the busy route must retain incoming plus deduplicated outgoing mail");
    require(ids_of(snapshot.histories.at("codex"))
            == std::vector<std::string>{"20260807T180100-c001"},
        "a sibling route must retain only its exact direct mail");
    require(snapshot.histories.at("quiet").messages.empty(),
        "a sibling with no matches must still receive an empty history");
    for (const auto &[key, history] : snapshot.histories) {
        static_cast<void>(key);
        require(history.skipped == 1,
            "one malformed shared entry must count once for every route");
    }
    const auto &telegram = snapshot.histories.at("telegram-bot");
    require(telegram.messages.front().attachments.size() == 1
            && telegram.skipped_attachments == 2,
        "attachment projection and independent skips must survive sharing");
    require(snapshot.histories.at("codex").skipped_attachments == 0
            && snapshot.histories.at("quiet").skipped_attachments == 0,
        "attachment skips must stay on the owning conversation only");

    const auto fingerprint_before = direct_mailbox_fingerprint(request);
    write_file(mailbox / "inbox" / "20260807T180500-new1" / "message.json",
        envelope("quiet", "human", "Fresh", "arrived", "received_at",
            "2026-08-07T18:05:00Z"));
    const auto fingerprint_after = direct_mailbox_fingerprint(request);
    require(fingerprint_before != fingerprint_after,
        "a fixed-count folder fingerprint must detect an appended entry");
}

DirectMailboxFingerprint fingerprint(std::uint64_t inode) {
    auto result = DirectMailboxFingerprint();
    result.mailbox = DirectMailboxDirectoryFingerprint{
        .state = 1, .device_id = 7, .inode_id = 11};
    result.folders[0] = DirectMailboxDirectoryFingerprint{
        .state = 1, .device_id = 7, .inode_id = inode};
    return result;
}

void verify_generation_single_flight_and_retry(const fs::path &sandbox) {
    const auto project_a = sandbox / "generation-a";
    const auto project_b = sandbox / "generation-b";
    auto request_a = DirectMailboxRequest{{{"a", route_for(project_a)}}};
    auto request_b = DirectMailboxRequest{{{"b", route_for(project_b)}}};
    auto index = DirectMailboxSnapshotIndex();

    const auto first = index.request(request_a, fingerprint(1));
    require(first && index.inflight(),
        "the first desired snapshot must launch one generation");
    require(!index.request(request_a, fingerprint(1)),
        "an unchanged request while inflight must remain single-flight");
    require(!index.request(request_b, fingerprint(2)),
        "a route/project change while inflight must queue, not overlap");

    auto stale_snapshot = DirectMailboxSnapshot();
    stale_snapshot.histories["a"].messages.push_back(
        {"stale", false, "1", {}, "old project", {}, "inbox"});
    const auto stale = index.complete(
        *first, std::move(stale_snapshot), fingerprint(1));
    require(!stale.accepted && stale.follow_up
            && stale.follow_up->request == request_b,
        "a stale project generation must not publish and must launch current");

    auto current_snapshot = DirectMailboxSnapshot();
    current_snapshot.histories["b"].messages.push_back(
        {"current", false, "2", {}, "new project", {}, "inbox"});
    const auto current = index.complete(*stale.follow_up,
        std::move(current_snapshot), fingerprint(2));
    require(current.accepted && !current.follow_up && index.current()
            && index.current()->histories.contains("b")
            && !index.current()->histories.contains("a"),
        "only the current project generation may become visible");
    require(!index.request(request_b, fingerprint(2)),
        "an unchanged completed tick must launch no new full scan");

    const auto mutation_job = index.request(request_b, fingerprint(3));
    require(mutation_job.has_value(),
        "a changed folder fingerprint must launch one fresh generation");
    const auto mutation = index.complete(
        *mutation_job, DirectMailboxSnapshot(), fingerprint(4));
    require(!mutation.accepted && mutation.follow_up
            && mutation.follow_up->fingerprint == fingerprint(4),
        "an in-scan append must reject the torn snapshot and retry freshness");
    const auto fresh = index.complete(*mutation.follow_up,
        DirectMailboxSnapshot(), fingerprint(4));
    require(fresh.accepted && !fresh.follow_up,
        "the stable follow-up generation must replace the stale attempt");

    const auto teardown_job = index.request(request_b, fingerprint(5));
    require(teardown_job.has_value(),
        "a final changed fingerprint must launch");
    index.reset();
    const auto after_reset = index.complete(
        *teardown_job, DirectMailboxSnapshot(), fingerprint(5));
    require(!after_reset.accepted && !after_reset.follow_up
            && !index.current(),
        "reset/destruction state must discard a late worker result safely");
}

DirectConversationMessage revision_message(
        std::string id, std::string timestamp, std::string text) {
    return {std::move(id), false, std::move(timestamp), {}, std::move(text),
        {}, "inbox"};
}

void verify_per_history_revisions_and_append_lineage(const fs::path &sandbox) {
    auto request = DirectMailboxRequest{{
        {"selected", route_for(sandbox / "revision")},
        {"other", route_for(sandbox / "revision")},
    }};
    auto index = DirectMailboxSnapshotIndex();
    const auto first_job = index.request(request, fingerprint(20));
    require(first_job.has_value(), "initial revision generation must launch");
    auto first_snapshot = DirectMailboxSnapshot();
    first_snapshot.histories["selected"].messages.push_back(
        revision_message("s1", "1", "selected"));
    first_snapshot.histories["other"].messages.push_back(
        revision_message("o1", "1", "other"));
    DirectMailboxSnapshotIndex::classify(*first_job, first_snapshot);
    require(index.complete(*first_job, std::move(first_snapshot), fingerprint(20))
            .accepted,
        "initial revision snapshot must be accepted");
    const auto selected_revision =
        index.current()->revisions.at("selected").revision;
    require(selected_revision != 0,
        "an accepted selected history must receive a stable revision");
    require(!index.request(request, fingerprint(20))
            && index.current()->revisions.at("selected").revision
                == selected_revision,
        "an unchanged request/current snapshot must retain its revision");

    const auto other_job = index.request(request, fingerprint(21));
    require(other_job.has_value(), "another-Agent append must launch");
    auto other_snapshot = DirectMailboxSnapshot();
    other_snapshot.histories["selected"].messages.push_back(
        revision_message("s1", "1", "selected"));
    other_snapshot.histories["other"].messages = {
        revision_message("o1", "1", "other"),
        revision_message("o2", "2", "other append")};
    DirectMailboxSnapshotIndex::classify(*other_job, other_snapshot);
    require(index.complete(*other_job, std::move(other_snapshot), fingerprint(21))
            .accepted
            && index.current()->revisions.at("selected").revision
                == selected_revision,
        "mail affecting only another Agent must not invalidate selected history");

    const auto append_job = index.request(request, fingerprint(22));
    auto appended = *index.current();
    appended.histories["selected"].messages.push_back(
        revision_message("s2", "2", "selected append"));
    DirectMailboxSnapshotIndex::classify(*append_job, appended);
    require(index.complete(*append_job, std::move(appended), fingerprint(22))
            .accepted,
        "selected append must be accepted");
    const auto append_revision = index.current()->revisions.at("selected");
    require(append_revision.revision != selected_revision
            && append_revision.append_from_revision == selected_revision
            && append_revision.append_from == 1,
        "a pure selected append must carry exact parent revision and boundary");

    const auto baseline = *index.current();
    const auto reject_append = [&](DirectConversationHistory changed,
            std::string_view reason) {
        auto job = DirectMailboxSnapshotIndex::Job{
            1, request, fingerprint(23),
            std::make_shared<const DirectMailboxSnapshot>(baseline)};
        auto candidate = baseline;
        candidate.histories["selected"] = std::move(changed);
        DirectMailboxSnapshotIndex::classify(job, candidate);
        const auto metadata = candidate.revisions.find("selected");
        require(metadata == candidate.revisions.end()
                || metadata->second.append_from_revision == 0,
            std::string(reason));
    };
    auto replacement = baseline.histories.at("selected");
    replacement.messages[1].text = "same-size replacement";
    reject_append(replacement, "same-size replacement must not claim append");
    auto prefix_edit = baseline.histories.at("selected");
    prefix_edit.messages[0].text = "prefix edit";
    prefix_edit.messages.push_back(revision_message("s3", "3", "suffix"));
    reject_append(prefix_edit, "prefix edit plus growth must not claim append");
    auto attachment_edit = baseline.histories.at("selected");
    attachment_edit.messages[0].attachments.push_back({
        sandbox / "changed", "changed", 1, AttachmentMediaKind::file, 1, 2});
    attachment_edit.messages.push_back(
        revision_message("s3", "3", "suffix"));
    reject_append(attachment_edit,
        "attachment identity/metadata change must not claim append");
    auto reordered = baseline.histories.at("selected");
    std::swap(reordered.messages[0], reordered.messages[1]);
    reordered.messages.push_back(revision_message("s3", "3", "suffix"));
    reject_append(reordered, "reorder plus growth must not claim append");
    auto shrunk = baseline.histories.at("selected");
    shrunk.messages.pop_back();
    reject_append(shrunk, "shrink must not claim append");
    auto diagnostic_change = baseline.histories.at("selected");
    ++diagnostic_change.skipped_attachments;
    diagnostic_change.messages.push_back(
        revision_message("s3", "3", "suffix"));
    reject_append(diagnostic_change,
        "skipped diagnostics plus growth must not claim append");
}

} // namespace

int main(int argc, char **argv) {
    static_assert(noexcept(read_direct_conversation(
        std::declval<const DirectConversationRoute &>())));
    static_assert(std::is_same_v<decltype(read_direct_conversation(
        std::declval<const DirectConversationRoute &>())),
        DirectConversationHistory>);
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
        verify_attachment_projection_and_current_entry_rooting(sandbox);
        verify_bad_attachments_preserve_messages_and_stay_contained(sandbox);
        verify_bad_neighbors_are_skipped(sandbox);
        verify_intermediate_symlink_no_outside_read(sandbox);
        verify_shared_multi_route_projection(sandbox);
        verify_generation_single_flight_and_retry(sandbox);
        verify_per_history_revisions_and_append_lineage(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "direct_conversation_history: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "direct_conversation_history: all checks passed\n";
    return 0;
}
