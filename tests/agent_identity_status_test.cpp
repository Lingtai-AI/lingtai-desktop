#ifdef QT_CORE_LIB
#error "Qt Core usage requirements must not propagate to this test consumer"
#endif
#if __has_include("agent_identity_status.h") \
    && __has_include("agent_identity_status_test_seam.h")
// The public projection seam is present and remains Qt-free to consumers.
#include "agent_identity_status.h"
#include "agent_identity_status_test_seam.h"
#include "project_attachment.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;
using namespace lingtai::desktop; using namespace lingtai::desktop::testing;
namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n', ++failures;
}
void write_file(const fs::path &path, std::string_view contents) {
    std::error_code error; fs::create_directories(path.parent_path(), error);
    expect(!error, "fixture parent is created");
    std::ofstream stream(path, std::ios::binary); stream << contents;
    expect(stream.good(), "fixture file is written");
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
class FakeFilesystem final : public AgentIdentityStatusFilesystem {
public:
    std::vector<fs::path> entries;
    mutable std::vector<fs::path> manifest_requests, heartbeat_requests, status_requests;
    std::map<fs::path, AgentManifestFileRead> manifests;
    std::map<fs::path, AgentHeartbeatFileRead> heartbeats;
    std::map<fs::path, AgentStatusFileRead> statuses;
    mutable int listing_calls = 0;
    AgentManifestDirectoryListing list_directory(const fs::path &) const override {
        ++listing_calls;
        return {AgentManifestDirectoryListing::Kind::complete, entries, {}};
    }
    AgentManifestFileRead read_manifest(const fs::path &,
            const fs::path &key) const override {
        manifest_requests.push_back(key);
        const auto found = manifests.find(key);
        return found == manifests.end()
            ? AgentManifestFileRead{AgentManifestFileReadKind::manifest_absent, {}, {}}
            : found->second;
    }
    AgentHeartbeatFileRead read_heartbeat(const fs::path &,
            const fs::path &key) const override {
        heartbeat_requests.push_back(key);
        const auto found = heartbeats.find(key);
        return found == heartbeats.end()
            ? AgentHeartbeatFileRead{AgentHeartbeatFileReadKind::absent, {}, {}}
            : found->second;
    }
    AgentStatusFileRead read_status(const fs::path &,
            const fs::path &key) const override {
        status_requests.push_back(key);
        const auto found = statuses.find(key);
        if (found != statuses.end()) return found->second;
        return {AgentStatusFileReadKind::absent, {}, {}};
    }
};
class TestClock final : public AgentRosterPresenceClock {
public:
    double now = 200.0; mutable int calls = 0;
    double wall_now_seconds() const override { ++calls; return now; }
};
AgentIdentityStatusSnapshot scan(const fs::path &project, FakeFilesystem &filesystem,
        TestClock &clock) {
    const auto attached = attach_project(project);
    expect(static_cast<bool>(attached), "fixture project attaches");
    if (!attached) return {};
    static_assert(noexcept(project_agent_identity_status(*attached.attachment)));
    static_assert(noexcept(testing::project_agent_identity_status(
        *attached.attachment, filesystem, clock)));
    return testing::project_agent_identity_status(
        *attached.attachment, filesystem, clock);
}
std::size_t index_of(const AgentIdentityStatusSnapshot &snapshot, std::string_view key) {
    const auto found = std::ranges::find_if(snapshot.roster.discovery.items,
        [&](const auto &item) { return item.directory_key == fs::path(key); });
    return found == snapshot.roster.discovery.items.end()
        ? snapshot.roster.discovery.items.size()
        : static_cast<std::size_t>(found - snapshot.roster.discovery.items.begin());
}
const AgentIdentityStatusItem *detail(const AgentIdentityStatusSnapshot &snapshot,
        std::string_view key) {
    const auto index = index_of(snapshot, key);
    expect(index < snapshot.roster.discovery.items.size(),
        std::string(key) + " discovery item exists");
    expect(index < snapshot.detail_by_item.size(),
        std::string(key) + " detail item exists index-parallel");
    return index < snapshot.detail_by_item.size() ? &snapshot.detail_by_item[index] : nullptr;
}
void test_manifest_identity(const fs::path &base) {
    const auto project = base / "manifest/project"; fs::create_directories(project);
    FakeFilesystem filesystem;
    filesystem.entries = {"agent"};
    const std::string manifest = R"({"agent_id":"manifest-id","agent_name":"True Name",
        "nickname":"Mutable Nick","address":"mailbox/address","state":"idle","admin":{},
        "llm":{"provider":"openai","model":"gpt-x","base_url":"https://safe.example/v1",
        "api_compat":"responses","context_limit":131072,"api_key":"must-not-project"},
        "capabilities":["email",["custom",{}],"custom",42]})";
    filesystem.manifests["agent"] = {
        AgentManifestFileReadKind::read, manifest, {}, 100.0};
    filesystem.heartbeats["agent"] = {
        AgentHeartbeatFileReadKind::read, "199", {}};
    TestClock clock;
    const auto result = scan(project, filesystem, clock);
    expect(result.projection_complete && result.roster.projection_complete
            && result.observed_at_seconds == 200.0
            && result.roster.discovery.items.size() == 1,
        "composite completes index-parallel with one shared read time");
    if (result.roster.discovery.items.empty()) return;
    const auto &item = result.roster.discovery.items.front();
    expect(item.identity.has_value(), "valid manifest has typed identity facts");
    if (!item.identity) return;
    const auto &identity = *item.identity;
    expect(identity.agent_id == "manifest-id" && identity.true_name == "True Name"
            && identity.nickname == "Mutable Nick"
            && identity.address == "mailbox/address" && identity.state == "idle",
        "manifest identity, immutable name, nickname, address, and state are exact");
    expect(identity.llm.provider == "openai" && identity.llm.model == "gpt-x"
            && identity.llm.base_url == "https://safe.example/v1"
            && identity.llm.api_compat == "responses" && identity.llm.context_limit == 131072,
        "only exact safe live LLM fields are projected");
    expect(identity.capabilities.evidence == AgentManifestCapabilityEvidenceKind::partially_parsed,
        "raw capability parse evidence retains a mixed manifest");
    expect(identity.capabilities.manifest_names ==
            std::vector<std::string>({"email", "custom", "custom"}),
        "raw capability names preserve source order and duplicates");
    expect(identity.capabilities.display_names == std::vector<std::string>(
            {"system", "soul", "email", "psyche", "custom"}),
        "display capabilities prepend four intrinsics and deduplicate");
    expect(item.manifest_source.relative_path == fs::path("agent/.agent.json")
            && item.manifest_source.modified_at_seconds == 100.0
            && item.manifest_source.observed_at_seconds == 200.0
            && item.manifest_source.byte_count == manifest.size(),
        "manifest source retains relative path, mtime, read time, and bytes");
    expect(filesystem.listing_calls == 1 && clock.calls == 1
            && filesystem.manifest_requests == std::vector<fs::path>{"agent"}
            && filesystem.heartbeat_requests == std::vector<fs::path>{"agent"}
            && filesystem.status_requests == std::vector<fs::path>{"agent"},
        "composite uses one discovery, one clock, and one read per source");
}
void test_status_observations(const fs::path &base) {
    const auto project = base / "status/project"; fs::create_directories(project);
    FakeFilesystem filesystem;
    filesystem.entries = {
        "absent", "invalid-window", "malformed", "partial", "positive", "zero"};
    for (const auto &key : filesystem.entries) {
        filesystem.manifests[key] = {AgentManifestFileReadKind::read,
            R"({"agent_id":"manifest-id","state":"idle"})", {}, 150.0};
    }
    const AgentStatusFileRead malformed{
        AgentStatusFileReadKind::read, "{", {}, 151.0};
    filesystem.statuses["malformed"] = malformed;
    filesystem.statuses["partial"] = {AgentStatusFileReadKind::read,
        R"({"runtime":{"state":"future-kernel-state","running":"yes","pid":3.5}})",
        {}, 152.0};
    const AgentStatusFileRead positive{AgentStatusFileReadKind::read,
        R"({"identity":{"agent_id":"manifest-id"},"runtime":{"state":"active","running":true,
        "pid":4321,"state_changed_at":180,"last_progress_at":190,"no_progress_seconds":10.5},
        "active_turn":{"kind":"tool_call","id":"call-7","started_at":188,"elapsed_seconds":12},
        "tokens":{"context":{"system_tokens":10,"tools_tokens":20,"history_tokens":30,
        "total_tokens":60,"window_size":1000,"usage_pct":6.0,"fixed_tokens":30,
        "growing_tokens":30}}})", {}, 199.0};
    filesystem.statuses["positive"] = positive;
    for (const auto &[key, window] : std::vector<std::pair<std::string, std::string>>{
            {"zero", "0"}, {"invalid-window", R"("invalid")"}}) {
        filesystem.statuses[key] = {AgentStatusFileReadKind::read,
            "{\"tokens\":{\"context\":{\"window_size\":" + window
                + ",\"total_tokens\":55}}}", {}, 153.0};
    }
    TestClock clock;
    const auto result = scan(project, filesystem, clock);
    expect(result.projection_complete && result.detail_by_item.size() == 6,
        "status observations remain index-parallel");
    const auto *absent = detail(result, "absent");
    expect(absent && !absent->status && absent->status_source.observation
            == AgentStatusObservationState::absent && absent->status_source.diagnostic
            == AgentStatusDiagnosticKind::none,
        "absent status is typed independently");
    const auto *bad = detail(result, "malformed");
    expect(bad && !bad->status && bad->status_source.observation
            == AgentStatusObservationState::read_this_scan
            && bad->status_source.diagnostic == AgentStatusDiagnosticKind::invalid_json
            && bad->status_source.byte_count == malformed.bytes.size(),
        "malformed status retains read and parse evidence");
    const auto *partial_detail = detail(result, "partial");
    expect(partial_detail && partial_detail->status
            && partial_detail->status->runtime.state == "future-kernel-state"
            && !partial_detail->status->runtime.running && !partial_detail->status->runtime.pid,
        "unknown state survives while wrong-typed optional fields stay absent");
    const auto *full = detail(result, "positive");
    expect(full && full->status && full->status->agent_id == "manifest-id"
            && full->status->runtime.state == "active" && full->status->runtime.running == true
            && full->status->runtime.pid == 4321 && full->status->runtime.state_changed_at == 180.0
            && full->status->runtime.last_progress_at == 190.0
            && full->status->runtime.no_progress_seconds == 10.5,
        "status runtime and progress facts are exact");
    expect(full && full->status && full->status->active_turn
            && full->status->active_turn->kind == "tool_call" && full->status->active_turn->id == "call-7"
            && full->status->active_turn->started_at == 188.0
            && full->status->active_turn->elapsed_seconds == 12.0,
        "optional active-turn facts are exact");
    expect(full && full->status && full->status->context
            && full->status->context->window_size == 1000
            && full->status->context->total_tokens == 60
            && full->status->context->usage_percent == 6.0,
        "positive-window context is exposed from status only");
    for (const auto key : {"zero", "invalid-window"}) {
        const auto *value = detail(result, key);
        expect(value && value->status && !value->status->context,
            std::string(key) + " window makes context unavailable");
    }
    expect(full && full->status_source.relative_path == fs::path("positive/.status.json")
            && full->status_source.modified_at_seconds == 199.0
            && full->status_source.observed_at_seconds == 200.0
            && full->status_source.byte_count == positive.bytes.size(),
        "status source retains independent path, mtime, shared time, and bytes");
}
void test_manifest_status_relationships(const fs::path &base) {
    const auto project = base / "relations/project"; fs::create_directories(project);
    FakeFilesystem filesystem;
    filesystem.entries = {
        "equal", "malformed", "manifest-newer", "status-newer", "unassessable"};
    filesystem.manifests["status-newer"] = {AgentManifestFileReadKind::read,
        R"({"agent_id":"manifest-authority","agent_name":"Durable Name","state":"idle","admin":{}})", {}, 100.0};
    filesystem.manifests["manifest-newer"] = {AgentManifestFileReadKind::read,
        R"({"agent_id":"same-id","state":"asleep","admin":{}})", {}, 170.0};
    filesystem.manifests["equal"] = {AgentManifestFileReadKind::read,
        R"({"agent_id":"equal-id","state":"active"})", {}, 180.0};
    filesystem.manifests["unassessable"] = {AgentManifestFileReadKind::read,
        R"({"agent_id":"manifest-only","state":"idle"})", {}};
    filesystem.manifests["malformed"] = {
        AgentManifestFileReadKind::read, "{", {}, 120.0};
    const auto set_status = [&](const fs::path &key, std::string bytes,
            std::optional<double> mtime) {
        filesystem.statuses[key] = {AgentStatusFileReadKind::read,
            std::move(bytes), {}, mtime};
    };
    set_status("status-newer", R"({"identity":{"agent_id":"status-must-not-win"},
        "runtime":{"state":"active"}})", 150.0);
    set_status("manifest-newer", R"({"identity":{"agent_id":"same-id"},
        "runtime":{"state":"asleep"}})", 160.0);
    set_status("equal", R"({"identity":{"agent_id":"equal-id"},
        "runtime":{"state":"idle"}})", 180.0);
    set_status("unassessable", R"({"identity":{"agent_id":"manifest-only"},
        "runtime":{"state":"idle"}})", 190.0);
    set_status("malformed", "{", 130.0);
    filesystem.heartbeats["status-newer"] = {
        AgentHeartbeatFileReadKind::read, "199", {}};
    filesystem.heartbeats["manifest-newer"] = {
        AgentHeartbeatFileReadKind::read, "195", {}};
    TestClock clock;
    const auto result = scan(project, filesystem, clock);
    const auto *new_status = detail(result, "status-newer");
    const auto status_index = index_of(result, "status-newer");
    expect(new_status && new_status->relation.mtime_order == AgentManifestStatusMtimeOrder::status_newer
            && new_status->relation.agent_id == AgentManifestStatusAgreement::disagree
            && new_status->relation.state == AgentManifestStatusAgreement::disagree,
        "newer status retains deterministic ID and state disagreement");
    expect(status_index < result.roster.discovery.items.size()
            && result.roster.discovery.items[status_index].identity
            && result.roster.discovery.items[status_index].identity->agent_id == "manifest-authority"
            && result.roster.discovery.items[status_index].identity->true_name == "Durable Name"
            && result.roster.discovery.items[status_index].identity->state == "idle"
            && new_status && new_status->status && new_status->status->agent_id == "status-must-not-win"
            && new_status->status->runtime.state == "active",
        "status identity and state never replace manifest authority");
    const auto *new_manifest = detail(result, "manifest-newer");
    expect(new_manifest && new_manifest->relation.mtime_order == AgentManifestStatusMtimeOrder::manifest_newer
            && new_manifest->relation.agent_id == AgentManifestStatusAgreement::agree
            && new_manifest->relation.state == AgentManifestStatusAgreement::agree,
        "inverse ordering and agreements are exact");
    const auto *equal = detail(result, "equal");
    expect(equal && equal->relation.mtime_order
            == AgentManifestStatusMtimeOrder::same_time
            && equal->relation.agent_id == AgentManifestStatusAgreement::agree
            && equal->relation.state == AgentManifestStatusAgreement::disagree,
        "same-time relation preserves state disagreement");
    const auto *unknown = detail(result, "unassessable");
    expect(unknown && unknown->relation.mtime_order
            == AgentManifestStatusMtimeOrder::unassessable,
        "missing source mtime is explicitly unassessable");
    const auto malformed_index = index_of(result, "malformed");
    const auto *malformed = detail(result, "malformed");
    expect(malformed_index < result.roster.discovery.items.size()
            && result.roster.discovery.items[malformed_index].manifest_kind == AgentManifestKind::malformed
            && !result.roster.discovery.items[malformed_index].identity
            && malformed && !malformed->status
            && malformed->status_source.diagnostic == AgentStatusDiagnosticKind::invalid_json
            && malformed->relation.mtime_order == AgentManifestStatusMtimeOrder::unassessable,
        "malformed roster item remains visible with typed unavailable evidence");
    const auto manifest_newer_index = index_of(result, "manifest-newer");
    expect(status_index < result.roster.presence_by_item.size()
            && result.roster.presence_by_item[status_index].presence == AgentPresenceKind::alive
            && manifest_newer_index < result.roster.presence_by_item.size()
            && result.roster.presence_by_item[manifest_newer_index].presence == AgentPresenceKind::stale,
        "accepted strict heartbeat results remain unchanged beside status");
}
void test_default_reader_is_bounded_and_read_only(const fs::path &base) {
    const auto scope = base / "default"; const auto project = scope / "project";
    const auto agents = project / ".lingtai";
    for (const auto key : {"valid", "absent", "malformed", "oversized", "symlink"})
        write_file(agents / key / ".agent.json", "{}");
    write_file(agents / "valid/.status.json",
        R"({"runtime":{"state":"idle"},"tokens":{"context":{"window_size":0}}})");
    write_file(agents / "malformed/.status.json", "{");
    write_file(agents / "oversized/.status.json", std::string(1024U * 1024U + 1U, 'x'));
    const auto outside = scope / "outside/status.json"; write_file(outside, "{}");
    write_file(scope / "outside/marker", "outside-marker");
    std::error_code error;
    fs::create_symlink(outside, agents / "symlink/.status.json", error);
    expect(!error, "status symlink fixture is created");
    const auto before = snapshot_tree(scope);
    const auto attached = attach_project(project);
    expect(static_cast<bool>(attached), "default-reader project attaches");
    const auto result = attached
        ? project_agent_identity_status(*attached.attachment)
        : AgentIdentityStatusSnapshot{};
    expect(result.projection_complete, "default descriptor projection completes");
    const auto *valid = detail(result, "valid");
    expect(valid && valid->status && valid->status->runtime.state == "idle"
            && !valid->status->context,
        "default reader parses status and gates zero-window context");
    const auto *absent = detail(result, "absent");
    const auto *malformed = detail(result, "malformed");
    const auto *oversized = detail(result, "oversized");
    const auto *symlink = detail(result, "symlink");
    expect(absent && absent->status_source.observation == AgentStatusObservationState::absent,
        "default reader types absent status");
    expect(malformed && malformed->status_source.diagnostic == AgentStatusDiagnosticKind::invalid_json,
        "default reader types malformed status");
    expect(oversized && oversized->status_source.diagnostic == AgentStatusDiagnosticKind::too_large,
        "default reader bounds status bytes");
    expect(symlink && symlink->status_source.observation
            == AgentStatusObservationState::rejected_unsafe,
        "default reader rejects status symlinks without following");
    expect(snapshot_tree(scope) == before,
        "default identity/status projection writes no project or outside path");
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 2) return (std::cerr << "expected one test-sandbox path\n", 2);
    const auto base = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(base, error); fs::create_directories(base, error);
    expect(!error, "test sandbox is created");
    test_manifest_identity(base);
    test_status_observations(base);
    test_manifest_status_relationships(base);
    test_default_reader_is_bounded_and_read_only(base);
    fs::remove_all(base, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " identity/status assertion(s) failed\n";
        return 1;
    }
    std::cout << "AGENT_IDENTITY_STATUS_CONTRACT_OK\n";
    return 0;
}
#else
#include <iostream>
int main() {
    std::cerr << "FAIL: Agent identity/status source projection is unavailable\n";
    return 1;
}
#endif
