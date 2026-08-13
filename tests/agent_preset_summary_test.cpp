#include "agent_preset_summary.h"
#include "project_attachment.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::AgentPresetSummarySource;
using lingtai::desktop::ProjectAttachment;
using lingtai::desktop::attach_project;
using lingtai::desktop::read_agent_preset_summary;

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

constexpr std::string_view kResolvedArtifact = R"JSON({
  "schema": "lingtai.manifest.resolved/v1",
  "schema_version": 1,
  "source": "kernel",
  "generated_at": "2026-08-13T19:53:34Z",
  "manifest": {
    "llm": {"provider": "codex", "model": "gpt-5.6-sol"},
    "context_limit": 250000,
    "capabilities": {"web": {}, "avatar": {}, "shell": {}}
  },
  "preset": {
    "active": "~/.lingtai-tui/presets/saved/codex.json",
    "default": "~/.lingtai-tui/presets/saved/codex.json",
    "allowed": [
      "~/.lingtai-tui/presets/saved/deepseek_flash.json",
      "~/.lingtai-tui/presets/saved/codex.json",
      "~/.lingtai-tui/presets/saved/zhipu-1.json"
    ]
  }
})JSON";

// Retained evidence ceiling for this reader: one supported v1 artifact
// proving exact ordered allowed refs, independent active/default badges,
// and the narrow active-effective fields (bound to Agent A's own key, with
// no write); then one stale observation (an otherwise-supported artifact
// strictly older than the selected Agent's own `init.json` by file mtime,
// bound to Agent B); then one unsafe/unreadable observation (a symlinked
// `system` directory component must not be followed, bound to Agent C).
void verify_resolved_stale_and_unsafe(const fs::path &sandbox) {
    const auto project = sandbox / "project";
    write_file(project / ".lingtai/agent-a/system/manifest.resolved.json",
        kResolvedArtifact);
    write_file(project / ".lingtai/agent-a/init.json", "{}");

    const auto attachment = attach(project);
    const auto project_before = tree_snapshot(project);

    const auto a = read_agent_preset_summary(attachment, "agent-a");
    require(tree_snapshot(project) == project_before,
        "reading a resolved preset summary must never write the project");
    require(a.source == AgentPresetSummarySource::resolved,
        "a supported complete v1 artifact must project resolved");
    require(a.active_ref
            && *a.active_ref == "~/.lingtai-tui/presets/saved/codex.json",
        "the exact active ref string must be preserved");
    require(a.default_ref
            && *a.default_ref == "~/.lingtai-tui/presets/saved/codex.json",
        "the exact default ref string must be preserved");
    require(a.allowed.size() == 3, "all three allowed refs must be projected");
    require(a.allowed[0].ref == "~/.lingtai-tui/presets/saved/deepseek_flash.json"
            && !a.allowed[0].is_active && !a.allowed[0].is_default,
        "published order must be preserved and a non-active/default ref "
        "must carry no badge");
    require(a.allowed[1].ref == "~/.lingtai-tui/presets/saved/codex.json"
            && a.allowed[1].is_active && a.allowed[1].is_default,
        "a ref exactly equal to both active and default must carry both "
        "badges independently");
    require(a.allowed[2].ref == "~/.lingtai-tui/presets/saved/zhipu-1.json"
            && !a.allowed[2].is_active && !a.allowed[2].is_default,
        "a later allowed ref must keep its own independent badge state");
    require(a.effective.provider && *a.effective.provider == "codex",
        "the active effective provider must be projected");
    require(a.effective.model && *a.effective.model == "gpt-5.6-sol",
        "the active effective model must be projected");
    require(a.effective.context_limit && *a.effective.context_limit == 250000,
        "the active effective context limit must be projected");
    require(a.effective.capability_names
            == std::vector<std::string>({"avatar", "shell", "web"}),
        "capability names must be the top-level manifest.capabilities keys, "
        "sorted only for stable display");
    require(a.generated_at && *a.generated_at == "2026-08-13T19:53:34Z",
        "kernel generated_at provenance must be projected");

    // The kernel only writes effective `manifest.context_limit` when the
    // active preset itself declared one -- it is a genuinely optional
    // narrow field, not a completeness gate. An otherwise-supported v1
    // artifact that omits it must still project resolved.
    const auto artifact_a =
        project / ".lingtai/agent-a/system/manifest.resolved.json";
    write_file(artifact_a, R"JSON({
      "schema": "lingtai.manifest.resolved/v1",
      "schema_version": 1,
      "source": "kernel",
      "generated_at": "2026-08-13T19:53:34Z",
      "manifest": {
        "llm": {"provider": "codex", "model": "gpt-5.6-sol"},
        "capabilities": {"web": {}, "avatar": {}, "shell": {}}
      },
      "preset": {
        "active": "~/.lingtai-tui/presets/saved/codex.json",
        "default": "~/.lingtai-tui/presets/saved/codex.json",
        "allowed": [
          "~/.lingtai-tui/presets/saved/deepseek_flash.json",
          "~/.lingtai-tui/presets/saved/codex.json",
          "~/.lingtai-tui/presets/saved/zhipu-1.json"
        ]
      }
    })JSON");
    const auto a_no_context_limit = read_agent_preset_summary(attachment, "agent-a");
    require(a_no_context_limit.source == AgentPresetSummarySource::resolved,
        "a supported artifact that simply omits the optional "
        "manifest.context_limit must still project resolved, never "
        "unavailable");
    require(!a_no_context_limit.effective.context_limit,
        "a genuinely absent context limit must project as no value, not a "
        "fabricated one");
    require(a_no_context_limit.effective.provider
            && *a_no_context_limit.effective.provider == "codex",
        "the other active-effective fields must still project normally");

    const auto artifact_b =
        project / ".lingtai/agent-b/system/manifest.resolved.json";
    write_file(artifact_b, kResolvedArtifact);
    const auto init_b = project / ".lingtai/agent-b/init.json";
    write_file(init_b, "{}");
    std::error_code time_error;
    const auto older_time = fs::last_write_time(init_b, time_error)
        - std::chrono::seconds(10);
    require(!time_error, "init.json mtime must be readable");
    fs::last_write_time(artifact_b, older_time, time_error);
    require(!time_error, "artifact mtime must be adjustable");

    const auto stale_before = tree_snapshot(project);
    const auto b = read_agent_preset_summary(attachment, "agent-b");
    require(tree_snapshot(project) == stale_before,
        "reading a stale preset summary must never write the project");
    require(b.source == AgentPresetSummarySource::stale,
        "a supported artifact strictly older than the selected Agent's own "
        "init.json mtime must project stale, never resolved");

    const auto outside = sandbox / "project-outside";
    write_file(outside / "manifest.resolved.json", kResolvedArtifact);
    std::error_code mkdir_error;
    fs::create_directories(project / ".lingtai" / "agent-c", mkdir_error);
    require(!mkdir_error, "agent-c directory must be created");
    std::error_code link_error;
    fs::create_directory_symlink(
        outside, project / ".lingtai" / "agent-c" / "system", link_error);
    require(!link_error, "symlinked system fixture must be created");
    const auto outside_before = tree_snapshot(outside);

    const auto c = read_agent_preset_summary(attachment, "agent-c");
    require(tree_snapshot(outside) == outside_before,
        "reading through a blocked intermediate symlink must never write "
        "outside state");
    require(c.source == AgentPresetSummarySource::unavailable,
        "a symlinked `system` directory component must reduce to "
        "unavailable, not be followed");
    require(!c.active_ref && c.allowed.empty(),
        "an unavailable observation must carry no partial projection");
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

        verify_resolved_stale_and_unsafe(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "agent_preset_summary: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "agent_preset_summary: all checks passed\n";
    return 0;
}
