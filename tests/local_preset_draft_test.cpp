#include "local_preset_draft.h"

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
using lingtai::desktop::kLocalPresetDraftMaxBytes;
using lingtai::desktop::LocalPresetDraftSaveResult;
using lingtai::desktop::load_local_preset_draft;
using lingtai::desktop::save_local_preset_draft;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// Exact byte/type image of a tree, so "never writes" is proven, not asserted.
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

// Retained evidence: absent->empty; a Unicode/newline round trip via a
// second independent `load` (this store's own "restart/reload", since it is
// stateless); project+Agent isolation, including a distinct project under
// the same `agent_id` string (the shape a directory rename would exercise);
// an oversize save leaving prior content intact; project tree never written.
void verify_local_preset_draft_journey(const fs::path &sandbox) {
    const auto app_data_root = sandbox / "app-data";
    const auto project = sandbox / "project";
    std::error_code error;
    fs::create_directories(app_data_root, error);
    require(!error, "app-data root must be created");
    fs::create_directories(project / ".lingtai" / "agent-a", error);
    require(!error, "project fixture must be created");
    const auto project_before = tree_snapshot(project);

    const auto absent = load_local_preset_draft(app_data_root, project, "agent-a");
    require(!absent.found && absent.document.empty(),
        "a key that was never saved must load as absent/empty");

    const auto unicode_document =
        std::string("Notes\n\xE4\xBD\xA0\xE5\xA5\xBD \xF0\x9F\x8E\x89\n"
                     "line three\r\nline four");
    const auto save_result =
        save_local_preset_draft(app_data_root, project, "agent-a", unicode_document);
    require(save_result == LocalPresetDraftSaveResult::saved,
        "saving a Unicode/newline document within bounds must succeed");

    const auto reloaded = load_local_preset_draft(app_data_root, project, "agent-a");
    require(reloaded.found, "a saved document must be found on a fresh reopen");
    require(reloaded.document == unicode_document,
        "the exact saved bytes must round-trip, including embedded newlines "
        "and multi-byte UTF-8");

    const auto reloaded_again =
        load_local_preset_draft(app_data_root, project, "agent-a");
    require(reloaded_again.found && reloaded_again.document == unicode_document,
        "an independent later reopen must read back the same exact bytes");

    const auto other_agent =
        load_local_preset_draft(app_data_root, project, "agent-b");
    require(!other_agent.found,
        "a distinct agent_id under the same project must not resolve to "
        "another Agent's saved draft");

    // Same agent_id string, different project root: must not collide; the
    // same shape a directory rename would exercise.
    const auto other_project = sandbox / "project-two";
    fs::create_directories(other_project, error);
    require(!error, "second project fixture must be created");
    const auto same_agent_id_other_project =
        load_local_preset_draft(app_data_root, other_project, "agent-a");
    require(!same_agent_id_other_project.found,
        "the same agent_id string under a different project root must not "
        "collide with the first project's saved draft");
    require(save_local_preset_draft(
                app_data_root, other_project, "agent-a", "second project text")
            == LocalPresetDraftSaveResult::saved,
        "saving under the second project must independently succeed");
    const auto first_project_unaffected =
        load_local_preset_draft(app_data_root, project, "agent-a");
    require(first_project_unaffected.found
            && first_project_unaffected.document == unicode_document,
        "saving a same-agent_id draft under a different project must never "
        "mutate the first project's own saved draft");

    const auto oversized = std::string(kLocalPresetDraftMaxBytes + 1, 'x');
    const auto oversized_result =
        save_local_preset_draft(app_data_root, project, "agent-a", oversized);
    require(oversized_result == LocalPresetDraftSaveResult::failed,
        "a document over the 64 KiB encoded bound must be refused");
    const auto after_oversized_attempt =
        load_local_preset_draft(app_data_root, project, "agent-a");
    require(after_oversized_attempt.found
            && after_oversized_attempt.document == unicode_document,
        "a refused oversize save must leave the prior saved content exactly "
        "intact, never partially written or cleared");

    require(tree_snapshot(project) == project_before,
        "no save or load call above may ever write the attached project tree");
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

        verify_local_preset_draft_journey(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "local_preset_draft: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "local_preset_draft: all checks passed\n";
    return 0;
}
