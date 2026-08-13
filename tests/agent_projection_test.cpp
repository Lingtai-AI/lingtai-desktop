#include "agent_projection.h"
#include "project_attachment.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace lingtai::desktop;

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void write_file(const fs::path &path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    expect(!error, "fixture parent is created");
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    expect(static_cast<bool>(stream), "fixture file is written");
}

std::map<std::string, std::string> snapshot_tree(const fs::path &root) {
    std::map<std::string, std::string> result;
    std::error_code error;
    for (fs::recursive_directory_iterator i(root, error), end;
            !error && i != end; i.increment(error)) {
        const auto key = i->path().lexically_relative(root).generic_string();
        const auto status = i->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) {
            result[key] = "link:" + fs::read_symlink(i->path()).generic_string();
        } else if (fs::is_directory(status)) {
            result[key] = "directory";
        } else if (fs::is_regular_file(status)) {
            std::ifstream stream(i->path(), std::ios::binary);
            result[key] = "file:" + std::string{
                std::istreambuf_iterator<char>(stream), {}};
        } else {
            result[key] = "other";
        }
    }
    return result;
}

double wall_now_seconds() {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// No injected clock exists in this collapsed composite API (the brief forbids
// a new test seam), so heartbeat boundary fixtures are written against real
// wall-clock time with a margin far larger than any plausible scan latency:
// clearly-alive and clearly-stale, not bit-exact age==5.0.
// `std::to_string(double)` is specified in terms of `sprintf`, which is
// sensitive to the active `LC_NUMERIC` locale; an explicit classic-locale
// stream keeps this fixture writer itself locale-independent, matching the
// production parser it is testing.
std::string heartbeat_seconds_ago(double seconds) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(6) << (wall_now_seconds() - seconds);
    return stream.str();
}

ProjectAttachmentResult make_project(const fs::path &base, std::string_view name) {
    const auto project = base / name;
    std::error_code error;
    fs::create_directories(project, error);
    expect(!error, "fixture project directory is created");
    auto attached = attach_project(project);
    expect(static_cast<bool>(attached), "fixture project attaches");
    return attached;
}

const AgentRow *find_row(const AgentSnapshot &snapshot, std::string_view key) {
    for (const auto &item : snapshot.items) {
        if (item.directory_key == fs::path(key)) return &item;
    }
    return nullptr;
}

// -- root and membership -----------------------------------------------

void test_root_and_membership(const fs::path &base) {
    {
        auto attached = make_project(base, "no-lingtai");
        if (!attached) return;
        const auto snapshot = project_agents(*attached.attachment);
        expect(snapshot.scan == AgentScanState::unavailable
                && snapshot.items.empty(),
            "a project with no .lingtai at all is a coarse unavailable scan");
    }
    {
        auto attached = make_project(base, "empty-lingtai");
        if (!attached) return;
        fs::create_directories(attached.attachment->root() / ".lingtai");
        const auto snapshot = project_agents(*attached.attachment);
        expect(snapshot.scan == AgentScanState::complete && snapshot.items.empty(),
            "an empty but real .lingtai is a distinct complete-empty scan");
    }
    {
        auto attached = make_project(base, "unsafe-root");
        if (!attached) return;
        const auto outside = base / "unsafe-root-outside";
        fs::create_directories(outside);
        std::error_code error;
        fs::create_directory_symlink(
            outside, attached.attachment->root() / ".lingtai", error);
        expect(!error, "root symlink fixture is created");
        const auto snapshot = project_agents(*attached.attachment);
        expect(snapshot.scan == AgentScanState::unavailable && snapshot.items.empty(),
            "a .lingtai symlink is rejected without being followed");
    }
    {
        // Deterministic sorted membership: siblings written out of order, a
        // safe-leaf dedupe boundary, and one unsafe child directory symlink
        // that must stay invisible without hiding its healthy neighbors.
        auto attached = make_project(base, "membership");
        if (!attached) return;
        const auto root = attached.attachment->root();
        write_file(root / ".lingtai/zeta/.agent.json", R"({"admin":{}})");
        write_file(root / ".lingtai/alpha/.agent.json", R"({"admin":{}})");
        fs::create_directories(root / ".lingtai/no-manifest");
        const auto outside = base / "membership-outside";
        write_file(outside / ".agent.json", R"({"admin":{"manage":true}})");
        std::error_code error;
        fs::create_directory_symlink(
            outside, root / ".lingtai/unsafe-child", error);
        expect(!error, "unsafe child-directory symlink fixture is created");
        const auto outside_before = snapshot_tree(outside);

        const auto snapshot = project_agents(*attached.attachment);
        expect(snapshot.scan == AgentScanState::complete, "membership scan completes");
        auto keys = std::vector<std::string>();
        for (const auto &item : snapshot.items) keys.push_back(item.directory_key.string());
        expect(keys == std::vector<std::string>{"alpha", "zeta"},
            "rows are alphabetically sorted by directory key regardless of "
            "listing order, and an absent manifest or unsafe child directory "
            "creates no row without hiding its healthy siblings");
        expect(snapshot_tree(outside) == outside_before,
            "an unsafe child directory symlink is never read through");
    }
}

