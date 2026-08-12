#ifdef QT_CORE_LIB
#error "Qt Core usage requirements must not propagate to this test consumer"
#endif
#if __has_include("agent_roster_presence.h") \
    && __has_include("agent_roster_presence_test_seam.h")
#include "agent_roster_presence.h"
#include "agent_roster_presence_test_seam.h"
#include "project_attachment.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace lingtai::desktop;
using namespace lingtai::desktop::testing;
namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n', ++failures;
}
void write_file(const fs::path &path, std::string_view contents) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    expect(!error, "fixture parent is created: " + path.string());
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    expect(stream.good(), "fixture file is written: " + path.string());
}
void write_agent(const fs::path &agents, std::string_view key,
        std::string_view manifest = R"({"admin":{}})") {
    write_file(agents / key / ".agent.json", manifest);
}

class FakeFilesystem final : public AgentRosterPresenceFilesystem {
public:
    std::vector<fs::path> entries;
    std::map<fs::path, AgentManifestFileRead> manifests;
    std::map<fs::path, AgentHeartbeatFileRead> heartbeats;
    std::set<fs::path> forbidden_heartbeat_keys;
    mutable int listing_calls = 0;
    mutable std::vector<fs::path> manifest_requests;
    mutable std::vector<fs::path> heartbeat_requests;

    AgentManifestDirectoryListing list_directory(
            const fs::path &) const override {
        ++listing_calls;
        return {AgentManifestDirectoryListing::Kind::complete, entries, {}};
    }
    AgentManifestFileRead read_manifest(
            const fs::path &, const fs::path &key) const override {
        manifest_requests.push_back(key);
        const auto found = manifests.find(key);
        return found == manifests.end()
            ? AgentManifestFileRead{AgentManifestFileReadKind::manifest_absent,
                {}, {}}
            : found->second;
    }
    AgentHeartbeatFileRead read_heartbeat(
            const fs::path &, const fs::path &key) const override {
        heartbeat_requests.push_back(key);
        if (forbidden_heartbeat_keys.contains(key)) {
            expect(false, key.string() + " heartbeat must not be opened");
            throw std::runtime_error("forbidden heartbeat read");
        }
        const auto found = heartbeats.find(key);
        return found == heartbeats.end()
            ? AgentHeartbeatFileRead{AgentHeartbeatFileReadKind::absent, {}, {}}
            : found->second;
    }
};

class TestClock final : public AgentRosterPresenceClock {
public:
    double now = 100.0;
    std::function<void()> before_return;
    mutable int calls = 0;
    double wall_now_seconds() const override {
        ++calls;
        if (before_return) before_return();
        return now;
    }
};

AgentRosterSnapshot scan(const fs::path &project, TestClock &clock,
        const AgentRosterPresenceFilesystem *filesystem = nullptr) {
    const auto attached = attach_project(project);
    expect(static_cast<bool>(attached), "fixture project attaches");
    if (!attached) return {};
    static_assert(noexcept(project_agent_roster(*attached.attachment)));
    static_assert(noexcept(testing::project_agent_roster(
        *attached.attachment, std::declval<const AgentRosterPresenceClock &>())));
    static_assert(noexcept(testing::project_agent_roster(*attached.attachment,
        std::declval<const AgentRosterPresenceFilesystem &>(),
        std::declval<const AgentRosterPresenceClock &>())));
    return filesystem
        ? testing::project_agent_roster(
            *attached.attachment, *filesystem, clock)
        : testing::project_agent_roster(*attached.attachment, clock);
}

