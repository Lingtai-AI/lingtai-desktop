#include "direct_mail_publisher.h"
#include "attachment_selection.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::DirectConversationRoute;
using lingtai::desktop::DirectMailFailureReason;
using lingtai::desktop::DirectMailSendResult;
using lingtai::desktop::preflight_attachments;
using lingtai::desktop::send_direct_mail;

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

// Exact byte/type image of the fixture, matching the reader test's own no-write
// proof: reading the same tree twice must produce identical results.
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

DirectConversationRoute route_for(const fs::path &project_root) {
    DirectConversationRoute route;
    route.human_directory_key = "human";
    route.target_directory_key = "telegram-bot";
    route.human_address = "human";
    route.target_address = "telegram-bot";
    route.project_root = project_root;
    route.target_agent_id = "20260712-191609-d0c8";
    route.human_identity.agent_id = "20260101-000000-h001";
    route.human_identity.true_name = "Ted";
    route.human_identity.address = "human";
    route.human_identity.state = "active";
    return route;
}

fs::path outbox_of(const fs::path &project_root) {
    return project_root / ".lingtai" / "human" / "mailbox" / "outbox";
}

void prepare_project(const fs::path &project_root) {
    std::error_code error;
    fs::create_directories(project_root / ".lingtai" / "human", error);
    require(!error, "project human directory must be created");
}

void require_attachment_failure(
        const lingtai::desktop::DirectMailSendOutcome &outcome,
        DirectMailFailureReason reason,
        std::size_t index,
        const fs::path &source,
        const fs::path &project) {
    require(outcome.result == DirectMailSendResult::failed_local,
        "attachment failure must not queue");
    require(outcome.failure_reason == reason,
        "attachment failure must preserve its typed reason");
    require(outcome.message_id.empty(),
        "failed sends must never expose a message id");
    require(!outcome.published_message,
        "failed sends must never expose published-row facts");
    require(outcome.attachment_failure.has_value()
            && outcome.attachment_failure->index == index
            && outcome.attachment_failure->source_path == source,
        "attachment failure must preserve index and canonical source path");
    const auto outbox = outbox_of(project);
    require(!fs::exists(outbox) || fs::is_empty(outbox),
        "failed attachment send must leave no owned outbox leaf");
}

mode_t permissions_of(const fs::path &path) {
    struct stat status {};
    require(::stat(path.c_str(), &status) == 0,
        "published attachment path must be stat-able");
    return status.st_mode & 0777;
}

std::vector<std::string> leaf_entries(const fs::path &leaf) {
    auto result = std::vector<std::string>();
    for (const auto &entry : fs::directory_iterator(leaf)) {
        result.push_back(entry.path().filename().string());
    }
    std::ranges::sort(result);
    return result;
}

