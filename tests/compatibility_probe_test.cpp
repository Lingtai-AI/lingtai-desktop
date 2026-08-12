#if __has_include("compatibility_probe.h")
#ifdef QT_CORE_LIB
#error "Qt Core usage requirements must not propagate to this test consumer"
#endif
#include "compatibility_probe.h"
#include "project_attachment.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
namespace fs = std::filesystem;
using namespace lingtai::desktop;
using A = CapabilityAlias;
using D = CompatibilityDisposition;
using K = CompatibilityFindingKind;
namespace {
constexpr auto kReceipt = R"({"schema":"lingtai.tui.install/v1","schema_version":1,"install_method":"source","stamped_version":"v1","resolved_commit":"abc","kernel_source":"bundle","kernel_version":"0.1"})";
constexpr auto kInit = R"({"manifest":{"capabilities":{"shell":{"enabled":true}}}})";
constexpr auto kResolved = R"({"schema":"lingtai.manifest.resolved/v1","schema_version":1,"source":"kernel","manifest":{"llm":{}}})";
int failures = 0;
struct JsonCase { const char *name; const char *json; K finding; };
void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
void write_file(const fs::path &path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    expect(stream.good(), "fixture file is written: " + path.string());
}
std::string read_file(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
bool has(const CompatibilityReport &report, CompatibilityFindingKind kind) {
    return std::ranges::any_of(report.findings, [=](const auto &finding) {
        return finding.kind == kind;
    });
}
void expect_findings(
        const CompatibilityReport &report,
        std::initializer_list<CompatibilityFindingKind> expected,
        const std::string &label) {
    expect(
        report.findings.size() == expected.size()
            && std::ranges::all_of(expected, [&](const auto kind) {
                return has(report, kind);
            }),
        label + " has exactly its intended findings");
}
struct Fixture {
    fs::path root, project, agent, receipt;
    void write_fresh_config(
            std::string_view init_contents,
            std::string_view resolved_contents = kResolved) const {
        const auto init_path = agent / "init.json";
        const auto resolved_path = agent / "system/manifest.resolved.json";
        write_file(init_path, init_contents);
        write_file(resolved_path, resolved_contents);
        const auto resolved_time = fs::file_time_type::clock::now();
        std::error_code init_error;
        std::error_code resolved_error;
        fs::last_write_time(
            init_path, resolved_time - std::chrono::seconds(2), init_error);
        fs::last_write_time(resolved_path, resolved_time, resolved_error);
        expect(!init_error && !resolved_error,
            "fixture config timestamps are set");
        if (!init_error && !resolved_error) {
            const auto init_time = fs::last_write_time(init_path, init_error);
            const auto actual_resolved_time =
                fs::last_write_time(resolved_path, resolved_error);
            expect(
                !init_error && !resolved_error
                    && actual_resolved_time > init_time,
                "fixture resolved manifest is strictly newer than init");
        }
    }
    Fixture(const fs::path &base, std::string_view name)
    : root(base / name), project(root / "project"),
      agent(project / "agent"), receipt(root / "global" / "install.json") {
        write_fresh_config(kInit);
        write_file(receipt, kReceipt);
    }
    CompatibilityReport run(
            std::optional<fs::path> requested = fs::path("agent")) const {
        const auto attached = attach_project(project);
        expect(static_cast<bool>(attached), "fixture project attaches");
        if (!attached) return {};
        static_assert(noexcept(probe_compatibility(
            *attached.attachment, requested, receipt)));
        return probe_compatibility(*attached.attachment, requested, receipt);
    }
};
void expect_disabled(
        const CompatibilityReport &report, CompatibilityDisposition disposition,
        CompatibilityFindingKind finding,
        const std::string &label) {
    expect(report.disposition == disposition, label + " disposition");
    expect(!report.commands_allowed, label + " disables commands");
    expect(has(report, finding), label + " retains its typed finding");
}
std::map<std::string, std::string> snapshot(const fs::path &root) {
    std::map<std::string, std::string> result;
    if (!fs::exists(root)) return result;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        const auto key = entry.path().lexically_relative(root).generic_string();
        if (entry.is_directory()) {
            result[key] = "directory";
        } else if (entry.is_regular_file()) {
            result[key] = "file:" + read_file(entry.path());
        } else if (entry.is_symlink()) {
            result[key] = "symlink:" + fs::read_symlink(entry.path()).string();
        }
    }
    return result;
}
void test_compatible(const fs::path &base) {
    Fixture fixture(base, "compatible");
    const auto report = fixture.run();
    expect(report.disposition == D::compatible, "recognized inputs are Compatible");
    expect(report.commands_allowed, "recognized fresh inputs allow commands");
    expect(report.findings.empty(), "compatible inputs have no findings");
    expect(report.install_receipt.has_value(), "recognized receipt is inspectable");
    expect(
        report.install_receipt
            && report.install_receipt->install_method == "source"
            && report.install_receipt->stamped_version == "v1"
            && report.install_receipt->resolved_commit == "abc"
            && report.install_receipt->kernel_source == "bundle"
            && report.install_receipt->kernel_version == "0.1",
        "selected receipt strings are exposed without executable verification");
    expect(report.resolved_manifest.has_value(), "fresh resolved manifest is effective");
    expect(
        report.raw_init
            && report.raw_init->capability_alias == A::shell_only,
        "raw init remains observable without claiming full validation");
}
void test_receipt_findings(const fs::path &base) {
    const std::vector<JsonCase> cases = {
        {"missing", nullptr, K::receipt_missing},
        {"malformed", "{", K::receipt_malformed},
        {"wrong-schema", R"({"schema":"other","schema_version":1})",
         K::receipt_schema_unsupported},
        {"wrong-version", R"({"schema":"lingtai.tui.install/v1","schema_version":2})",
         K::receipt_version_unsupported},
        {"bool-version", R"({"schema":"lingtai.tui.install/v1","schema_version":true})",
         K::receipt_version_unsupported},
        {"fraction-version", R"({"schema":"lingtai.tui.install/v1","schema_version":1.5})",
         K::receipt_version_unsupported},
    };
    for (const auto &item : cases) {
        Fixture fixture(base, std::string("receipt-") + item.name);
        if (item.json) {
            write_file(fixture.receipt, item.json);
        } else {
            fs::remove(fixture.receipt);
        }
        const auto report = fixture.run();
        expect_disabled(report, D::degraded, item.finding, item.name);
        expect(!report.install_receipt, std::string(item.name) + " is not recognized");
        expect(report.raw_init.has_value(), std::string(item.name) + " keeps raw init");
        expect(
            report.resolved_manifest.has_value(),
            std::string(item.name) + " keeps resolved facts");
    }
    Fixture unreadable(base, "receipt-unreadable");
    fs::remove(unreadable.receipt);
    fs::create_directory(unreadable.receipt);
    expect_disabled(
        unreadable.run(), D::degraded, K::receipt_unreadable,
        "unreadable receipt");
}
void test_resolved_findings(const fs::path &base) {
    const std::vector<JsonCase> cases = {
        {"missing", nullptr, K::resolved_missing},
        {"malformed", "{", K::resolved_malformed},
        {"nonobject", "[]", K::resolved_malformed},
        {"wrong-schema", R"({"schema":"other","schema_version":1,"source":"kernel","manifest":{}})",
         K::resolved_schema_unsupported},
        {"wrong-version", R"({"schema":"lingtai.manifest.resolved/v1","schema_version":2,"source":"kernel","manifest":{}})",
         K::resolved_version_unsupported},
        {"bool-version", R"({"schema":"lingtai.manifest.resolved/v1","schema_version":true,"source":"kernel","manifest":{}})",
         K::resolved_version_unsupported},
        {"fraction-version", R"({"schema":"lingtai.manifest.resolved/v1","schema_version":1.5,"source":"kernel","manifest":{}})",
         K::resolved_version_unsupported},
        {"wrong-source", R"({"schema":"lingtai.manifest.resolved/v1","schema_version":1,"source":"desktop","manifest":{}})",
         K::resolved_source_unsupported},
        {"bad-manifest", R"({"schema":"lingtai.manifest.resolved/v1","schema_version":1,"source":"kernel","manifest":[]})",
         K::resolved_manifest_invalid},
    };
    for (const auto &item : cases) {
        Fixture fixture(base, std::string("resolved-") + item.name);
        const auto path = fixture.agent / "system/manifest.resolved.json";
        if (item.json) {
            write_file(path, item.json);
        } else {
            fs::remove(path);
        }
        const auto report = fixture.run();
        expect_disabled(report, D::degraded, item.finding, item.name);
        expect(!report.resolved_manifest, std::string(item.name) + " is not effective");
        expect(report.raw_init.has_value(), std::string(item.name) + " keeps raw init");
    }
    Fixture stale(base, "resolved-stale");
    std::error_code error;
    const auto init_time = fs::file_time_type::clock::now();
    fs::last_write_time(stale.agent / "init.json", init_time, error);
    expect(!error, "stale fixture init timestamp is set");
    fs::last_write_time(
        stale.agent / "system/manifest.resolved.json",
        init_time - std::chrono::seconds(2),
        error);
    expect(!error, "stale fixture resolved timestamp is set");
    expect_disabled(
        stale.run(), D::degraded, K::resolved_stale,
        "stale resolved manifest");
    const auto stale_report = stale.run();
    expect(!stale_report.resolved_manifest && stale_report.raw_init,
        "stale resolved config is ineffective while raw init remains observable");

    Fixture unreadable(base, "resolved-unreadable");
    const auto path = unreadable.agent / "system/manifest.resolved.json";
    fs::remove(path); fs::create_directory(path);
    const auto report = unreadable.run();
    expect_disabled(report, D::degraded, K::resolved_unreadable, "unreadable resolved");
    expect(!report.resolved_manifest && report.raw_init, "unreadable resolved is ineffective");
}
void test_init_findings(const fs::path &base) {
    const std::vector<JsonCase> cases = {
        {"missing", nullptr, K::init_missing},
        {"malformed", "{", K::init_malformed},
        {"nonobject", "[]", K::init_not_object},
        {"missing-manifest", "{}", K::init_manifest_missing},
        {"bad-manifest", R"({"manifest":[]})", K::init_manifest_not_object},
    };
    for (const auto &item : cases) {
        Fixture fixture(base, std::string("init-") + item.name);
        const auto path = fixture.agent / "init.json";
        if (item.json) {
            fixture.write_fresh_config(item.json);
        } else {
            fs::remove(path);
        }
        const auto report = fixture.run();
        expect_disabled(report, D::blocked, item.finding, item.name);
        if (item.json) {
            expect_findings(report, {item.finding}, item.name);
            expect(report.resolved_manifest && report.resolved_manifest->fresh,
                std::string(item.name) + " keeps fresh resolved facts");
        } else {
            expect_findings(report,
                {item.finding, K::resolved_freshness_unavailable}, item.name);
            expect(!report.resolved_manifest,
                std::string(item.name) + " cannot establish freshness");
        }
    }
    Fixture unreadable(base, "init-unreadable");
    fs::remove(unreadable.agent / "init.json");
    fs::create_directory(unreadable.agent / "init.json");
    const auto report = unreadable.run();
    expect_disabled(report, D::blocked, K::init_unreadable, "unreadable init");
    expect_findings(report,
        {K::init_unreadable, K::resolved_freshness_unavailable},
        "unreadable init");
    expect(!report.raw_init && !report.resolved_manifest,
        "unreadable init cannot authorize resolved config");
}
void test_capability_aliases(const fs::path &base) {
    struct Case { const char *name; const char *json; A alias;
        D disposition; K finding; };
    const std::vector<Case> cases = {
        {"bash-only", R"({"manifest":{"capabilities":{"bash":{"x":1}}}})",
         A::bash_only, D::degraded, K::legacy_bash_alias},
        {"equal", R"({"manifest":{"capabilities":{"bash":{"x":1},"shell":{"x":1}}}})",
         A::equal, D::degraded, K::legacy_bash_alias},
        {"conflict", R"({"manifest":{"capabilities":{"bash":{"x":1},"shell":{"x":2}}}})",
         A::conflict, D::blocked, K::capability_conflict},
        {"shape", R"({"manifest":{"capabilities":true}})",
         A::shape_unknown, D::blocked, K::capabilities_shape_unknown},
    };
    for (const auto &item : cases) {
        Fixture fixture(base, std::string("capability-") + item.name);
        fixture.write_fresh_config(item.json);
        const auto report = fixture.run();
        expect_disabled(report, item.disposition, item.finding, item.name);
        expect_findings(report, {item.finding}, item.name);
        expect(
            report.install_receipt.has_value()
                && report.raw_init.has_value()
                && report.resolved_manifest.has_value()
                && report.resolved_manifest->fresh,
            std::string(item.name)
                + " has recognized receipt, raw init, and fresh resolved facts");
        expect(
            report.raw_init && report.raw_init->capability_alias == item.alias,
            std::string(item.name) + " is observed without rewriting raw init");
    }
}
void test_fail_closed_and_no_write(const fs::path &base) {
    Fixture no_agent(base, "no-agent");
    const auto no_agent_report = no_agent.run(std::nullopt);
    expect_disabled(no_agent_report, D::degraded, K::no_agent_requested,
        "no requested agent");
    expect(no_agent_report.install_receipt.has_value(), "no-agent still inspects receipt");
    const auto attached = attach_project(no_agent.project);
    expect_disabled(probe_compatibility(*attached.attachment, fs::path("agent"),
        "relative/install.json"), D::degraded, K::receipt_unreadable, "relative receipt");
    const auto absent_agent = no_agent.run("absent");
    expect_disabled(absent_agent, D::blocked, K::agent_unavailable, "absent agent");
    expect(has(absent_agent, K::init_missing) && has(absent_agent, K::resolved_missing),
        "requested absent agent retains typed missing config findings");
    Fixture independent(base, "independent-findings");
    write_file(independent.receipt, R"({"schema":"other","schema_version":true})");
    independent.write_fresh_config(
        R"({"manifest":{"capabilities":{"bash":1,"shell":true}}})",
        R"({"schema":"other","schema_version":true,"source":"desktop","manifest":[]})");
    const auto combined = independent.run();
    expect_disabled(combined, D::blocked, K::capability_conflict,
        "independent findings");
    expect_findings(combined,
        {K::receipt_schema_unsupported, K::receipt_version_unsupported,
         K::resolved_schema_unsupported, K::resolved_version_unsupported,
         K::resolved_source_unsupported, K::resolved_manifest_invalid,
         K::capability_conflict},
        "independent findings");
    Fixture unchanged(base, "no-write");
    unchanged.write_fresh_config(
        R"({"manifest":{"capabilities":{"bash":{"x":1}}}})");
    const auto before = snapshot(unchanged.root);
    const auto first = unchanged.run();
    const auto second = unchanged.run();
    expect(has(first, K::legacy_bash_alias) && has(second, K::legacy_bash_alias),
        "repeated probe observes but never rewrites legacy alias");
    expect_findings(first, {K::legacy_bash_alias}, "first no-write probe");
    expect_findings(second, {K::legacy_bash_alias}, "second no-write probe");
    expect(
        first.install_receipt && first.raw_init && first.resolved_manifest
            && second.install_receipt && second.raw_init
            && second.resolved_manifest,
        "no-write probes retain recognized receipt, raw init, and resolved facts");
    expect(snapshot(unchanged.root) == before, "repeated probing preserves all bytes and paths");

    Fixture absent(base, "no-create");
    fs::remove(absent.receipt); fs::remove_all(absent.agent / "system");
    const auto absent_before = snapshot(absent.root);
    absent.run(); absent.run();
    expect(snapshot(absent.root) == absent_before, "repeated probe creates no missing paths");
    expect(!fs::exists(absent.receipt), "probe does not create a global receipt");
    expect(!fs::exists(absent.agent / "system"), "probe does not create agent system state");
    expect(!fs::exists(unchanged.project / ".lingtai"), "probe does not initialize project state");
    expect(!fs::exists(unchanged.root / "global/registry.json"), "probe does not create a registry");
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 2) return (std::cerr << "expected one test-sandbox path\n", 2);
    const auto base = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base, error);
    expect(!error, "test sandbox is created");
    test_compatible(base);
    test_receipt_findings(base);
    test_resolved_findings(base);
    test_init_findings(base);
    test_capability_aliases(base);
    test_fail_closed_and_no_write(base);
    fs::remove_all(base, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " compatibility assertion(s) failed\n";
        return 1;
    }
    std::cout << "COMPATIBILITY_PROBE_CONTRACT_OK\n";
    return 0;
}
#else
#include <iostream>
int main() {
    std::cerr << "FAIL: compatibility probe behavior is unavailable\n";
    return 1;
}
#endif