std::size_t index_of(const AgentRosterSnapshot &snapshot, std::string_view key) {
    const auto found = std::ranges::find_if(snapshot.discovery.items,
        [&](const auto &item) { return item.directory_key == fs::path(key); });
    return found == snapshot.discovery.items.end()
        ? snapshot.discovery.items.size()
        : static_cast<std::size_t>(found - snapshot.discovery.items.begin());
}
const AgentRosterPresenceItem *presence(
        const AgentRosterSnapshot &snapshot, std::string_view key) {
    const auto index = index_of(snapshot, key);
    expect(index < snapshot.discovery.items.size(),
        std::string(key) + " discovery item exists");
    expect(index < snapshot.presence_by_item.size(),
        std::string(key) + " presence item exists");
    return index < snapshot.presence_by_item.size()
        ? &snapshot.presence_by_item[index] : nullptr;
}
void expect_presence(const AgentRosterSnapshot &snapshot, std::string_view key,
        AgentPresenceKind kind, AgentHeartbeatObservationState observation,
        AgentHeartbeatDiagnosticKind diagnostic) {
    const auto found = presence(snapshot, key);
    if (!found) return;
    expect(found->presence == kind, std::string(key) + " presence kind");
    expect(found->heartbeat_source.observation == observation,
        std::string(key) + " heartbeat observation");
    expect(found->heartbeat_source.diagnostic == diagnostic,
        std::string(key) + " heartbeat diagnostic");
    const auto index = index_of(snapshot, key);
    expect(found->heartbeat_source.path
            == snapshot.discovery.items[index].directory_path / ".agent.heartbeat",
        std::string(key) + " heartbeat provenance path");
}

std::map<std::string, std::string> snapshot_tree(const fs::path &root) {
    std::map<std::string, std::string> result;
    std::error_code error;
    for (fs::recursive_directory_iterator i(root, error), end;
         !error && i != end; i.increment(error)) {
        const auto relative = i->path().lexically_relative(root).generic_string();
        const auto status = i->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) {
            result[relative] = "symlink:" + fs::read_symlink(i->path()).string();
        } else if (fs::is_directory(status)) {
            result[relative] = "directory";
        } else if (fs::is_regular_file(status)) {
            std::ifstream stream(i->path(), std::ios::binary);
            result[relative] = "file:" + std::string{
                std::istreambuf_iterator<char>(stream), {}};
        } else result[relative] = "other";
    }
    expect(!error, "fixture snapshot is complete");
    return result;
}

void test_role_matrix_and_short_circuit(const fs::path &base) {
    const auto project = base / "roles/project";
    fs::create_directories(project);
    FakeFilesystem filesystem;
    const std::map<fs::path, std::pair<std::string, AgentRole>> cases = {
        {"human-missing", {R"({})", AgentRole::human}},
        {"human-null", {R"({"admin":null})", AgentRole::human}},
        {"main", {R"({"admin":{"manage":true,"other":false}})", AgentRole::main}},
        {"empty", {R"({"admin":{}})", AgentRole::agent}},
        {"all-false", {R"({"admin":{"a":false,"b":false}})", AgentRole::agent}},
        {"nested-true", {R"({"admin":{"a":{"nested":true}}})", AgentRole::agent}},
        {"string-true", {R"({"admin":"true"})", AgentRole::agent}},
        {"boolean-true", {R"({"admin":true})", AgentRole::agent}},
        {"number", {R"({"admin":1})", AgentRole::agent}},
        {"array", {R"({"admin":[true]})", AgentRole::agent}},
    };
    for (const auto &[key, value] : cases) {
        filesystem.entries.push_back(key);
        filesystem.manifests[key] = {
            AgentManifestFileReadKind::read, value.first, {}};
    }
    filesystem.entries.insert(filesystem.entries.end(), {"malformed", "unsafe"});
    filesystem.manifests["malformed"] = {
        AgentManifestFileReadKind::read, "{", {}};
    filesystem.manifests["unsafe"] = {
        AgentManifestFileReadKind::unsafe_symlink, {}, {}};
    filesystem.forbidden_heartbeat_keys = {
        "human-missing", "human-null", "malformed", "unsafe"};
    TestClock clock;
    const auto result = scan(project, clock, &filesystem);
    expect(result.projection_complete, "role projection completes");
    expect(filesystem.listing_calls == 1, "accepted discovery is called once");
    expect(clock.calls == 1, "wall clock is read once");
    expect(filesystem.manifest_requests.size() == filesystem.entries.size(),
        "projection does not re-read any manifest");
    for (const auto &key : filesystem.entries) {
        expect(std::ranges::count(filesystem.manifest_requests, key) == 1,
            key.string() + " manifest is observed exactly once");
    }
    for (const auto &[key, value] : cases) {
        const auto index = index_of(result, key.string());
        expect(index < result.discovery.items.size(), key.string() + " is visible");
        if (index < result.discovery.items.size()) {
            expect(result.discovery.items[index].role == value.second,
                key.string() + " exact role");
        }
    }
    for (const auto key : {"malformed", "unsafe"}) {
        const auto index = index_of(result, key);
        expect(index < result.discovery.items.size()
                && result.discovery.items[index].role == AgentRole::unknown,
            std::string(key) + " role stays unknown");
        expect_presence(result, key, AgentPresenceKind::unknown,
            AgentHeartbeatObservationState::not_applicable,
            AgentHeartbeatDiagnosticKind::none);
    }
    for (const auto key : {"human-missing", "human-null"}) {
        expect_presence(result, key, AgentPresenceKind::alive_human,
            AgentHeartbeatObservationState::not_applicable,
            AgentHeartbeatDiagnosticKind::none);
    }
}

