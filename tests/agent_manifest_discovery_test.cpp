#ifdef QT_CORE_LIB
#error "Qt Core usage requirements must not propagate to this test consumer"
#endif
#if __has_include("agent_manifest_discovery.h") \
    && __has_include("agent_manifest_discovery_test_seam.h")
#include "agent_manifest_discovery.h"
#include "agent_manifest_discovery_test_seam.h"
#include "project_attachment.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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
void write_manifest(const fs::path &agents, std::string_view name,
        std::string_view contents = "{}") {
    write_file(agents / name / ".agent.json", contents);
}
bool missing(const std::error_code &error) {
    return error == std::errc::no_such_file_or_directory
        || error == std::errc::not_a_directory;
}
class TestFilesystem final : public AgentManifestDiscoveryFilesystem {
public:
    mutable std::set<fs::path> remove_candidate_before_read;
    mutable std::set<fs::path> remove_before_read;
    std::map<fs::path, AgentManifestFileRead> read_overrides;
    std::error_code listing_error;
    std::vector<fs::path> injected_entries;
    bool reverse_and_duplicate = false;
    mutable std::vector<fs::path> read_requests;

    AgentManifestDirectoryListing list_directory(const fs::path &path) const override {
        AgentManifestDirectoryListing result;
        std::error_code error;
        fs::directory_iterator iterator(path, error), end;
        while (!error && iterator != end) {
            result.entries.push_back(iterator->path().filename());
            iterator.increment(error);
        }
        result.entries.insert(
            result.entries.end(), injected_entries.begin(), injected_entries.end());
        if (reverse_and_duplicate && !result.entries.empty()) {
            std::ranges::sort(result.entries);
            std::ranges::reverse(result.entries);
            result.entries.push_back("alpha");
        }
        result.error = listing_error ? listing_error : error;
        result.kind = !result.error ? AgentManifestDirectoryListing::Kind::complete
            : result.error == std::errc::no_such_file_or_directory
                ? AgentManifestDirectoryListing::Kind::missing
            : result.error == std::errc::not_a_directory
                ? AgentManifestDirectoryListing::Kind::not_directory
            : result.error == std::errc::too_many_symbolic_link_levels
                ? AgentManifestDirectoryListing::Kind::unsafe_symlink
            : result.error == std::errc::permission_denied
                ? AgentManifestDirectoryListing::Kind::unreadable
                : AgentManifestDirectoryListing::Kind::io_error;
        return result;
    }