// -- role classification -------------------------------------------------

void test_role_matrix(const fs::path &base) {
    auto attached = make_project(base, "roles");
    if (!attached) return;
    const auto root = attached.attachment->root();
    write_file(root / ".lingtai/no-admin/.agent.json", R"({})");
    write_file(root / ".lingtai/null-admin/.agent.json", R"({"admin":null})");
    write_file(root / ".lingtai/main-admin/.agent.json",
        R"({"admin":{"manage":true,"other":false}})");
    write_file(root / ".lingtai/empty-admin/.agent.json", R"({"admin":{}})");
    write_file(root / ".lingtai/false-admin/.agent.json",
        R"({"admin":{"a":false,"b":false}})");
    // Only a top-level admin.* boolean promotes to main; a nested true does not.
    write_file(root / ".lingtai/nested-true-admin/.agent.json",
        R"({"admin":{"a":{"nested":true}}})");
    // admin must be an object; a bare boolean/number/array/string never promotes.
    write_file(root / ".lingtai/bool-admin/.agent.json", R"({"admin":true})");

    const auto snapshot = project_agents(*attached.attachment);
    const struct { const char *key; AgentRole role; } expectations[] = {
        {"no-admin", AgentRole::human},
        {"null-admin", AgentRole::human},
        {"main-admin", AgentRole::main},
        {"empty-admin", AgentRole::agent},
        {"false-admin", AgentRole::agent},
        {"nested-true-admin", AgentRole::agent},
        {"bool-admin", AgentRole::agent},
    };
    for (const auto &expectation : expectations) {
        const auto *row = find_row(snapshot, expectation.key);
        expect(row && row->manifest_kind == AgentManifestKind::valid
                && row->role == expectation.role,
            std::string(expectation.key) + " classifies to the expected role");
    }
}

// -- manifest boundary: valid/malformed/oversize/nonregular/symlink ------