void test_strict_clock_matrix(const fs::path &base) {
    const auto project = base / "clock/project";
    fs::create_directories(project);
    FakeFilesystem filesystem;
    const std::map<fs::path, AgentHeartbeatFileRead> cases = {
        {"zero", {AgentHeartbeatFileReadKind::read, "100", {}}},
        {"below", {AgentHeartbeatFileReadKind::read, "95.0000001", {}}},
        {"exact", {AgentHeartbeatFileReadKind::read, "95", {}}},
        {"above", {AgentHeartbeatFileReadKind::read, "94.999", {}}},
        {"absent", {AgentHeartbeatFileReadKind::absent, {}, {}}},
        {"whitespace", {AgentHeartbeatFileReadKind::read, " \t99.5\r\n", {}}},
        {"leading-dot", {AgentHeartbeatFileReadKind::read, ".995e2", {}}},
        {"trailing-dot", {AgentHeartbeatFileReadKind::read, "99.", {}}},
        {"plus", {AgentHeartbeatFileReadKind::read, "+99.5", {}}},
        {"nonnumeric", {AgentHeartbeatFileReadKind::read, "soon", {}}},
        {"trailing", {AgentHeartbeatFileReadKind::read, "99x", {}}},
        {"hex", {AgentHeartbeatFileReadKind::read, "0x1.8p+6", {}}},
        {"overflow", {AgentHeartbeatFileReadKind::read, "1e9999", {}}},
        {"empty", {AgentHeartbeatFileReadKind::read, " \n", {}}},
        {"nan", {AgentHeartbeatFileReadKind::read, "NaN", {}}},
        {"plus-inf", {AgentHeartbeatFileReadKind::read, "+Inf", {}}},
        {"minus-inf", {AgentHeartbeatFileReadKind::read, "-Inf", {}}},
        {"future", {AgentHeartbeatFileReadKind::read, "101", {}}},
    };
    for (const auto &[key, heartbeat] : cases) {
        filesystem.entries.push_back(key);
        filesystem.manifests[key] = {
            AgentManifestFileReadKind::read, R"({"admin":{}})", {}};
        filesystem.heartbeats[key] = heartbeat;
    }
    TestClock clock;
    const auto result = scan(project, clock, &filesystem);
    expect(clock.calls == 1, "strict matrix shares exactly one now");
    for (const auto key : {
            "zero", "below", "whitespace", "leading-dot", "trailing-dot",
            "plus"}) {
        expect_presence(result, key, AgentPresenceKind::alive,
            AgentHeartbeatObservationState::read_this_scan,
            AgentHeartbeatDiagnosticKind::none);
    }
    for (const auto key : {"exact", "above"}) {
        expect_presence(result, key, AgentPresenceKind::stale,
            AgentHeartbeatObservationState::read_this_scan,
            AgentHeartbeatDiagnosticKind::none);
    }
    expect_presence(result, "absent", AgentPresenceKind::missing,
        AgentHeartbeatObservationState::absent,
        AgentHeartbeatDiagnosticKind::none);
    for (const auto key : {
            "nonnumeric", "trailing", "hex", "overflow", "empty"}) {
        expect_presence(result, key, AgentPresenceKind::invalid,
            AgentHeartbeatObservationState::read_this_scan,
            AgentHeartbeatDiagnosticKind::invalid_number);
    }
    for (const auto key : {"nan", "plus-inf", "minus-inf"}) {
        expect_presence(result, key, AgentPresenceKind::invalid,
            AgentHeartbeatObservationState::read_this_scan,
            AgentHeartbeatDiagnosticKind::nonfinite);
        const auto found = presence(result, key);
        if (found) expect(!found->age_seconds,
            std::string(key) + " never exposes nonfinite age");
    }
    expect_presence(result, "future", AgentPresenceKind::invalid,
        AgentHeartbeatObservationState::read_this_scan,
        AgentHeartbeatDiagnosticKind::future);
    const auto future = presence(result, "future");
    expect(future && future->age_seconds && *future->age_seconds == 0.0,
        "future display age is clamped to exactly zero");
    expect(future && future->timestamp_seconds
            && *future->timestamp_seconds == 101.0,
        "future finite timestamp provenance is retained");
    for (const auto &item : result.presence_by_item) {
        expect(!item.age_seconds || (std::isfinite(*item.age_seconds)
                && *item.age_seconds >= 0.0),
            "every exposed display age is finite and nonnegative");
    }

    TestClock nonfinite_clock;
    nonfinite_clock.now = std::numeric_limits<double>::infinity();
    const auto nonfinite_now = scan(project, nonfinite_clock, &filesystem);
    expect_presence(nonfinite_now, "zero", AgentPresenceKind::invalid,
        AgentHeartbeatObservationState::read_this_scan,
        AgentHeartbeatDiagnosticKind::nonfinite);
    const auto nonfinite = presence(nonfinite_now, "zero");
    expect(nonfinite && !nonfinite->age_seconds,
        "a nonfinite clock never exposes nonfinite age");
}

