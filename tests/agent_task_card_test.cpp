#include "agent_task_card.h"
#include "project_attachment.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::AgentTaskCardState;
using lingtai::desktop::ProjectAttachment;
using lingtai::desktop::attach_project;
using lingtai::desktop::read_agent_task_card;

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

// Exact byte/type image of the fixture, so "reading never writes" is proven
// against the real tree rather than asserted.
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

ProjectAttachment attach(const fs::path &project) {
    auto attached = attach_project(project);
    require(static_cast<bool>(attached),
        "project attachment must succeed: " + project.string());
    return std::move(*attached.attachment);
}

fs::path taskcard_dir(const fs::path &project, std::string_view key) {
    return project / ".lingtai" / std::string(key) / "taskcard";
}

// Retained evidence ceiling for this reader: exact active-body projection,
// exact inactive exclusion bound to its own selected key, and one
// outside/unsafe non-display proof (a symlinked `taskcard` component must
// not be followed). Reading must never write the project.
void verify_active_inactive_and_containment(const fs::path &sandbox) {
    const auto project = sandbox / "project";
    write_file(taskcard_dir(project, "agent-a") / "status", "active");
    write_file(taskcard_dir(project, "agent-a") / "taskcard.md", "TASK_A_BODY");
    write_file(taskcard_dir(project, "agent-b") / "status", "inactive");

    const auto attachment = attach(project);
    const auto project_before = tree_snapshot(project);

    const auto a = read_agent_task_card(attachment, "agent-a");
    require(tree_snapshot(project) == project_before,
        "reading an active Task Card must never write the project");
    require(a.state == AgentTaskCardState::active,
        "an exact `active` status with a nonblank bounded body must "
        "project active");
    require(a.body == "TASK_A_BODY",
        "the active body must render unchanged, bound to Agent A's own key");

    const auto b = read_agent_task_card(attachment, "agent-b");
    require(tree_snapshot(project) == project_before,
        "reading an inactive Task Card must never write the project");
    require(b.state == AgentTaskCardState::inactive,
        "an exact `inactive` status must project inactive regardless of "
        "any other Agent's active body");
    require(b.body.empty(), "an inactive projection must carry no body");

    // Outside/unsafe non-display proof: an intermediate `taskcard` symlink
    // must not be followed to an outside body, and nothing may be written.
    const auto outside = sandbox / "project-outside";
    write_file(outside / "status", "active");
    write_file(outside / "taskcard.md", "OUTSIDE_SHOULD_NOT_APPEAR");
    std::error_code mkdir_error;
    fs::create_directories(project / ".lingtai" / "agent-c", mkdir_error);
    require(!mkdir_error, "agent-c directory must be created");
    std::error_code link_error;
    fs::create_directory_symlink(
        outside, project / ".lingtai" / "agent-c" / "taskcard", link_error);
    require(!link_error, "symlinked taskcard fixture must be created");
    const auto outside_before = tree_snapshot(outside);

    const auto c = read_agent_task_card(attachment, "agent-c");
    require(tree_snapshot(outside) == outside_before,
        "reading through a blocked intermediate symlink must never write "
        "outside state");
    require(c.state == AgentTaskCardState::unavailable,
        "a symlinked `taskcard` component must reduce to unavailable, not "
        "be followed");
    require(c.body.empty(), "no outside body may ever be projected");
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

        verify_active_inactive_and_containment(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "agent_task_card: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "agent_task_card: all checks passed\n";
    return 0;
}