void test_manifest_boundary(const fs::path &base) {
    auto attached = make_project(base, "manifest-boundary");
    if (!attached) return;
    const auto root = attached.attachment->root();
    constexpr auto limit = 1024U * 1024U;
    write_file(root / ".lingtai/at-limit/.agent.json",
        std::string("{\"admin\":{},\"nickname\":\"")
            + std::string(limit - 26, 'x') + "\"}");
    write_file(root / ".lingtai/over-limit/.agent.json",
        std::string("{\"admin\":{},\"nickname\":\"")
            + std::string(limit - 25, 'x') + "\"}");
    write_file(root / ".lingtai/invalid-json/.agent.json", "{");
    write_file(root / ".lingtai/not-object/.agent.json", "[]");
    fs::create_directories(root / ".lingtai/nonregular/.agent.json");
    const auto outside = base / "manifest-boundary-outside";
    write_file(outside / "manifest.json", R"({"admin":{"manage":true}})");
    std::error_code error;
    fs::create_directories(root / ".lingtai/unsafe", error);
    expect(!error, "unsafe manifest directory fixture is created");
    fs::create_symlink(
        outside / "manifest.json", root / ".lingtai/unsafe/.agent.json", error);
    expect(!error, "manifest symlink fixture is created");
    const auto outside_before = snapshot_tree(outside);

    const auto snapshot = project_agents(*attached.attachment);

    const auto *at_limit = find_row(snapshot, "at-limit");
    expect(at_limit && at_limit->manifest_kind == AgentManifestKind::valid,
        "a manifest at exactly the 1 MiB limit is read and parsed");
    const auto *over_limit = find_row(snapshot, "over-limit");
    expect(over_limit && over_limit->manifest_kind == AgentManifestKind::malformed,
        "a manifest one byte over the 1 MiB limit is malformed, not read");
    const auto *invalid_json = find_row(snapshot, "invalid-json");
    expect(invalid_json && invalid_json->manifest_kind == AgentManifestKind::malformed
            && invalid_json->manifest_diagnostic
                == AgentManifestDiagnosticKind::invalid_json,
        "invalid JSON is malformed with a typed invalid_json diagnostic");
    const auto *not_object = find_row(snapshot, "not-object");
    expect(not_object && not_object->manifest_kind == AgentManifestKind::malformed
            && not_object->manifest_diagnostic
                == AgentManifestDiagnosticKind::not_object,
        "a JSON array root is malformed with a typed not_object diagnostic");
    const auto *nonregular = find_row(snapshot, "nonregular");
    expect(nonregular && nonregular->manifest_kind == AgentManifestKind::malformed,
        "a directory in place of the manifest file is malformed, not a crash");
    const auto *unsafe = find_row(snapshot, "unsafe");
    expect(unsafe && unsafe->manifest_kind == AgentManifestKind::unsafe
            && unsafe->manifest_diagnostic
                == AgentManifestDiagnosticKind::unsafe_symlink,
        "a manifest file symlink is a visible unsafe row, never followed");
    expect(snapshot_tree(outside) == outside_before,
        "an unsafe manifest symlink is never read through");
}

// -- heartbeat presence: boundary, format, short-circuit ------------------

