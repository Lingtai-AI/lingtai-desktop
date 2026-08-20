#include "direct_mail_publisher.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::DirectConversationRoute;
using lingtai::desktop::DirectMailSendResult;
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

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "direct_mail_publisher: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "direct_mail_publisher: all checks passed\n";
    return 0;
}
