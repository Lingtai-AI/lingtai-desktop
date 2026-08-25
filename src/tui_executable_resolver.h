#pragma once

#include <filesystem>
#include <string>

namespace lingtai::desktop {

// All host-dependent inputs are supplied by the caller. The production
// wrapper uses the inherited PATH, home directory, and canonical macOS
// fallback directories; tests redirect every input into a fixture.
struct TuiExecutableSearch {
    std::string inherited_path;
    std::filesystem::path home;
    std::filesystem::path usr_local_bin;
    std::filesystem::path opt_homebrew_bin;
};

// Resolves the first acceptable candidate without launching a process or
// changing PATH. Accepted symlink paths are preserved rather than canonicalized.
[[nodiscard]] std::filesystem::path resolve_tui_executable(
    const TuiExecutableSearch &search) noexcept;

} // namespace lingtai::desktop
