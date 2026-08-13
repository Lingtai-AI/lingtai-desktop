#include "agent_sleep.h"
#include "project_attachment.h"

#include <cstddef>
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
using lingtai::desktop::AgentSleepRequestResult;
using lingtai::desktop::ProjectAttachment;
using lingtai::desktop::attach_project;
using lingtai::desktop::capture_agent_sleep_event_baseline;
using lingtai::desktop::observe_agent_sleep_received;
using lingtai::desktop::request_agent_sleep;

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

void append_file(const fs::path &path, std::string_view bytes) {
    auto stream = std::ofstream(path, std::ios::binary | std::ios::app);
    stream << bytes;
    require(stream.good(), "fixture must be appended: " + path.string());
}

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// Exact byte/type image of the fixture, so a refused/blocked write is proven
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

fs::path sleep_marker_path(const fs::path &project, std::string_view key) {
    return project / ".lingtai" / std::string(key) / ".sleep";
}

fs::path events_path(const fs::path &project, std::string_view key) {
    return project / ".lingtai" / std::string(key) / "logs" / "events.jsonl";
}

ProjectAttachment attach(const fs::path &project) {
    auto attached = attach_project(project);
    require(static_cast<bool>(attached),
        "project attachment must succeed: " + project.string());
    return std::move(*attached.attachment);
}

// Unique risk 1: the write must land on exactly the selected target, never a
// sibling, and every other fixture byte -- including a sibling's own bytes --
// must stay untouched. Overwrite must truncate rather than refuse, and a
// selected key with no existing directory must fail rather than create one.
void verify_write_targets_exact_selected_agent(const fs::path &sandbox) {
    const auto project = sandbox / "targeting";
    write_file(project / ".lingtai/agent-a/.agent.json", R"({"admin":{}})");
    write_file(project / ".lingtai/agent-b/.agent.json", R"({"admin":{}})");
    write_file(events_path(project, "agent-a"), "unrelated a bytes\n");

    const auto attachment = attach(project);
    const auto before_a = tree_snapshot(project / ".lingtai/agent-a");
    const auto result = request_agent_sleep(attachment, "agent-b");
    require(result == AgentSleepRequestResult::requested,
        "a request against an existing plain target directory must succeed");
    require(tree_snapshot(project / ".lingtai/agent-a") == before_a,
        "the unselected sibling must remain byte-for-byte unchanged");
    require(fs::exists(sleep_marker_path(project, "agent-b")),
        "the selected target must gain its own .sleep marker");
    require(!fs::exists(sleep_marker_path(project, "agent-a")),
        "the unselected sibling must never gain a .sleep marker");
    require(read_file(sleep_marker_path(project, "agent-b")).empty(),
        "the written marker must be exactly zero bytes");

    write_file(sleep_marker_path(project, "agent-b"), "stale content");
    const auto second = request_agent_sleep(attachment, "agent-b");
    require(second == AgentSleepRequestResult::requested,
        "an existing marker must be overwritten, not refused");
    require(read_file(sleep_marker_path(project, "agent-b")).empty(),
        "overwriting an existing marker must truncate it to zero bytes");

    const auto missing_key_result =
        request_agent_sleep(attachment, "never-existed");
    require(missing_key_result == AgentSleepRequestResult::failed_local,
        "a selected key with no existing directory must fail rather than "
        "create one");
    require(!fs::exists(project / ".lingtai/never-existed"),
        "a failed request must never create the selected key's directory");
}