    AgentManifestFileRead read_manifest(
            const fs::path &root, const fs::path &directory_key) const override {
        const auto directory = root / directory_key;
        const auto path = directory / ".agent.json";
        read_requests.push_back(path);
        if (remove_candidate_before_read.erase(directory) != 0U) {
            std::error_code error;
            fs::remove_all(directory, error);
            return {AgentManifestFileReadKind::candidate_absent, {}, error};
        }
        std::error_code directory_error;
        const auto directory_status = fs::symlink_status(directory, directory_error);
        if (directory_error || !fs::exists(directory_status))
            return {AgentManifestFileReadKind::candidate_absent, {}, directory_error};
        if (fs::is_symlink(directory_status))
            return {AgentManifestFileReadKind::candidate_unsafe_symlink, {}, {}};
        if (!fs::is_directory(directory_status))
            return {AgentManifestFileReadKind::candidate_not_directory, {}, {}};
        if (remove_before_read.erase(path) != 0U) {
            std::error_code error;
            fs::remove(path, error);
        }
        if (const auto i = read_overrides.find(path); i != read_overrides.end())
            return i->second;
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (error || !fs::exists(status)) {
            return {
                missing(error) || !fs::exists(status)
                    ? AgentManifestFileReadKind::manifest_absent
                    : AgentManifestFileReadKind::io_error,
                {}, error};
        }
        if (fs::is_symlink(status))
            return {AgentManifestFileReadKind::unsafe_symlink, {}, {}};
        if (!fs::is_regular_file(status))
            return {AgentManifestFileReadKind::not_regular, {}, {}};
        std::ifstream stream(path, std::ios::binary);
        std::string bytes{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        return stream.bad() ? AgentManifestFileRead{
                AgentManifestFileReadKind::io_error, {},
                std::make_error_code(std::errc::io_error)}
            : AgentManifestFileRead{
                AgentManifestFileReadKind::read, std::move(bytes), {}};
    }
};
AgentManifestDiscoveryReport scan(const fs::path &project,
        const AgentManifestDiscoveryFilesystem *filesystem = nullptr) {
    const auto attached = attach_project(project);
    expect(static_cast<bool>(attached), "fixture project attaches");
    if (!attached) return {};
    static_assert(noexcept(discover_agent_manifests(*attached.attachment)));
    static_assert(noexcept(testing::discover_agent_manifests(*attached.attachment,
        std::declval<const AgentManifestDiscoveryFilesystem &>())));
    auto report = filesystem
        ? testing::discover_agent_manifests(*attached.attachment, *filesystem)
        : discover_agent_manifests(*attached.attachment);
    expect(report.scan.path == attached.attachment->root() / ".lingtai",
        "scan retains canonical-root provenance");
    return report;
}
const AgentManifestDiscoveryItem *item(const AgentManifestDiscoveryReport &report,
        std::string_view key) {
    const auto found = std::ranges::find_if(report.items, [&](const auto &value) {
        return value.directory_key == fs::path(key);
    });
    return found == report.items.end() ? nullptr : &*found;
}
bool has_scan_diagnostic(const AgentManifestDiscoveryReport &report,
        AgentManifestScanDiagnosticKind kind,
        const fs::path &path,
        const std::error_code &error = {}) {
    return std::ranges::any_of(report.scan_diagnostics, [&](const auto &value) {
        return value.kind == kind && value.path == path
            && value.system_error == error;
    });
}
void expect_item(const AgentManifestDiscoveryReport &report, std::string_view key,
        AgentManifestKind kind, AgentManifestObservationState observation,
        AgentManifestDiagnosticKind diagnostic,
        const std::error_code &error = {}) {
    const auto found = item(report, key);
    expect(found != nullptr, std::string(key) + " is visible");
    if (!found) return;
    expect(found->manifest_kind == kind, std::string(key) + " manifest kind");
    expect(found->manifest_source.observation == observation,
        std::string(key) + " source observation");
    expect(found->manifest_source.diagnostic == diagnostic,
        std::string(key) + " exact source diagnostic");
    expect(found->manifest_source.system_error == error,
        std::string(key) + " exact source system error");
    expect(found->manifest_source.path
            == found->directory_path / ".agent.json",
        std::string(key) + " retains manifest provenance");
    expect(found->directory_path == report.scan.path / found->directory_key,
        std::string(key) + " key losslessly derives its contained path");
}
std::vector<fs::path> keys(const AgentManifestDiscoveryReport &report) {
    std::vector<fs::path> result;
    for (const auto &value : report.items) result.push_back(value.directory_key);
    return result;
}

std::map<std::string, std::string> snapshot(const fs::path &root) {
    std::map<std::string, std::string> result;
    std::error_code error;
    if (!fs::exists(root, error)) return result;
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
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()};
        } else result[relative] = "other";
    }
    expect(!error, "fixture snapshot is complete");
    return result;
}

