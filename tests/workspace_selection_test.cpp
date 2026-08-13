#ifdef QT_CORE_LIB
#error "Qt Core usage requirements must not propagate to this test consumer"
#endif
#if __has_include("workspace_selection.h")
#include "project_attachment.h"
#include "workspace_selection.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace lingtai::desktop;

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

ProjectAttachment accepted_project(const fs::path &root) {
    std::error_code error;
    fs::create_directories(root, error);
    expect(!error, "project fixture is created");
    auto result = attach_project(root);
    expect(static_cast<bool>(result), "project fixture attaches");
    return *std::move(result.attachment);
}

void write_file(const fs::path &path, const std::string &contents) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    expect(!error, "file fixture parent is created");
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    expect(stream.good(), "file fixture is written");
}

struct TreeEntry {
    fs::path relative_path;
    fs::file_type type = fs::file_type::none;
    std::string bytes;

    bool operator==(const TreeEntry &) const = default;
};

std::vector<TreeEntry> snapshot_tree(const fs::path &root) {
    std::vector<TreeEntry> result;
    std::error_code error;
    fs::recursive_directory_iterator iterator(root, error), end;
    expect(!error, "snapshot root is readable");
    while (!error && iterator != end) {
        const auto status = iterator->symlink_status(error);
        expect(!error, "snapshot entry type is readable");
        if (error) {
            break;
        }
        TreeEntry entry{
            .relative_path = iterator->path().lexically_relative(root),
            .type = status.type(),
            .bytes = {},
        };
        if (fs::is_regular_file(status)) {
            std::ifstream stream(iterator->path(), std::ios::binary);
            entry.bytes.assign(
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>());
            expect(stream.good() || stream.eof(), "snapshot file is readable");
        } else if (fs::is_symlink(status)) {
            entry.bytes = fs::read_symlink(iterator->path(), error).string();
            expect(!error, "snapshot symlink is readable");
        }
        result.push_back(std::move(entry));
        iterator.increment(error);
    }
    expect(!error, "snapshot enumeration completes");
    std::ranges::sort(result, {}, &TreeEntry::relative_path);
    return result;
}

void test_activation_owns_project(const fs::path &sandbox) {
    auto project = accepted_project(sandbox / "alpha");
    const auto root = project.root();
    WorkspaceSelectionState state;

    expect(!state.active_project(), "workspace starts closed");

    state.activate_project(std::move(project));

    expect(state.active_project().has_value(), "activation owns the project");
    expect(
        state.active_project() && state.active_project()->root() == root,
        "the active project retains its accepted canonical root");
}

void test_agent_selection_requires_an_active_project_and_safe_key(
        const fs::path &sandbox) {
    WorkspaceSelectionState state;
    expect(
        state.select_agent("agent-01")
            == AgentSelectionResult::no_active_project,
        "Agent selection is rejected without an active project");
    expect(
        state.select_agent("../unsafe")
            == AgentSelectionResult::no_active_project,
        "no active project takes precedence over key validation");
    expect(
        !state.selected_agent_directory_key(),
        "rejected selection does not create state");

    state.activate_project(accepted_project(sandbox / "selection-project"));
    const std::vector<fs::path> unsafe_keys = {
        fs::path(),
        ".",
        "..",
        fs::path("nested") / "agent",
        fs::path("/absolute-agent"),
        fs::path(std::string("nul\0agent", 9)),
    };
    for (const auto &key : unsafe_keys) {
        expect(
            state.select_agent(key)
                == AgentSelectionResult::invalid_directory_key,
            "unsafe Agent directory key is rejected");
        expect(
            !state.selected_agent_directory_key(),
            "unsafe Agent directory key is not stored");
    }

    expect(
        state.select_agent("agent-01") == AgentSelectionResult::selected,
        "a safe Agent directory key is selected");
    expect(
        state.selected_agent_directory_key()
            == std::optional<fs::path>("agent-01"),
        "selection stores the directory key without treating it as identity");

    expect(
        state.select_agent("other/unsafe")
            == AgentSelectionResult::invalid_directory_key,
        "an unsafe replacement selection is rejected");
    expect(
        state.selected_agent_directory_key()
            == std::optional<fs::path>("agent-01"),
        "a rejected replacement preserves the accepted selection");
}