// The whole publisher contract on one journey: a fresh exclusive leaf with the
// real interoperable JSON schema, a second send that never touches the first,
// and a blocked outbox path that fails closed without disturbing anything.
void verify_publish_and_failure(const fs::path &sandbox) {
    const auto project = sandbox / "publish";
    const auto outbox = outbox_of(project);
    const auto route = route_for(project);

    // Pre-existing, unrelated mail that a publish must never touch.
    write_file(project / ".lingtai" / "human" / "mailbox" / "inbox"
            / "20260807T184852-0d13" / "message.json",
        R"({"from":"telegram-bot","to":["human"],"message":"pre-existing",)"
        R"("received_at":"2026-08-07T18:48:52Z"})");
    const auto before = tree_snapshot(project);

    const auto result = send_direct_mail(route, "Ted, the slice is complete.");
    require(result.result == DirectMailSendResult::queued,
        "a writable outbox must publish successfully");

    require(fs::exists(outbox), "the outbox directory must now exist");
    auto leaves = std::vector<fs::path>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        leaves.push_back(entry.path());
    }
    require(leaves.size() == 1,
        "exactly one fresh leaf must be created by one publish call");
    const auto &leaf = leaves.front();
    const auto id = leaf.filename().string();
    require(result.message_id == id,
        "queued outcome must expose the created leaf id");
    require(result.published_message
            && result.published_message->id == id
            && result.published_message->timestamp.size() == 20
            && result.published_message->subject.empty()
            && result.published_message->text
                == "Ted, the slice is complete."
            && result.published_message->attachments.empty(),
        "queued text outcome must expose only exact atomic publication facts");
    require(id.size() == 20 && id[8] == 'T' && id[15] == '-',
        "the id must be the interoperable YYYYMMDDTHHMMSS-xxxx shape");
    require(leaf_entries(leaf) == std::vector<std::string>{"message.json"},
        "the leaf must contain exactly message.json, with no leftover temp file");

    const auto body = read_file(leaf / "message.json");
    require(body.find("\"id\":\"" + id + "\"") != std::string::npos
            && body.find("\"_mailbox_id\":\"" + id + "\"") != std::string::npos,
        "id and _mailbox_id must equal the directory basename");
    require(body.find("\"from\":\"human\"") != std::string::npos,
        "from must be the route's human address");
    require(body.find("\"to\":[\"telegram-bot\"]") != std::string::npos,
        "to must be exactly the one target address, as an array");
    require(body.find("\"cc\":[]") != std::string::npos,
        "cc must be the empty array the current schema always carries");
    require(body.find("\"message\":\"Ted, the slice is complete.\"") != std::string::npos,
        "message must be the exact sent text");
    require(body.find("\"type\":\"normal\"") != std::string::npos,
        "type must be the current schema's plain-mail value");
    require(body.find("\"received_at\":\"") != std::string::npos,
        "received_at must be stamped, matching the current TUI pseudo-send path");
    require(body.find("\"identity\":{") != std::string::npos
            && body.find("\"agent_id\":\"20260101-000000-h001\"") != std::string::npos
            && body.find("\"agent_name\":\"Ted\"") != std::string::npos,
        "identity must carry the accepted human sender facts under their real field names");
    require(body.find("\"attachments\"") == std::string::npos,
        "text-only envelope must remain attachment-field free");

    for (const auto &[key, value] : before) {
        require(tree_snapshot(project).count(key)
                && tree_snapshot(project).at(key) == value,
            "publishing must never modify or remove pre-existing content: " + key);
    }

    const auto second = send_direct_mail(route, "A second, distinct message.");
    require(second.result == DirectMailSendResult::queued,
        "a second publish on the same route must also succeed");
    auto leaves_after_second = std::vector<std::string>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        leaves_after_second.push_back(entry.path().filename().string());
    }
    require(leaves_after_second.size() == 2,
        "a second send must allocate a fresh id rather than reuse or overwrite the first");
    require(read_file(leaf / "message.json") == body,
        "an unrelated later send must never modify an earlier leaf");

    // A pre-existing regular file where the outbox directory must go makes
    // every attempt fail closed, cleanly, and without disturbing it.
    const auto blocked_project = sandbox / "blocked";
    const auto blocked_mailbox = blocked_project / ".lingtai" / "human" / "mailbox";
    std::error_code mkdir_error;
    fs::create_directories(blocked_mailbox, mkdir_error);
    require(!mkdir_error, "blocked fixture parent must be created");
    write_file(blocked_mailbox / "outbox", "not a directory");
    const auto blocked_route = route_for(blocked_project);
    const auto blocked_before = tree_snapshot(blocked_project);
    const auto blocked_result = send_direct_mail(
        blocked_route, "Should never be queued.");
    require(blocked_result.result == DirectMailSendResult::failed_local,
        "an outbox path blocked by a regular file must fail closed, generically");
    require(tree_snapshot(blocked_project) == blocked_before,
        "a failed publish must leave every pre-existing byte and path untouched");
}

// An intermediate mailbox-path component (`outbox`, before the final fresh
// leaf the publisher already creates exclusively) must not be followed even
// though the OS otherwise resolves it transparently: a symlink there must
// fail the publish closed rather than write a leaf into an outside tree, and
// the outside tree must stay exactly as it was.
void verify_intermediate_symlink_no_outside_write(const fs::path &sandbox) {
    const auto project = sandbox / "symlink-write-project";
    const auto outside = sandbox / "symlink-write-outside";
    write_file(outside / "marker.txt", "outside-marker");

    std::error_code parent_error;
    fs::create_directories(
        project / ".lingtai" / "human" / "mailbox", parent_error);
    require(!parent_error, "mailbox parent must be created");
    std::error_code link_error;
    fs::create_directory_symlink(outside,
        project / ".lingtai" / "human" / "mailbox" / "outbox", link_error);
    require(!link_error,
        "intermediate outbox symlink fixture must be created");

    const auto outside_before = tree_snapshot(outside);
    const auto result = send_direct_mail(route_for(project),
        "Must never leave the project through a symlinked outbox.");
    require(result.result == DirectMailSendResult::failed_local,
        "a symlinked outbox component must fail closed rather than "
        "publish outside the project");
    require(tree_snapshot(outside) == outside_before,
        "a blocked publish through a symlinked outbox must leave outside "
        "content untouched, with no outside leaf or file created");
}