void test_root_contract(const fs::path &base) {
    const auto missing_project = base / "root-missing/project";
    fs::create_directories(missing_project);
    const auto missing_report = scan(missing_project);
    expect(missing_report.scan.state == AgentManifestScanState::root_missing,
        "missing .lingtai is typed");
    expect(missing_report.items.empty(), "missing root has no items");

    const auto empty_project = base / "root-empty/project";
    fs::create_directories(empty_project / ".lingtai");
    const auto empty_report = scan(empty_project);
    expect(empty_report.scan.state == AgentManifestScanState::complete,
        "empty root is a successful scan");
    expect(empty_report.items.empty(), "empty root has no items");

    const auto file_project = base / "root-file/project";
    write_file(file_project / ".lingtai", "not a directory");
    expect(scan(file_project).scan.state
            == AgentManifestScanState::root_not_directory,
        "a root file is typed as not a directory");

    const auto link_project = base / "root-link/project";
    const auto link_target = base / "root-link/target";
    fs::create_directories(link_project);
    fs::create_directories(link_target);
    std::error_code error;
    fs::create_directory_symlink(link_target, link_project / ".lingtai", error);
    expect(!error, "root symlink fixture is created");
    expect(scan(link_project).scan.state
            == AgentManifestScanState::root_unsafe_symlink,
        "a .lingtai symlink is rejected without following it");

    TestFilesystem failed_listing;
    failed_listing.listing_error =
        std::make_error_code(std::errc::permission_denied);
    const auto unreadable_report = scan(empty_project, &failed_listing);
    expect(unreadable_report.scan.state == AgentManifestScanState::root_unreadable
            && unreadable_report.scan.system_error == std::errc::permission_denied,
        "root permission failure is typed without chmod");
    failed_listing.listing_error = std::make_error_code(std::errc::io_error);
    const auto io_report = scan(empty_project, &failed_listing);
    expect(io_report.scan.state == AgentManifestScanState::root_io_error
            && io_report.scan.system_error == std::errc::io_error,
        "root IO failure is distinct from permission failure");
    failed_listing.listing_error =
        std::make_error_code(std::errc::no_such_file_or_directory);
    const auto vanished_report = scan(empty_project, &failed_listing);
    expect(vanished_report.scan.state == AgentManifestScanState::root_missing
            && vanished_report.scan.system_error == std::errc::no_such_file_or_directory
            && vanished_report.items.empty()
            && vanished_report.scan_diagnostics.empty(),
        "root disappearance during enumeration fails closed");
}

void test_immediate_sorted_membership(const fs::path &base) {
    const auto project = base / "membership/project";
    const auto agents = project / ".lingtai";
    write_manifest(agents, "zeta");
    write_manifest(agents, "alpha");
    fs::create_directories(agents / "no-manifest");
    write_manifest(agents / "container", "nested-only");
    write_file(agents / "ordinary-file", "ignored");
    std::error_code error;
    fs::create_directory_symlink(
        agents / "alpha", agents / "inside-child-link", error);
    expect(!error, "contained child symlink fixture is created");
    fs::create_directory_symlink(
        agents / "absent-target", agents / "dangling-child-link", error);
    expect(!error, "dangling child symlink fixture is created");

    const auto default_report = scan(project);
    expect(keys(default_report) == std::vector<fs::path>({"alpha", "zeta"}),
        "production reader rejects child symlinks and ignores nesting");
    expect(has_scan_diagnostic(default_report,
            AgentManifestScanDiagnosticKind::child_unsafe_symlink,
            agents / "inside-child-link"),
        "production reader diagnoses a contained child symlink");

    TestFilesystem filesystem;
    filesystem.reverse_and_duplicate = true;
    filesystem.injected_entries = {"../escape", base / "absolute-escape"};
    const auto report = scan(project, &filesystem);
    expect(report.scan.state == AgentManifestScanState::complete,
        "normal membership scan completes");
    expect(keys(report) == std::vector<fs::path>({"alpha", "zeta"}),
        "only direct agents appear once in deterministic order");
    for (const auto name : {"alpha", "zeta"}) {
        expect_item(report, name, AgentManifestKind::valid,
            AgentManifestObservationState::read_this_scan,
            AgentManifestDiagnosticKind::none);
    }
    expect(has_scan_diagnostic(report,
            AgentManifestScanDiagnosticKind::child_unsafe_symlink,
            agents / "inside-child-link"),
        "contained child symlink rejection is diagnosed");
    expect(has_scan_diagnostic(report,
            AgentManifestScanDiagnosticKind::child_unsafe_symlink,
            agents / "dangling-child-link"),
        "dangling child symlink rejection is diagnosed");
    expect(std::ranges::none_of(filesystem.read_requests, [&](const auto &path) {
        return path == agents / "../escape/.agent.json"
            || path == base / "absolute-escape/.agent.json";
    }), "injected names cannot escape the attachment");
}