void test_locale_independent_decimal(const fs::path &base) {
    const auto original = std::string(std::setlocale(LC_NUMERIC, nullptr));
    const char *selected = nullptr;
    for (const auto candidate : {
            "fr_FR.UTF-8", "de_DE.UTF-8", "fr_FR", "de_DE"}) {
        if (std::setlocale(LC_NUMERIC, candidate)
            && std::localeconv()->decimal_point[0] == ',') {
            selected = candidate;
            break;
        }
    }
    if (!selected) {
        std::cout << "SKIP: comma-decimal LC_NUMERIC locale unavailable\n";
        std::setlocale(LC_NUMERIC, original.c_str());
        return;
    }

    const auto project = base / "locale/project";
    fs::create_directories(project);
    FakeFilesystem filesystem;
    filesystem.entries = {"decimal", "hex"};
    for (const auto &key : filesystem.entries) {
        filesystem.manifests[key] = {
            AgentManifestFileReadKind::read, R"({"admin":{}})", {}};
    }
    filesystem.heartbeats["decimal"] = {
        AgentHeartbeatFileReadKind::read, "99.5", {}};
    filesystem.heartbeats["hex"] = {
        AgentHeartbeatFileReadKind::read, "0x1.8p+6", {}};
    TestClock clock;
    const auto result = scan(project, clock, &filesystem);
    std::setlocale(LC_NUMERIC, original.c_str());
    expect_presence(result, "decimal", AgentPresenceKind::alive,
        AgentHeartbeatObservationState::read_this_scan,
        AgentHeartbeatDiagnosticKind::none);
    expect_presence(result, "hex", AgentPresenceKind::invalid,
        AgentHeartbeatObservationState::read_this_scan,
        AgentHeartbeatDiagnosticKind::invalid_number);
}

