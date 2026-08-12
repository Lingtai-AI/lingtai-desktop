#include "src/project_attachment.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using lingtai::desktop::ProjectPathFailure;
using lingtai::desktop::attach_project;

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void write_file(const fs::path &path, const std::string &contents) {
    std::ofstream stream(path);
    stream << contents;
}

} // namespace

int main(int argc, char **argv) {
    static_assert(noexcept(attach_project(fs::path{})));

    if (argc != 2) {
        std::cerr << "expected one temporary-directory argument\n";
        return 2;
    }

    const auto sandbox = fs::path(argv[1]);
    const auto project = sandbox / "project";
    const auto nested = project / "nested";
    const auto outside = sandbox / "outside";
    std::error_code error;
    fs::create_directories(nested, error);
    expect(!error, "project fixture directories are created");
    fs::create_directories(outside, error);
    expect(!error, "outside fixture directory is created");
    write_file(nested / "note.txt", "inside");
    write_file(outside / "secret.txt", "outside");
    fs::create_directory_symlink(project, sandbox / "project-link", error);
    expect(!error, "selected-root symlink fixture is created");
    fs::create_directory_symlink(nested, project / "inside-link", error);
    expect(!error, "contained symlink fixture is created");
    fs::create_directory_symlink(outside, project / "escape-link", error);
    expect(!error, "escaping symlink fixture is created");

    const auto attached = attach_project(sandbox / "project-link");
    expect(static_cast<bool>(attached), "an existing directory attaches");
    expect(
        attached.attachment
            && attached.attachment->root() == fs::canonical(project),
        "attachment stores the stable canonical root");

    const auto missing_root = attach_project(sandbox / "absent");
    expect(
        missing_root.failure == ProjectPathFailure::selection_not_found,
        "a missing selection has a typed failure");
    const auto selected_file = attach_project(nested / "note.txt");
    expect(
        selected_file.failure == ProjectPathFailure::selection_not_directory,
        "a non-directory selection has a typed failure");

    if (attached) {
        static_assert(noexcept(attached.attachment->resolve(fs::path{})));

        const auto direct = attached.attachment->resolve("nested/note.txt");
        expect(static_cast<bool>(direct), "a contained relative path resolves");
        expect(
            direct.path == fs::canonical(nested / "note.txt"),
            "relative resolution returns the canonical target");

        const auto contained_link =
            attached.attachment->resolve("inside-link/note.txt");
        expect(
            contained_link.path == fs::canonical(nested / "note.txt"),
            "a symlink that stays inside the project resolves");

        const auto absolute =
            attached.attachment->resolve(nested / "note.txt");
        expect(
            absolute.failure == ProjectPathFailure::absolute_path_forbidden,
            "absolute paths are rejected even when they point inside");

        const auto traversal =
            attached.attachment->resolve("nested/../nested/note.txt");
        expect(
            traversal.failure ==
                ProjectPathFailure::parent_traversal_forbidden,
            "any parent traversal component is rejected");

        const auto escape =
            attached.attachment->resolve("escape-link/secret.txt");
        expect(
            escape.failure == ProjectPathFailure::outside_project,
            "a symlink cannot escape the canonical project root");

        const auto missing = attached.attachment->resolve("new/output.txt");
        expect(
            missing.failure == ProjectPathFailure::target_not_found,
            "a missing target has a typed failure");
        expect(
            !fs::exists(project / "new"),
            "resolving a path performs no project-tree writes");
    }

    if (failures != 0) {
        std::cerr << failures << " project attachment assertion(s) failed\n";
        return 1;
    }
    std::cout << "PROJECT_ATTACHMENT_CONTRACT_OK\n";
    return 0;
}