void test_presence_matrix(const fs::path &base) {
    auto attached = make_project(base, "presence");
    if (!attached) return;
    const auto root = attached.attachment->root();
    const auto agent = [&](std::string_view key) {
        write_file(root / ".lingtai" / key / ".agent.json", R"({"admin":{}})");
        return root / ".lingtai" / key;
    };
    // Pinned to within 0.5s of the real five-second threshold on either
    // side -- close enough to actually probe the boundary, wide enough that
    // real scan/process latency (well under 500ms here) cannot flip it.
    write_file(agent("clearly-alive") / ".agent.heartbeat",
        heartbeat_seconds_ago(4.5));
    write_file(agent("clearly-stale") / ".agent.heartbeat",
        heartbeat_seconds_ago(5.5));
    // No heartbeat file at all.
    agent("missing-heartbeat");
    write_file(agent("future") / ".agent.heartbeat", heartbeat_seconds_ago(-100.0));
    write_file(agent("nonnumeric") / ".agent.heartbeat", "soon");
    const auto outside = base / "presence-outside";
    write_file(outside / "heartbeat", heartbeat_seconds_ago(1.0));
    std::error_code error;
    fs::create_symlink(
        outside / "heartbeat", agent("symlinked") / ".agent.heartbeat", error);
    expect(!error, "heartbeat symlink fixture is created");
    // A malformed manifest and an unsafe manifest must never attempt a
    // heartbeat read, no matter what value it would otherwise carry.
    write_file(root / ".lingtai/malformed-manifest/.agent.json", "{");
    write_file(root / ".lingtai/malformed-manifest/.agent.heartbeat",
        heartbeat_seconds_ago(1.0));
    write_file(root / ".lingtai/human/.agent.json", R"({"admin":null})");
    write_file(root / ".lingtai/human/.agent.heartbeat", heartbeat_seconds_ago(1.0));

    const auto snapshot = project_agents(*attached.attachment);
    const struct { const char *key; AgentPresenceKind presence; } expectations[] = {
        {"clearly-alive", AgentPresenceKind::alive},
        {"clearly-stale", AgentPresenceKind::stale},
        {"missing-heartbeat", AgentPresenceKind::missing},
        {"future", AgentPresenceKind::invalid},
        {"nonnumeric", AgentPresenceKind::invalid},
        {"symlinked", AgentPresenceKind::unavailable},
        {"malformed-manifest", AgentPresenceKind::unknown},
        {"human", AgentPresenceKind::alive_human},
    };
    for (const auto &expectation : expectations) {
        const auto *row = find_row(snapshot, expectation.key);
        expect(row && row->presence == expectation.presence,
            std::string(expectation.key) + " has the expected presence");
    }
    const auto *alive = find_row(snapshot, "clearly-alive");
    expect(alive && alive->heartbeat_age_seconds
            && *alive->heartbeat_age_seconds >= 0.0,
        "a live heartbeat exposes a non-negative age");
    const auto *symlinked_agent = find_row(snapshot, "symlinked");
    expect(symlinked_agent && !symlinked_agent->heartbeat_age_seconds,
        "an unavailable heartbeat never exposes an age");
}

// -- status: absent/malformed/positive/context suppression ---------------

void test_status_matrix(const fs::path &base) {
    auto attached = make_project(base, "status");
    if (!attached) return;
    const auto root = attached.attachment->root();
    const auto agent = [&](std::string_view key) {
        write_file(root / ".lingtai" / key / ".agent.json", R"({"admin":{}})");
        return root / ".lingtai" / key;
    };
    agent("absent-status");
    write_file(agent("malformed-status") / ".status.json", "{");
    write_file(agent("positive") / ".status.json", R"({
        "runtime":{"state":"active","running":true,"pid":4321,
            "state_changed_at":180,"last_progress_at":190,
            "no_progress_seconds":10.5},
        "active_turn":{"kind":"tool_call","id":"call-7",
            "started_at":188,"elapsed_seconds":12},
        "tokens":{"context":{"system_tokens":10,"tools_tokens":20,
            "history_tokens":30,"total_tokens":60,"window_size":1000,
            "usage_pct":6.0,"fixed_tokens":30,"growing_tokens":30}}
    })");
    write_file(agent("zero-window") / ".status.json",
        R"({"tokens":{"context":{"total_tokens":55,"window_size":0}}})");
    write_file(agent("invalid-window") / ".status.json",
        R"({"tokens":{"context":{"total_tokens":55,"window_size":"invalid"}}})");
    constexpr auto oversize = 1024U * 1024U + 1U;
    write_file(agent("oversized-status") / ".status.json",
        std::string(oversize, '1'));
    const auto outside = base / "status-outside";
    write_file(outside / "status.json", R"({"runtime":{"state":"outside"}})");
    std::error_code error;
    fs::create_symlink(
        outside / "status.json", agent("symlinked-status") / ".status.json", error);
    expect(!error, "status symlink fixture is created");

    const auto snapshot = project_agents(*attached.attachment);

    const auto *absent = find_row(snapshot, "absent-status");
    expect(absent && !absent->status, "no status.json leaves status unset");
    const auto *malformed = find_row(snapshot, "malformed-status");
    expect(malformed && !malformed->status,
        "invalid JSON status leaves status unset rather than a partial guess");
    const auto *positive = find_row(snapshot, "positive");
    expect(positive && positive->status, "a valid status.json is projected");
    if (positive && positive->status) {
        const auto &status = *positive->status;
        expect(status.state == "active" && status.running == true
                && status.pid == 4321 && status.state_changed_at == 180.0
                && status.last_progress_at == 190.0
                && status.no_progress_seconds == 10.5,
            "runtime facts are exact");
        expect(status.active_turn
                && status.active_turn->kind == "tool_call"
                && status.active_turn->id == "call-7"
                && status.active_turn->started_at == 188.0
                && status.active_turn->elapsed_seconds == 12.0,
            "active turn facts are exact");
        expect(status.context && status.context->window_size == 1000
                && status.context->total_tokens == 60
                && status.context->usage_percent == 6.0,
            "a positive window projects context");
    }
    const auto *zero_window = find_row(snapshot, "zero-window");
    expect(zero_window && zero_window->status && !zero_window->status->context,
        "a zero window suppresses context without discarding the rest of status");
    const auto *invalid_window = find_row(snapshot, "invalid-window");
    expect(invalid_window && invalid_window->status
            && !invalid_window->status->context,
        "a wrong-typed window also suppresses context");
    const auto *oversized = find_row(snapshot, "oversized-status");
    expect(oversized && !oversized->status,
        "a status.json one byte over the 1 MiB limit is never read");
    const auto *symlinked_status = find_row(snapshot, "symlinked-status");
    expect(symlinked_status && !symlinked_status->status,
        "a status.json symlink is never followed");
}