void test_default_reader_and_no_write(const fs::path &base) {
    const auto scope = base / "default";
    const auto project = scope / "project";
    const auto agents = project / ".lingtai";
    for (const auto key : {"fresh", "missing", "symlink", "dangling",
            "nonregular", "oversized", "status-absent", "status-malformed"}) {
        write_agent(agents, key);
    }
    write_file(agents / "fresh/.agent.heartbeat", "100");
    write_file(agents / "status-absent/.agent.heartbeat", "99");
    write_file(agents / "status-malformed/.agent.heartbeat", "98");
    write_file(agents / "status-malformed/.status.json", "{");
    fs::create_directories(agents / "nonregular/.agent.heartbeat");
    write_file(agents / "oversized/.agent.heartbeat", std::string(129, '1'));
    const auto outside = scope / "global/outside-heartbeat";
    write_file(outside, "100");
    write_file(scope / "global/marker", "external-marker");
    std::error_code error;
    fs::create_symlink(outside, agents / "symlink/.agent.heartbeat", error);
    expect(!error, "heartbeat symlink fixture is created");
    fs::create_symlink(scope / "global/absent", agents / "dangling/.agent.heartbeat",
        error);
    expect(!error, "dangling heartbeat fixture is created");
    const auto before = snapshot_tree(scope);
    TestClock clock;
    const auto result = scan(project, clock);
    expect(result.projection_complete, "default projection completes");
    for (const auto key : {"fresh", "status-absent", "status-malformed"}) {
        expect_presence(result, key, AgentPresenceKind::alive,
            AgentHeartbeatObservationState::read_this_scan,
            AgentHeartbeatDiagnosticKind::none);
    }
    expect_presence(result, "missing", AgentPresenceKind::missing,
        AgentHeartbeatObservationState::absent,
        AgentHeartbeatDiagnosticKind::none);
    for (const auto key : {"symlink", "dangling"}) {
        expect_presence(result, key, AgentPresenceKind::unavailable,
            AgentHeartbeatObservationState::rejected_unsafe,
            AgentHeartbeatDiagnosticKind::unsafe_symlink);
    }
    expect_presence(result, "nonregular", AgentPresenceKind::unavailable,
        AgentHeartbeatObservationState::observed_unavailable,
        AgentHeartbeatDiagnosticKind::not_regular);
    expect_presence(result, "oversized", AgentPresenceKind::unavailable,
        AgentHeartbeatObservationState::observed_unavailable,
        AgentHeartbeatDiagnosticKind::too_large);
    expect(snapshot_tree(scope) == before,
        "roster scan preserves project, status, and external markers");
}

void test_real_container_races(const fs::path &base) {
    const auto run = [&](std::string_view name,
            const std::function<void(const fs::path &, const fs::path &)> &mutate,
            AgentHeartbeatObservationState observation,
            AgentHeartbeatDiagnosticKind diagnostic) {
        const auto project = base / "races" / name / "project";
        const auto agents = project / ".lingtai";
        write_agent(agents, "agent");
        write_file(agents / "agent/.agent.heartbeat", "100");
        TestClock clock;
        clock.before_return = [&] { mutate(project, agents); };
        const auto result = scan(project, clock);
        expect(result.discovery.scan.state == AgentManifestScanState::complete,
            std::string(name) + " preserves accepted discovery scan");
        expect(result.discovery.items.size() == 1,
            std::string(name) + " preserves discovered item");
        expect_presence(result, "agent", AgentPresenceKind::unavailable,
            observation, diagnostic);
    };
    run("child-deleted", [](const fs::path &, const fs::path &agents) {
        std::error_code error;
        fs::remove_all(agents / "agent", error);
        expect(!error, "candidate deletion succeeds");
    }, AgentHeartbeatObservationState::observed_unavailable,
       AgentHeartbeatDiagnosticKind::container_unavailable);
    run("child-replaced", [](const fs::path &, const fs::path &agents) {
        std::error_code error;
        fs::rename(agents / "agent", agents / "old-agent", error);
        expect(!error, "candidate old inode is retained");
        write_agent(agents, "agent");
        write_file(agents / "agent/.agent.heartbeat", "100");
    }, AgentHeartbeatObservationState::observed_unavailable,
       AgentHeartbeatDiagnosticKind::container_changed);
    run("child-symlink", [](const fs::path &project, const fs::path &agents) {
        std::error_code error;
        fs::rename(agents / "agent", agents / "old-agent", error);
        expect(!error, "old candidate stays available for no-follow proof");
        const auto outside = project.parent_path() / "outside-agent";
        write_file(outside / ".agent.heartbeat", "100");
        write_file(outside / "marker", "outside-marker");
        fs::create_directory_symlink(outside, agents / "agent", error);
        expect(!error, "candidate symlink replacement is created");
    }, AgentHeartbeatObservationState::rejected_unsafe,
       AgentHeartbeatDiagnosticKind::unsafe_symlink);
    run("root-deleted", [](const fs::path &project, const fs::path &agents) {
        std::error_code error;
        fs::rename(agents, project / ".lingtai-old", error);
        expect(!error, "root deletion fixture is deterministic");
    }, AgentHeartbeatObservationState::observed_unavailable,
       AgentHeartbeatDiagnosticKind::container_unavailable);
    run("root-replaced", [](const fs::path &project, const fs::path &agents) {
        std::error_code error;
        fs::rename(agents, project / ".lingtai-old", error);
        expect(!error, "old root inode is retained");
        write_agent(project / ".lingtai", "agent");
        write_file(project / ".lingtai/agent/.agent.heartbeat", "100");
    }, AgentHeartbeatObservationState::observed_unavailable,
       AgentHeartbeatDiagnosticKind::container_changed);
    run("root-symlink", [](const fs::path &project, const fs::path &agents) {
        std::error_code error;
        fs::rename(agents, project / ".lingtai-old", error);
        expect(!error, "old root stays available for no-follow proof");
        const auto outside = project.parent_path() / "outside-root";
        write_file(outside / "agent/.agent.heartbeat", "100");
        write_file(outside / "marker", "outside-marker");
        fs::create_directory_symlink(outside, project / ".lingtai", error);
        expect(!error, "root symlink replacement is created");
    }, AgentHeartbeatObservationState::rejected_unsafe,
       AgentHeartbeatDiagnosticKind::unsafe_symlink);
}

