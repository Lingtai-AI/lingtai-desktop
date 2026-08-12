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
AgentManifestIdentityFacts facts(std::optional<std::string> agent_id,
        std::optional<std::string> address,
        std::optional<std::string> true_name = std::nullopt,
        std::optional<std::string> nickname = std::nullopt,
        std::optional<std::string> state = std::nullopt) {
    AgentManifestIdentityFacts result;
    result.agent_id = std::move(agent_id); result.address = std::move(address);
    result.true_name = std::move(true_name); result.nickname = std::move(nickname);
    result.state = std::move(state); return result;
}
AgentManifestDiscoveryItem row(std::string_view key, AgentRole role,
        std::optional<AgentManifestIdentityFacts> identity,
        AgentManifestKind kind = AgentManifestKind::valid) {
    AgentManifestDiscoveryItem item;
    item.directory_key = fs::path(key); item.role = role;
    item.manifest_kind = kind; item.identity = std::move(identity);
    return item;
}
std::vector<AgentManifestDiscoveryItem> standard_rows() {
    return {row(operator_key, AgentRole::human,
                facts(std::string("operator-manifest-id"), std::string(operator_address),
                    std::string("Operator True Name"), std::string("Op"),
                    std::string("idle"))),
            row(worker_key, AgentRole::agent,
                facts(std::string(worker_agent_id), std::string(worker_address)))};
}
AgentIdentityStatusSnapshot snapshot(std::vector<AgentManifestDiscoveryItem> items,
        bool complete = true) {
    AgentIdentityStatusSnapshot result;
    result.roster.discovery.scan.state = AgentManifestScanState::complete;
    result.roster.discovery.items = std::move(items);
    result.roster.presence_by_item.resize(result.roster.discovery.items.size());
    result.detail_by_item.resize(result.roster.discovery.items.size());
    result.roster.projection_complete = complete;
    result.projection_complete = complete;
    result.observed_at_seconds = 200.0;
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
void expect_failure(const ProjectAttachment &attachment,
        const AgentIdentityStatusSnapshot &identity,
        const std::optional<fs::path> &selected, DirectRouteFailure expected,
        std::string_view message) {
    const auto report =
        resolve_direct_conversation_route(attachment, identity, selected);
    expect(!report.route && !report && report.failure == expected, message);
}
void test_resolved_route(const fs::path &base) {
    auto attached = make_project(base, "resolved"); if (!attached) return;
    const auto &attachment = *attached.attachment;
    const auto identity = snapshot(standard_rows());
    static_assert(noexcept(resolve_direct_conversation_route(
        std::declval<const ProjectAttachment &>(),
        std::declval<const AgentIdentityStatusSnapshot &>(),
        std::declval<const std::optional<fs::path> &>())));
    const auto before = snapshot_tree(base);
    const auto report = resolve_direct_conversation_route(
        attachment, identity, fs::path(worker_key));
    expect(report.failure == DirectRouteFailure::none && report.route.has_value()
            && static_cast<bool>(report),
        "the selected Agent and one valid human resolve exactly one direct route");
    if (!report.route) { expect(snapshot_tree(base) == before,
        "route resolution changes no project path"); return; }
    const auto &route = *report.route;
    expect(route.human_directory_key == fs::path(operator_key)
            && route.human_directory_key != fs::path("human"),
        "the human route key is discovered from the human-role row, never assumed");
    expect(route.target_directory_key == fs::path(worker_key),
        "the target route key is the exact selected directory key");
    expect(route.human_address == std::string(operator_address)
            && route.target_address == std::string(worker_address),
        "current human and target addresses are returned separately");
    expect(route.thread_key
            == DirectThreadKey{attachment.root(), std::string(worker_agent_id)},
        "stable identity is canonical project root plus target manifest agent_id");
    expect(route.thread_key.target_agent_id != std::string(worker_address)
            && route.thread_key.target_agent_id != fs::path(worker_key).string()
            && route.thread_key.project_root == attachment.root(),
        "stable identity uses neither the target address nor the target directory");
    expect(route.human_identity.agent_id == "operator-manifest-id"
            && route.human_identity.true_name == "Operator True Name"
            && route.human_identity.nickname == "Op"
            && route.human_identity.address == std::string(operator_address)
            && route.human_identity.state == "idle",
        "the human sender card carries exactly the accepted typed manifest fields");
    expect(route.human_identity.role_evidence == AgentRole::human,
        "the human sender card carries explicit human-role evidence");
    expect(report.human_candidate_keys == std::vector<fs::path>{fs::path(operator_key)}
            && report.human_candidate_count == 1,
        "bounded human candidate evidence names the one resolved human");
    expect(snapshot_tree(base) == before,
        "route resolution changes no project path");
}
void test_stable_identity_survives_route_rename(const fs::path &base) {
    auto first = make_project(base, "stable/one");
    auto second = make_project(base, "stable/two");
    if (!first || !second) return;
    const auto original = resolve_direct_conversation_route(
        *first.attachment, snapshot(standard_rows()), fs::path(worker_key));
    auto renamed_rows = standard_rows();
    renamed_rows[0].directory_key = "operator-2";
    renamed_rows[0].identity->address = "mail/operator-desk-moved";
    renamed_rows[1].directory_key = "worker-dir-renamed";
    renamed_rows[1].identity->address = "mail/worker-post-moved";
    const auto renamed = resolve_direct_conversation_route(*first.attachment,
        snapshot(renamed_rows), fs::path("worker-dir-renamed"));
    expect(original.route && renamed.route
            && original.route->thread_key == renamed.route->thread_key,
        "stable thread identity survives an address and directory rename");
    expect(renamed.route
            && renamed.route->target_directory_key == fs::path("worker-dir-renamed")
            && renamed.route->target_address == "mail/worker-post-moved"
            && renamed.route->human_directory_key == fs::path("operator-2")
            && renamed.route->human_address == "mail/operator-desk-moved",
        "current route keys and addresses follow the rename as route authority");
    const auto elsewhere = resolve_direct_conversation_route(
        *second.attachment, snapshot(standard_rows()), fs::path(worker_key));
    expect(original.route && elsewhere.route
            && !(original.route->thread_key == elsewhere.route->thread_key),
        "the same target agent_id in another project is a different thread");
}
void test_failure_matrix(const fs::path &base) {
    auto attached = make_project(base, "failures"); if (!attached) return;
    const auto &attachment = *attached.attachment;
    const auto complete = snapshot(standard_rows());
    expect_failure(attachment, snapshot(standard_rows(), false), fs::path(worker_key),
        DirectRouteFailure::identity_projection_incomplete,
        "an otherwise resolvable but incomplete projection fails closed");
    expect_failure(attachment, complete, std::nullopt,
        DirectRouteFailure::no_selected_agent, "an absent selection fails closed");
    expect_failure(attachment, complete, fs::path(),
        DirectRouteFailure::no_selected_agent, "an empty selection fails closed");
    expect_failure(attachment, complete, fs::path("worker-di"),
        DirectRouteFailure::selected_agent_not_found,
        "an inexact selected key never falls back to another valid Agent");
    for (const auto kind : {AgentManifestKind::malformed, AgentManifestKind::unsafe}) {
        auto rows = standard_rows();
        rows[1] = row(worker_key, AgentRole::unknown, std::nullopt, kind);
        expect_failure(attachment, snapshot(rows), fs::path(worker_key),
            DirectRouteFailure::selected_agent_not_valid,
            "a malformed or unsafe selected row is never routable");
    }
    auto without_identity = standard_rows();
    without_identity[1].identity.reset();
    expect_failure(attachment, snapshot(without_identity), fs::path(worker_key),
        DirectRouteFailure::selected_agent_not_valid,
        "a selected row without typed identity facts is never routable");
    expect_failure(attachment, complete, fs::path(operator_key),
        DirectRouteFailure::selected_agent_is_human,
        "the human row is never its own conversation target");
    for (const auto &absent : {std::optional<std::string>{},
            std::optional<std::string>{""}}) {
        auto rows = standard_rows(); rows[1].identity->agent_id = absent;
        expect_failure(attachment, snapshot(rows), fs::path(worker_key),
            DirectRouteFailure::target_agent_id_missing,
            "a target without a manifest agent_id has no stable thread identity");
        rows = standard_rows(); rows[1].identity->address = absent;
        expect_failure(attachment, snapshot(rows), fs::path(worker_key),
            DirectRouteFailure::target_address_missing,
            "a target without a manifest address is not routable");
        rows = standard_rows(); rows[0].identity->address = absent;
        expect_failure(attachment, snapshot(rows), fs::path(worker_key),
            DirectRouteFailure::human_address_missing,
            "a human without a manifest address is not routable");
    }
    auto no_human = standard_rows(); no_human[0].role = AgentRole::main;
    expect_failure(attachment, snapshot(no_human), fs::path(worker_key),
        DirectRouteFailure::human_missing, "no valid human row fails closed");
    auto malformed_human = standard_rows();
    malformed_human[0] = row(operator_key, AgentRole::human, std::nullopt,
        AgentManifestKind::malformed);
    expect_failure(attachment, snapshot(malformed_human), fs::path(worker_key),
        DirectRouteFailure::human_missing,
        "a malformed human row is not a usable sender identity");
    auto ambiguous = standard_rows();
    ambiguous.push_back(row("operator-2", AgentRole::human,
        facts(std::string("second-human"), std::string("mail/second-desk"))));
    const auto ambiguous_report = resolve_direct_conversation_route(
        attachment, snapshot(ambiguous), fs::path(worker_key));
    expect(!ambiguous_report.route
            && ambiguous_report.failure == DirectRouteFailure::human_ambiguous,
        "two valid humans fail closed rather than silently picking one");
    expect(ambiguous_report.human_candidate_keys == std::vector<fs::path>{
                fs::path(operator_key), fs::path("operator-2")}
            && ambiguous_report.human_candidate_count == 2,
        "ambiguous evidence names each human candidate for degraded presentation");
    auto conflict = standard_rows();
    conflict[0].identity->address = std::string(worker_address);
    expect_failure(attachment, snapshot(conflict), fs::path(worker_key),
        DirectRouteFailure::human_target_address_conflict,
        "a human and target sharing one address is never a direct route");
}
void test_candidate_evidence_is_bounded(const fs::path &base) {
    auto attached = make_project(base, "bounded"); if (!attached) return;
    std::vector<AgentManifestDiscoveryItem> rows;
    for (int index = 0; index < 40; ++index) {
        const auto suffix = std::to_string(index);
        rows.push_back(row("operator-" + suffix, AgentRole::human,
            facts("human-" + suffix, "mail/desk-" + suffix)));
    }
    rows.push_back(row(worker_key, AgentRole::agent,
        facts(std::string(worker_agent_id), std::string(worker_address))));
    const auto report = resolve_direct_conversation_route(
        *attached.attachment, snapshot(rows), fs::path(worker_key));
    expect(report.failure == DirectRouteFailure::human_ambiguous
            && report.human_candidate_keys.size() == human_candidate_key_limit
            && report.human_candidate_count == 40,
        "human candidate evidence is bounded while the exact count stays truthful");
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 2) return (std::cerr << "expected one test-sandbox path\n", 2);
    const auto base = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(base, error); fs::create_directories(base, error);
    expect(!error, "test sandbox is created");
    test_resolved_route(base);
    test_stable_identity_survives_route_rename(base);
    test_failure_matrix(base);
    test_candidate_evidence_is_bounded(base);
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
