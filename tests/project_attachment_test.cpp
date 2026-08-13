#include "src/project_attachment.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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
    const auto sibling = sandbox / "project-evil";
    std::error_code error;
    fs::remove_all(sandbox, error);
    expect(!error, "test sandbox is reset");
    fs::create_directories(nested, error);
    expect(!error, "project fixture directories are created");
    fs::create_directories(outside, error);
    expect(!error, "outside fixture directory is created");
    fs::create_directories(sibling, error);
    expect(!error, "same-prefix sibling fixture directory is created");
    write_file(nested / "note.txt", "inside");
    write_file(outside / "secret.txt", "outside");
    write_file(sibling / "evil.txt", "sibling");
    fs::create_directory_symlink(project, sandbox / "project-link", error);
    expect(!error, "selected-root symlink fixture is created");
    fs::create_directory_symlink(nested, project / "inside-link", error);
    expect(!error, "contained symlink fixture is created");
    fs::create_directory_symlink(outside, project / "escape-link", error);
    expect(!error, "escaping symlink fixture is created");
    fs::create_directory_symlink(sibling, project / "sibling-link", error);
    expect(!error, "same-prefix sibling symlink fixture is created");

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
    const auto selected_below_file =
        attach_project(nested / "note.txt" / "project");
    expect(
        selected_below_file.failure ==
            ProjectPathFailure::selection_not_directory,
        "a selection below a regular file is not a directory");
    expect(
        selected_below_file.system_error == std::errc::not_a_directory,
        "a selection below a regular file retains ENOTDIR");

#if !defined(_WIN32)
    if (geteuid() == 0) {
        std::cout << "SKIP: permission fixture is ineffective as root\n";
    } else {
        const auto locked = sandbox / "locked";
        const auto locked_project = locked / "project";
        fs::create_directories(locked_project, error);
        expect(!error, "permission fixture directory is created");
        fs::permissions(
            locked,
            fs::perms::none,
            fs::perm_options::replace,
            error);
        if (error) {
            std::cout << "SKIP: platform does not support permission fixture\n";
        } else {
            const auto inaccessible = attach_project(locked_project);
            fs::permissions(
                locked,
                fs::perms::owner_all,
                fs::perm_options::replace,
                error);
            expect(!error, "permission fixture access is restored");
            expect(
                inaccessible.failure == ProjectPathFailure::filesystem_error,
                "an inaccessible selection has a filesystem failure");
            expect(
                inaccessible.system_error ==
                    std::errc::permission_denied,
                "an inaccessible selection retains EACCES");
        }
    }
#else
    std::cout << "SKIP: permission fixture requires POSIX permissions\n";
#endif

    if (attached) {
        static_assert(noexcept(attached.attachment->resolve(fs::path{})));

        const auto direct = attached.attachment->resolve("nested/note.txt");
        expect(static_cast<bool>(direct), "a contained relative path resolves");
        expect(
            direct.path == fs::canonical(nested / "note.txt"),
            "relative resolution returns the canonical target");

        const auto empty = attached.attachment->resolve("");
        expect(
            empty.failure == ProjectPathFailure::invalid_relative_path,
            "an empty relative path is invalid input");
        const auto dot = attached.attachment->resolve(".");
        expect(
            dot.failure == ProjectPathFailure::invalid_relative_path,
            "a dot-only relative path is invalid input");
        const auto directory = attached.attachment->resolve("nested");
        expect(
            static_cast<bool>(directory),
            "a nonempty contained directory remains valid");

        const auto below_file =
            attached.attachment->resolve("nested/note.txt/child");
        expect(
            below_file.failure == ProjectPathFailure::target_not_directory,
            "a target below a regular file is not a directory");
        expect(
            below_file.system_error == std::errc::not_a_directory,
            "a target below a regular file retains ENOTDIR");

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

        const auto sibling_escape =
            attached.attachment->resolve("sibling-link/evil.txt");
        expect(
            sibling_escape.failure == ProjectPathFailure::outside_project,
            "a same-prefix sibling is outside the project root");

        const auto missing = attached.attachment->resolve("new/output.txt");
        expect(
            missing.failure == ProjectPathFailure::target_not_found,
            "a missing target has a typed failure");
        expect(
            !fs::exists(project / "new"),
            "resolving a path performs no project-tree writes");
    }

    fs::remove_all(sandbox, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " project attachment assertion(s) failed\n";
        return 1;
    }
    std::cout << "PROJECT_ATTACHMENT_CONTRACT_OK\n";
    return 0;
}