void test_sibling_independence(const fs::path &base) {
    const auto project = base / "siblings/project";
    fs::create_directories(project);
    FakeFilesystem filesystem;
    filesystem.entries = {"z-live", "a-error"};
    for (const auto &key : filesystem.entries) {
        filesystem.manifests[key] = {
            AgentManifestFileReadKind::read, R"({"admin":{}})", {}};
    }
    filesystem.heartbeats["a-error"] = {AgentHeartbeatFileReadKind::io_error,
        {}, std::make_error_code(std::errc::io_error)};
    filesystem.heartbeats["z-live"] = {
        AgentHeartbeatFileReadKind::read, "100", {}};
    TestClock clock;
    const auto result = scan(project, clock, &filesystem);
    expect(std::ranges::equal(
            result.discovery.items | std::views::transform(
                [](const auto &item) { return item.directory_key; }),
            std::vector<fs::path>{"a-error", "z-live"}),
        "roster preserves deterministic discovery order");
    expect_presence(result, "a-error", AgentPresenceKind::unavailable,
        AgentHeartbeatObservationState::observed_unavailable,
        AgentHeartbeatDiagnosticKind::io_error);
    expect_presence(result, "z-live", AgentPresenceKind::alive,
        AgentHeartbeatObservationState::read_this_scan,
        AgentHeartbeatDiagnosticKind::none);
    expect(filesystem.heartbeat_requests
            == std::vector<fs::path>({"a-error", "z-live"}),
        "one heartbeat failure does not stop later siblings");
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return (std::cerr << "expected one test-sandbox path\n", 2);
    const auto requested = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(requested, error);
    fs::create_directories(requested, error);
    expect(!error, "test sandbox is created");
    const auto base = fs::canonical(requested, error);
    expect(!error, "test sandbox has a canonical path");
    test_role_matrix_and_short_circuit(base);
    test_strict_clock_matrix(base);
    test_locale_independent_decimal(base);
    test_default_reader_and_no_write(base);
    test_real_container_races(base);
    test_sibling_independence(base);
    fs::remove_all(base, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " roster presence assertion(s) failed\n";
        return 1;
    }
    std::cout << "AGENT_ROSTER_PRESENCE_CONTRACT_OK\n";
    return 0;
}
#else
#include <iostream>
int main() {
    std::cerr << "FAIL: agent roster role and presence behavior is unavailable\n";
    return 1;
}
#endif