void test_manifest_tristate_and_races(const fs::path &base) {
    const auto project = base / "manifests/project";
    const auto agents = project / ".lingtai";
    write_manifest(agents, "valid");
    write_manifest(agents, "invalid", R"({"agent_id":"must-not-leak")");
    write_manifest(agents, "invalid-utf8", std::string_view{"{\"x\":\"\xFF\"}", 9});
    write_manifest(agents, "nonobject", "[]");
    fs::create_directories(agents / "nonregular/.agent.json");
    write_manifest(agents, "unreadable");
    write_manifest(agents, "io-error");
    fs::create_directories(agents / "absent");
    write_manifest(agents, "manifest-vanished");
    write_manifest(agents, "candidate-vanished");
    const auto outside = base / "manifests/outside.json";
    write_file(outside, "{}");
    fs::create_directories(agents / "unsafe");
    std::error_code error;
    fs::create_symlink(outside, agents / "unsafe/.agent.json", error);
    expect(!error, "manifest symlink fixture is created");

    const auto default_report = scan(project);
    expect_item(default_report, "unsafe", AgentManifestKind::unsafe,
        AgentManifestObservationState::rejected_unsafe,
        AgentManifestDiagnosticKind::unsafe_symlink);

    TestFilesystem filesystem;
    filesystem.read_overrides[agents / "unreadable/.agent.json"] = {
        AgentManifestFileReadKind::unreadable, {},
        std::make_error_code(std::errc::permission_denied)};
    filesystem.read_overrides[agents / "io-error/.agent.json"] = {
        AgentManifestFileReadKind::io_error, {},
        std::make_error_code(std::errc::io_error)};
    filesystem.remove_before_read.insert(
        agents / "manifest-vanished/.agent.json");
    filesystem.remove_candidate_before_read.insert(agents / "candidate-vanished");

    const auto report = scan(project, &filesystem);
    expect(keys(report) == std::vector<fs::path>({
            "invalid", "invalid-utf8", "io-error", "nonobject", "nonregular",
            "unreadable", "unsafe", "valid"}),
        "broken manifests remain visible while absent races are omitted");
    expect_item(report, "valid", AgentManifestKind::valid,
        AgentManifestObservationState::read_this_scan,
        AgentManifestDiagnosticKind::none);
    expect_item(report, "invalid", AgentManifestKind::malformed,
        AgentManifestObservationState::read_this_scan,
        AgentManifestDiagnosticKind::invalid_json);
    expect_item(report, "invalid-utf8", AgentManifestKind::malformed,
        AgentManifestObservationState::read_this_scan,
        AgentManifestDiagnosticKind::invalid_json);
    expect_item(report, "nonobject", AgentManifestKind::malformed,
        AgentManifestObservationState::read_this_scan,
        AgentManifestDiagnosticKind::not_object);
    expect_item(report, "nonregular", AgentManifestKind::malformed,
        AgentManifestObservationState::observed_unavailable,
        AgentManifestDiagnosticKind::not_regular);
    expect_item(report, "unreadable", AgentManifestKind::malformed,
        AgentManifestObservationState::observed_unavailable,
        AgentManifestDiagnosticKind::unreadable,
        std::make_error_code(std::errc::permission_denied));
    expect_item(report, "io-error", AgentManifestKind::malformed,
        AgentManifestObservationState::observed_unavailable,
        AgentManifestDiagnosticKind::io_error,
        std::make_error_code(std::errc::io_error));
    expect_item(report, "unsafe", AgentManifestKind::unsafe,
        AgentManifestObservationState::rejected_unsafe,
        AgentManifestDiagnosticKind::unsafe_symlink);
}