// -- capabilities: intrinsics, dedupe, order; no raw evidence exposed ----

void test_capabilities(const fs::path &base) {
    auto attached = make_project(base, "capabilities");
    if (!attached) return;
    const auto root = attached.attachment->root();
    write_file(root / ".lingtai/agent/.agent.json", R"({
        "admin":{},
        "capabilities":["email",["custom",{}],"custom",42]
    })");
    const auto snapshot = project_agents(*attached.attachment);
    const auto *row = find_row(snapshot, "agent");
    expect(row && row->identity, "a valid manifest carries identity facts");
    if (row && row->identity) {
        expect(row->identity->capabilities.display_names
                == std::vector<std::string>{
                    "system", "soul", "email", "psyche", "custom"},
            "display names lead with intrinsics then de-duplicated manifest "
            "names, in manifest order, with no raw evidence list retained");
    }
}

// -- no-write proof --------------------------------------------------------

void test_read_only(const fs::path &base) {
    auto attached = make_project(base, "read-only");
    if (!attached) return;
    const auto root = attached.attachment->root();
    write_file(root / ".lingtai/agent/.agent.json", R"({"admin":{}})");
    write_file(root / ".lingtai/agent/.agent.heartbeat",
        heartbeat_seconds_ago(1.0));
    write_file(root / ".lingtai/agent/.status.json",
        R"({"runtime":{"state":"idle"}})");
    write_file(root / ".lingtai/broken/.agent.json", "{");
    const auto before = snapshot_tree(attached.attachment->root());
    static_cast<void>(project_agents(*attached.attachment));
    static_cast<void>(project_agents(*attached.attachment));
    expect(snapshot_tree(attached.attachment->root()) == before,
        "repeated projection never writes, truncates, or creates any path");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "expected one test-sandbox path\n";
        return 2;
    }
    const auto base = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base, error);
    expect(!error, "test sandbox is created");

    test_root_and_membership(base);
    test_role_matrix(base);
    test_manifest_boundary(base);
    test_presence_matrix(base);
    test_status_matrix(base);
    test_capabilities(base);
    test_read_only(base);

    fs::remove_all(base, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " agent projection assertion(s) failed\n";
        return 1;
    }
    std::cout << "AGENT_PROJECTION_CONTRACT_OK\n";
    return 0;
}