void verify_attachment_publish(const fs::path &sandbox) {
    const auto project = sandbox / "attachments-publish";
    prepare_project(project);
    const auto sources = sandbox / "attachment-sources";
    const auto report = sources / "report.pdf";
    const auto image = sources / "image.png";
    write_file(report, std::string("report-bytes\0binary", 19));
    write_file(image, "image-bytes");
    auto selected = preflight_attachments({report, image});
    require(selected.accepted.size() == 2,
        "attachment fixtures must pass selection");
    selected.accepted.push_back(selected.accepted.front());

    const auto result = send_direct_mail(route_for(project), "", selected.accepted);
    require(result.result == DirectMailSendResult::queued
            && result.failure_reason == DirectMailFailureReason::none
            && !result.attachment_failure && !result.message_id.empty()
            && result.published_message
            && result.published_message->attachments.size() == 3,
        "attachment-only mail must queue with success-only id facts");

    const auto leaf = outbox_of(project) / result.message_id;
    const auto attachments = leaf / "attachments";
    require(leaf_entries(leaf)
            == std::vector<std::string>{"attachments", "message.json"},
        "message.json must be the only publish marker beside attachments");
    require(leaf_entries(attachments)
            == std::vector<std::string>{"image.png", "report-1.pdf", "report.pdf"},
        "duplicate basenames use -1 before a normal extension deterministically");
    require(read_file(attachments / "report.pdf") == read_file(report)
            && read_file(attachments / "report-1.pdf") == read_file(report)
            && read_file(attachments / "image.png") == read_file(image),
        "every attachment is copied byte-exactly from the validated source");
    const auto expected_names = std::array<std::string, 3>{
        "report.pdf", "image.png", "report-1.pdf"};
    for (auto index = std::size_t{0}; index != expected_names.size(); ++index) {
        const auto &published =
            result.published_message->attachments[index];
        struct stat copied {};
        require(::stat(published.local_path.c_str(), &copied) == 0
                && published.local_path
                    == attachments / expected_names[index]
                && published.display_filename == expected_names[index]
                && published.byte_size
                    == static_cast<std::uint64_t>(copied.st_size)
                && published.device_id
                    == static_cast<std::uint64_t>(copied.st_dev)
                && published.inode_id
                    == static_cast<std::uint64_t>(copied.st_ino),
            "published attachment facts must name and identify the exact copied outbox file");
    }
    require((permissions_of(attachments) & 0077) == 0
            && (permissions_of(attachments / "report.pdf") & 0177) == 0,
        "attachment directory and files are private");

    const auto body = read_file(leaf / "message.json");
    require(body.find("\"message\":\"\"") != std::string::npos
            && body.find("\"attachments\":[") != std::string::npos,
        "attachment-only payload has empty message and an attachment array");
    for (const auto &name : {"report.pdf", "image.png", "report-1.pdf"}) {
        const auto sent = project / ".lingtai" / "human" / "mailbox" / "sent"
            / result.message_id / "attachments" / name;
        require(body.find(sent.string()) != std::string::npos,
            "payload path must point to the future human sent leaf: "
                + sent.string());
        require(fs::exists(attachments / name) && !fs::exists(sent),
            "before pickup bytes exist in outbox while payload names future sent");
    }

    const auto mixed = send_direct_mail(
        route_for(project), "caption", {selected.accepted[1]});
    require(mixed.result == DirectMailSendResult::queued,
        "text plus one attachment must queue");
    const auto mixed_leaf = outbox_of(project) / mixed.message_id;
    require(read_file(mixed_leaf / "message.json").find(
                "\"message\":\"caption\"") != std::string::npos
            && read_file(mixed_leaf / "attachments" / "image.png")
                == read_file(image),
        "mixed mail preserves text and exact attachment bytes");
}