// Unique risk 2: false application attribution. A pre-baseline
// sleep_received must never satisfy observation, and completing a
// pre-baseline partial tail line after the baseline must not be attributed
// either; only one exact complete post-baseline event may be observed.
void verify_baseline_excludes_preexisting_and_observes_post_baseline(
        const fs::path &sandbox) {
    const auto project = sandbox / "baseline";
    write_file(project / ".lingtai/agent/.agent.json", R"({"admin":{}})");
    const auto events = events_path(project, "agent");
    write_file(events,
        R"({"type":"sleep_received","source":"signal_file"})" "\n");

    const auto attachment = attach(project);
    const auto baseline =
        capture_agent_sleep_event_baseline(attachment, "agent");
    require(baseline.available, "an existing readable log must be available");
    require(baseline.ends_with_newline,
        "a log ending exactly on a complete line must record a true tail-LF "
        "bit");
    require(!observe_agent_sleep_received(attachment, "agent", baseline),
        "a sleep_received row written before the baseline must never be "
        "attributed to this request");

    append_file(events,
        R"({"type":"sleep_received","source":"signal_file"})" "\n");
    require(observe_agent_sleep_received(attachment, "agent", baseline),
        "one exact complete post-baseline sleep_received row must be "
        "observed");

    const auto project2 = sandbox / "partial-tail";
    write_file(project2 / ".lingtai/agent/.agent.json", R"({"admin":{}})");
    const auto events2 = events_path(project2, "agent");
    write_file(events2, R"({"type":"sleep_received","source":"signal_fil)");
    const auto attachment2 = attach(project2);
    const auto baseline2 =
        capture_agent_sleep_event_baseline(attachment2, "agent");
    require(baseline2.available,
        "a log with a trailing partial line is still available for "
        "baseline capture");
    require(!baseline2.ends_with_newline,
        "a log ending mid-line must record a false tail-LF bit");
    append_file(events2, "e\"}\n");
    require(!observe_agent_sleep_received(attachment2, "agent", baseline2),
        "completing a pre-baseline partial tail line after the baseline "
        "must never be attributed to this request");

    append_file(events2,
        R"({"type":"sleep_received","source":"signal_file"})" "\n");
    require(observe_agent_sleep_received(attachment2, "agent", baseline2),
        "a real post-baseline row must still be observed after an "
        "attributed-but-discarded completed partial tail");
}

// Unique risk 3: containment. An intermediate symlink at the selected key,
// or a final `.sleep` that is itself a symlink or another non-regular type,
// must never redirect the write outside the accepted project and must
// report no local success.
void verify_symlink_and_nonregular_containment(const fs::path &sandbox) {
    const auto project = sandbox / "containment";
    write_file(project / ".lingtai/agent/.agent.json", R"({"admin":{}})");

    const auto outside = sandbox / "containment-outside";
    write_file(outside / "escaped.sleep", "");
    std::error_code link_error;
    fs::create_symlink(outside / "escaped.sleep",
        project / ".lingtai/agent/.sleep", link_error);
    require(!link_error, "the symlinked .sleep fixture must be created");

    const auto attachment = attach(project);
    const auto outside_before = tree_snapshot(outside);
    const auto symlinked_result = request_agent_sleep(attachment, "agent");
    require(symlinked_result == AgentSleepRequestResult::failed_local,
        "a symlinked .sleep leaf must never be followed or truncated");
    require(tree_snapshot(outside) == outside_before,
        "a blocked symlinked leaf must never write outside the project");
    require(fs::is_symlink(project / ".lingtai/agent/.sleep"),
        "a refused symlinked leaf must be left exactly as it was, not "
        "replaced");

    const auto project3 = sandbox / "containment-intermediate";
    write_file(project3 / ".lingtai/real-elsewhere/.agent.json",
        R"({"admin":{}})");
    const auto elsewhere = sandbox / "containment-elsewhere";
    fs::create_directories(elsewhere);
    std::error_code dir_link_error;
    fs::create_directory_symlink(elsewhere,
        project3 / ".lingtai/agent", dir_link_error);
    require(!dir_link_error,
        "the symlinked selected-key fixture must be created");
    const auto attachment3 = attach(project3);
    const auto elsewhere_before = tree_snapshot(elsewhere);
    const auto intermediate_result = request_agent_sleep(attachment3, "agent");
    require(intermediate_result == AgentSleepRequestResult::failed_local,
        "a symlinked selected-key directory must never be traversed");
    require(tree_snapshot(elsewhere) == elsewhere_before,
        "a blocked symlinked intermediate directory must never write "
        "outside the project");

    const auto project4 = sandbox / "containment-nonregular";
    fs::create_directories(project4 / ".lingtai/agent/.sleep");
    const auto attachment4 = attach(project4);
    const auto nonregular_result = request_agent_sleep(attachment4, "agent");
    require(nonregular_result == AgentSleepRequestResult::failed_local,
        "an existing non-regular .sleep target must be refused, not "
        "replaced");
    require(fs::is_directory(project4 / ".lingtai/agent/.sleep"),
        "a refused non-regular leaf must be left exactly as it was");
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

        verify_write_targets_exact_selected_agent(sandbox);
        verify_baseline_excludes_preexisting_and_observes_post_baseline(
            sandbox);
        verify_symlink_and_nonregular_containment(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "agent_sleep: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "agent_sleep: all checks passed\n";
    return 0;
}