void test_independent_errors_and_fail_closed(const fs::path &base) {
    const auto project = base / "errors/project";
    const auto agents = project / ".lingtai";
    write_manifest(agents, "alpha");
    write_manifest(agents, "blocked");
    write_manifest(agents, "broken");
    fs::create_directories(agents / "unproven");
    TestFilesystem filesystem;
    filesystem.read_overrides[agents / "blocked/.agent.json"] = {
        AgentManifestFileReadKind::candidate_unreadable, {},
        std::make_error_code(std::errc::permission_denied)};
    filesystem.read_overrides[agents / "broken/.agent.json"] = {
        AgentManifestFileReadKind::candidate_io_error, {},
        std::make_error_code(std::errc::io_error)};
    filesystem.read_overrides[agents / "unproven/.agent.json"] = {
        AgentManifestFileReadKind::candidate_unreadable, {},
        std::make_error_code(std::errc::permission_denied)};
    const auto report = scan(project, &filesystem);
    expect(keys(report) == std::vector<fs::path>({"alpha"}),
        "one child error does not suppress another agent");
    expect(has_scan_diagnostic(report,
            AgentManifestScanDiagnosticKind::child_unreadable,
            agents / "blocked",
            std::make_error_code(std::errc::permission_denied)),
        "child permission failure is independently diagnosed");
    expect(has_scan_diagnostic(report,
            AgentManifestScanDiagnosticKind::child_io_error,
            agents / "broken",
            std::make_error_code(std::errc::io_error)),
        "child IO failure is independently diagnosed");

    TestFilesystem root_failure;
    root_failure.listing_error = std::make_error_code(std::errc::io_error);
    const auto failed = scan(project, &root_failure);
    expect(failed.scan.state == AgentManifestScanState::root_io_error,
        "root enumeration error is typed");
    expect(failed.items.empty() && failed.scan_diagnostics.empty(),
        "root enumeration fails closed without a partial claim");
}

void test_repeated_scans_do_not_write(const fs::path &base) {
    const auto project = base / "no-write/project";
    const auto agents = project / ".lingtai";
    write_manifest(agents, "valid", R"({"state":"ignored","admin":true})");
    write_manifest(agents, "invalid", "{");
    fs::create_directories(agents / "no-manifest");
    write_file(base / "no-write/global/registry.json", "global-marker");
    const auto before = snapshot(base / "no-write");
    const auto first = scan(project);
    const auto second = scan(project);
    expect(keys(first) == keys(second), "repeated scans return stable membership");
    expect(snapshot(base / "no-write") == before,
        "repeated discovery preserves every path, type, and byte");

    const auto absent_project = base / "no-create/project";
    fs::create_directories(absent_project);
    write_file(base / "no-create/global/install.json", "global-marker");
    const auto absent_before = snapshot(base / "no-create");
    scan(absent_project);
    scan(absent_project);
    expect(snapshot(base / "no-create") == absent_before,
        "repeated missing-root discovery creates nothing");
    expect(!fs::exists(absent_project / ".lingtai"),
        "discovery never initializes .lingtai");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return (std::cerr << "expected one test-sandbox path\n", 2);
    const auto requested_base = fs::path(argv[1]); std::error_code error;
    fs::remove_all(requested_base, error);
    fs::create_directories(requested_base, error);
    expect(!error, "test sandbox is created");
    const auto base = fs::canonical(requested_base, error);
    expect(!error, "test sandbox has a canonical path");
    test_root_contract(base);
    test_immediate_sorted_membership(base);
    test_manifest_tristate_and_races(base);
    test_independent_errors_and_fail_closed(base);
    test_repeated_scans_do_not_write(base);
    fs::remove_all(base, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " agent discovery assertion(s) failed\n";
        return 1;
    }
    std::cout << "AGENT_MANIFEST_DISCOVERY_CONTRACT_OK\n";
    return 0;
}
#else
#include <iostream>
int main() {
    std::cerr << "FAIL: agent manifest discovery behavior is unavailable\n";
    return 1;
}
#endif