void verify_empty_and_source_failures(const fs::path &sandbox) {
    const auto empty_project = sandbox / "both-empty";
    prepare_project(empty_project);
    const auto before = tree_snapshot(empty_project);
    const auto empty = send_direct_mail(route_for(empty_project), "");
    require(empty.failure_reason == DirectMailFailureReason::empty_content
            && empty.message_id.empty(),
        "both-empty send fails with typed facts and no id");
    require(tree_snapshot(empty_project) == before,
        "both-empty send creates no mailbox state");

    const auto base = sandbox / "source-failures";
    const auto original = base / "source.txt";

    write_file(original, "same-size");
    auto accepted = preflight_attachments({original}).accepted.front();
    fs::remove(original);
    auto project = base / "missing-project";
    prepare_project(project);
    auto outcome = send_direct_mail(route_for(project), "body", {accepted});
    require_attachment_failure(outcome, DirectMailFailureReason::attachment_missing,
        0, accepted.source_path, project);

    write_file(original, "same-size");
    accepted = preflight_attachments({original}).accepted.front();
    const auto replacement = base / "replacement.txt";
    write_file(replacement, "replaced!");
    fs::rename(replacement, original);
    project = base / "replaced-project";
    prepare_project(project);
    outcome = send_direct_mail(route_for(project), "body", {accepted});
    require_attachment_failure(outcome, DirectMailFailureReason::attachment_replaced,
        0, accepted.source_path, project);

    fs::remove(original);
    write_file(original, "target");
    accepted = preflight_attachments({original}).accepted.front();
    const auto target = base / "symlink-target.txt";
    write_file(target, "target");
    fs::remove(original);
    fs::create_symlink(target, original);
    project = base / "symlink-project";
    prepare_project(project);
    outcome = send_direct_mail(route_for(project), "body", {accepted});
    require_attachment_failure(outcome, DirectMailFailureReason::attachment_symlink,
        0, accepted.source_path, project);

    fs::remove(original);
    write_file(original, "regular");
    accepted = preflight_attachments({original}).accepted.front();
    fs::remove(original);
    fs::create_directory(original);
    project = base / "nonregular-project";
    prepare_project(project);
    outcome = send_direct_mail(route_for(project), "body", {accepted});
    require_attachment_failure(outcome,
        DirectMailFailureReason::attachment_not_regular,
        0, accepted.source_path, project);

    fs::remove_all(original);
    write_file(original, "small");
    accepted = preflight_attachments({original}).accepted.front();
    write_file(original, "now-a-different-size");
    project = base / "size-project";
    prepare_project(project);
    outcome = send_direct_mail(route_for(project), "body", {accepted});
    require_attachment_failure(outcome,
        DirectMailFailureReason::attachment_size_changed,
        0, accepted.source_path, project);

    fs::remove(original);
    write_file(original, "locked");
    accepted = preflight_attachments({original}).accepted.front();
    std::error_code permission_error;
    fs::permissions(original, fs::perms::none, fs::perm_options::replace,
        permission_error);
    if (!permission_error && geteuid() != 0) {
        project = base / "unreadable-project";
        prepare_project(project);
        outcome = send_direct_mail(route_for(project), "body", {accepted});
        require_attachment_failure(outcome,
            DirectMailFailureReason::attachment_unreadable,
            0, accepted.source_path, project);
    } else {
        std::cout << "SKIP: unreadable publisher fixture is ineffective\n";
    }
    fs::permissions(original, fs::perms::owner_all, fs::perm_options::replace,
        permission_error);
}

void resize_file(const fs::path &path, std::uint64_t size) {
    write_file(path, "");
    std::error_code error;
    fs::resize_file(path, size, error);
    require(!error, "sparse publisher fixture must be sized");
}

void verify_limits_names_and_rollback(const fs::path &sandbox) {
    const auto base = sandbox / "limits";
    const auto oversized = base / "oversized.bin";
    write_file(oversized, "small");
    auto stale = preflight_attachments({oversized}).accepted.front();
    resize_file(oversized,
        lingtai::desktop::kAttachmentPerFileLimitBytes + 1);
    auto project = base / "per-file-project";
    prepare_project(project);
    auto outcome = send_direct_mail(route_for(project), "body", {stale});
    require_attachment_failure(outcome,
        DirectMailFailureReason::attachment_per_file_limit,
        0, stale.source_path, project);

    std::vector<lingtai::desktop::AcceptedAttachment> total;
    for (auto index = 0; index != 5; ++index) {
        const auto path = base / ("total-" + std::to_string(index) + ".bin");
        resize_file(path, lingtai::desktop::kAttachmentPerFileLimitBytes);
        const auto one = preflight_attachments({path});
        require(one.accepted.size() == 1, "each total fixture is individually valid");
        total.push_back(one.accepted.front());
    }
    project = base / "total-project";
    prepare_project(project);
    outcome = send_direct_mail(route_for(project), "body", total);
    require_attachment_failure(outcome,
        DirectMailFailureReason::attachment_total_limit,
        4, total[4].source_path, project);

    const auto short_source = base / "short.txt";
    write_file(short_source, "short");
    auto unsafe = preflight_attachments({short_source}).accepted.front();
    unsafe.display_filename = "../forged.txt";
    project = base / "unsafe-name-project";
    prepare_project(project);
    outcome = send_direct_mail(route_for(project), "body", {unsafe});
    require_attachment_failure(outcome,
        DirectMailFailureReason::unsafe_attachment_name,
        0, unsafe.source_path, project);

    auto too_long = preflight_attachments({short_source}).accepted.front();
    too_long.display_filename = std::string(300, 'a');
    project = base / "destination-failure-project";
    prepare_project(project);
    outcome = send_direct_mail(route_for(project), "body", {too_long});
    require_attachment_failure(outcome,
        DirectMailFailureReason::attachment_destination_failure,
        0, too_long.source_path, project);
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

        verify_publish_and_failure(sandbox);
        verify_intermediate_symlink_no_outside_write(sandbox);
        verify_attachment_publish(sandbox);
        verify_empty_and_source_failures(sandbox);
        verify_limits_names_and_rollback(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "direct_mail_publisher: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "direct_mail_publisher: all checks passed\n";
    return 0;
}
