#ifdef QT_CORE_LIB
#error "Qt Core usage requirements must not propagate to this route consumer"
#endif
#if __has_include("direct_conversation_route.h")
// The pure direct-route seam is present and remains Qt-free to consumers.
#include "direct_conversation_route.h"
#include "project_attachment.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace fs = std::filesystem;
using namespace lingtai::desktop;
namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n', ++failures;
}
std::map<std::string, std::string> snapshot_tree(const fs::path &root) {
    std::map<std::string, std::string> result; std::error_code error;
    for (fs::recursive_directory_iterator i(root, error), end;
            !error && i != end; i.increment(error)) {
        const auto key = i->path().lexically_relative(root).generic_string();
        const auto status = i->symlink_status(error); if (error) break;
        if (fs::is_symlink(status)) result[key] =
            "link:" + fs::read_symlink(i->path()).generic_string();
        else if (fs::is_directory(status)) result[key] = "directory";
        else if (fs::is_regular_file(status)) {
            std::ifstream stream(i->path(), std::ios::binary);
            result[key] = "file:" + std::string{
                std::istreambuf_iterator<char>(stream), {}};
        } else result[key] = "other";
    }
    expect(!error, "fixture snapshot is complete"); return result;
}
// Deliberately distinct fixture vocabulary: the human key is not "human", and
// the target key, manifest agent_id, and address are three different strings.
constexpr std::string_view operator_key = "operator-1";
constexpr std::string_view operator_address = "mail/operator-desk";
constexpr std::string_view worker_key = "worker-dir";
constexpr std::string_view worker_agent_id = "worker-agent-id";
constexpr std::string_view worker_address = "mail/worker-post";
AgentIdentityFacts facts(std::optional<std::string> agent_id,
        std::optional<std::string> address,
        std::optional<std::string> true_name = std::nullopt,
        std::optional<std::string> nickname = std::nullopt,
        std::optional<std::string> state = std::nullopt) {
    AgentIdentityFacts result;
    result.agent_id = std::move(agent_id); result.address = std::move(address);
    result.true_name = std::move(true_name); result.nickname = std::move(nickname);
    result.state = std::move(state); return result;
}
AgentRow row(std::string_view key, AgentRole role,
        std::optional<AgentIdentityFacts> identity,
        AgentManifestKind kind = AgentManifestKind::valid) {
    AgentRow item;
    item.directory_key = fs::path(key); item.role = role;
    item.manifest_kind = kind; item.identity = std::move(identity);
    return item;
}
std::vector<AgentRow> standard_rows() {
    return {row(operator_key, AgentRole::human,
                facts(std::string("operator-manifest-id"), std::string(operator_address),
                    std::string("Operator True Name"), std::string("Op"),
                    std::string("idle"))),
            row(worker_key, AgentRole::agent,
                facts(std::string(worker_agent_id), std::string(worker_address)))};
}
AgentSnapshot snapshot(std::vector<AgentRow> items) {
    AgentSnapshot result;
    result.scan = AgentScanState::complete;
    result.items = std::move(items);
    return result;
}
ProjectAttachmentResult make_project(const fs::path &base, std::string_view name) {
    const auto project = base / name; std::error_code error;
    fs::create_directories(project, error);
    expect(!error, "fixture project directory is created");
    auto attached = attach_project(project);
    expect(static_cast<bool>(attached), "fixture project attaches");
    return attached;
}
void expect_no_route(const ProjectAttachment &attachment,
        const AgentSnapshot &identity,
        const std::optional<fs::path> &selected, std::string_view message) {
    const auto route =
        resolve_direct_conversation_route(attachment, identity, selected);
    expect(!route, message);
}
void test_resolved_route(const fs::path &base) {
    auto attached = make_project(base, "resolved"); if (!attached) return;
    const auto &attachment = *attached.attachment;
    const auto identity = snapshot(standard_rows());
    static_assert(noexcept(resolve_direct_conversation_route(
        std::declval<const ProjectAttachment &>(),
        std::declval<const AgentSnapshot &>(),
        std::declval<const std::optional<fs::path> &>())));
    const auto before = snapshot_tree(base);
    const auto route = resolve_direct_conversation_route(
        attachment, identity, fs::path(worker_key));
    expect(route.has_value(),
        "the selected Agent and one valid human resolve exactly one direct route");
    if (!route) { expect(snapshot_tree(base) == before,
        "route resolution changes no project path"); return; }
    expect(route->human_directory_key == fs::path(operator_key)
            && route->human_directory_key != fs::path("human"),
        "the human route key is discovered from the human-role row, never assumed");
    expect(route->target_directory_key == fs::path(worker_key),
        "the target route key is the exact selected directory key");
    expect(route->human_address == std::string(operator_address)
            && route->target_address == std::string(worker_address),
        "current human and target addresses are returned separately");
    expect(route->project_root == attachment.root()
            && route->target_agent_id == std::string(worker_agent_id),
        "the route anchors on the canonical project root and target manifest agent_id");
    expect(route->target_agent_id != std::string(worker_address)
            && route->target_agent_id != fs::path(worker_key).string(),
        "the anchoring target agent_id uses neither the target address nor directory");
    expect(route->human_identity.agent_id == "operator-manifest-id"
            && route->human_identity.true_name == "Operator True Name"
            && route->human_identity.nickname == "Op"
            && route->human_identity.address == std::string(operator_address)
            && route->human_identity.state == "idle",
        "the human sender card carries exactly the accepted typed manifest fields");
    expect(snapshot_tree(base) == before,
        "route resolution changes no project path");
}
void test_route_eligibility(const fs::path &base) {
    auto attached = make_project(base, "eligibility"); if (!attached) return;
    const auto &attachment = *attached.attachment;
    const auto complete = snapshot(standard_rows());
    expect_no_route(attachment, complete, std::nullopt,
        "an absent selection fails closed");
    expect_no_route(attachment, complete, fs::path(),
        "an empty selection fails closed");
    expect_no_route(attachment, complete, fs::path("worker-di"),
        "an inexact selected key never falls back to another valid Agent");
    for (const auto kind : {AgentManifestKind::malformed, AgentManifestKind::unsafe}) {
        auto rows = standard_rows();
        rows[1] = row(worker_key, AgentRole::unknown, std::nullopt, kind);
        expect_no_route(attachment, snapshot(rows), fs::path(worker_key),
            "a malformed or unsafe selected row is never routable");
    }
    auto without_identity = standard_rows();
    without_identity[1].identity.reset();
    expect_no_route(attachment, snapshot(without_identity), fs::path(worker_key),
        "a selected row without typed identity facts is never routable");
    expect_no_route(attachment, complete, fs::path(operator_key),
        "the human row is never its own conversation target");
    for (const auto &absent : {std::optional<std::string>{},
            std::optional<std::string>{""}}) {
        auto rows = standard_rows(); rows[1].identity->agent_id = absent;
        expect_no_route(attachment, snapshot(rows), fs::path(worker_key),
            "a target without a manifest agent_id has no stable thread identity");
        rows = standard_rows(); rows[1].identity->address = absent;
        expect_no_route(attachment, snapshot(rows), fs::path(worker_key),
            "a target without a manifest address is not routable");
        rows = standard_rows(); rows[0].identity->address = absent;
        expect_no_route(attachment, snapshot(rows), fs::path(worker_key),
            "a human without a manifest address is not routable");
    }
    auto no_human = standard_rows(); no_human[0].role = AgentRole::main;
    expect_no_route(attachment, snapshot(no_human), fs::path(worker_key),
        "no valid human row fails closed");
    auto malformed_human = standard_rows();
    malformed_human[0] = row(operator_key, AgentRole::human, std::nullopt,
        AgentManifestKind::malformed);
    expect_no_route(attachment, snapshot(malformed_human), fs::path(worker_key),
        "a malformed human row is not a usable sender identity");
    auto ambiguous = standard_rows();
    ambiguous.push_back(row("operator-2", AgentRole::human,
        facts(std::string("second-human"), std::string("mail/second-desk"))));
    expect_no_route(attachment, snapshot(ambiguous), fs::path(worker_key),
        "two valid humans fail closed rather than silently picking one");
    auto conflict = standard_rows();
    conflict[0].identity->address = std::string(worker_address);
    expect_no_route(attachment, snapshot(conflict), fs::path(worker_key),
        "a human and target sharing one address is never a direct route");
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 2) return (std::cerr << "expected one test-sandbox path\n", 2);
    const auto base = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(base, error); fs::create_directories(base, error);
    expect(!error, "test sandbox is created");
    test_resolved_route(base);
    test_route_eligibility(base);
    fs::remove_all(base, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " direct route assertion(s) failed\n";
        return 1;
    }
    std::cout << "DIRECT_CONVERSATION_ROUTE_CONTRACT_OK\n";
    return 0;
}
#else
#include <iostream>
int main() {
    std::cerr << "FAIL: the direct conversation route seam is unavailable\n";
    return 1;
}
#endif