void test_project_activation_preserves_or_clears_agent_by_root(
        const fs::path &sandbox) {
    auto alpha = accepted_project(sandbox / "transition-alpha");
    auto beta = accepted_project(sandbox / "transition-beta");
    WorkspaceSelectionState state;

    state.activate_project(alpha);
    expect(
        state.select_agent("alpha-agent") == AgentSelectionResult::selected,
        "the first project's Agent is selected");
    state.activate_project(alpha);
    expect(
        state.selected_agent_directory_key()
            == std::optional<fs::path>("alpha-agent"),
        "reactivating the same canonical root preserves Agent selection");

    state.activate_project(beta);
    expect(
        !state.selected_agent_directory_key(),
        "activating a different canonical root clears Agent selection");
}

void test_clear_agent_selection_is_idempotent(const fs::path &sandbox) {
    WorkspaceSelectionState state;
    state.activate_project(accepted_project(sandbox / "clear-project"));
    expect(
        state.select_agent("clear-agent") == AgentSelectionResult::selected,
        "clear fixture has an Agent selection");
    const auto active_root = state.active_project()->root();

    state.clear_agent_selection();
    expect(
        !state.selected_agent_directory_key(),
        "explicit clear removes Agent selection");
    state.clear_agent_selection();
    expect(
        !state.selected_agent_directory_key(),
        "clearing an empty Agent selection is idempotent");
    expect(
        state.active_project()
            && state.active_project()->root() == active_root,
        "clearing Agent selection retains the active project");
}

void test_deleted_active_root_does_not_change_model_state(
        const fs::path &sandbox) {
    auto project = accepted_project(sandbox / "deleted-project");
    const auto root = project.root();
    WorkspaceSelectionState state;
    state.activate_project(project);
    expect(
        state.select_agent("before-delete") == AgentSelectionResult::selected,
        "deleted-root fixture has an Agent selection");

    std::error_code error;
    fs::remove_all(root, error);
    expect(!error && !fs::exists(root), "active fixture is deleted");
    state.activate_project(project);

    expect(
        state.active_project() && state.active_project()->root() == root,
        "same accepted attachment stays active after its root is deleted");
    expect(
        state.selected_agent_directory_key()
            == std::optional<fs::path>("before-delete"),
        "same-root activation after deletion preserves Agent selection");
    expect(
        state.select_agent("after-delete") == AgentSelectionResult::selected,
        "Agent-key selection does not test deleted-root availability");
    expect(
        !fs::exists(root),
        "workspace transitions do not recreate a deleted project root");
}

void test_workspace_transitions_write_nothing(const fs::path &sandbox) {
    const auto root = sandbox / "no-write-project";
    auto project = accepted_project(root);
    write_file(root / ".lingtai" / "agent-a" / ".agent.json", "{}");
    write_file(root / ".lingtai" / "agent-a" / ".agent.heartbeat", "100");
    write_file(root / ".lingtai" / "registry.json", "registry-before");
    write_file(sandbox / "desktop-settings.json", "settings-before");
    const auto before = snapshot_tree(sandbox);

    WorkspaceSelectionState state;
    state.activate_project(project);
    expect(
        state.select_agent("agent-a") == AgentSelectionResult::selected,
        "no-write fixture selects a safe Agent key");
    state.clear_agent_selection();
    expect(
        state.select_agent("agent-a") == AgentSelectionResult::selected,
        "no-write fixture reselects without touching the tree");

    expect(
        snapshot_tree(sandbox) == before,
        "workspace transitions do not write project or registry state");
}

} // namespace

int main(int argc, char **argv) {
    static_assert(noexcept(WorkspaceSelectionState()));
    static_assert(std::is_same_v<
        decltype(std::declval<WorkspaceSelectionState &>()
            .active_project()),
        const std::optional<ProjectAttachment> &>);
    static_assert(std::is_same_v<
        decltype(std::declval<WorkspaceSelectionState &>()
            .selected_agent_directory_key()),
        const std::optional<fs::path> &>);
    if (argc != 2) {
        std::cerr << "expected one test-sandbox path\n";
        return 2;
    }
    const auto sandbox = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(sandbox, error);
    fs::create_directories(sandbox, error);
    expect(!error, "test sandbox is created");

    test_activation_owns_project(sandbox);
    test_agent_selection_requires_an_active_project_and_safe_key(sandbox);
    test_project_activation_preserves_or_clears_agent_by_root(sandbox);
    test_clear_agent_selection_is_idempotent(sandbox);
    test_deleted_active_root_does_not_change_model_state(sandbox);
    test_workspace_transitions_write_nothing(sandbox);

    fs::remove_all(sandbox, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " workspace selection assertion(s) failed\n";
        return 1;
    }
    std::cout << "WORKSPACE_SELECTION_CONTRACT_OK\n";
    return 0;
}
#else
#include <iostream>
int main() {
    std::cerr << "FAIL: workspace selection behavior is unavailable\n";
    return 1;
}
#endif
