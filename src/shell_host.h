#pragma once

#include "native_shell.h"
#include "runtime_options.h"

#include <QtCore/QObject>

#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace lingtai::desktop {

class DesktopStatusItem;

// Owns every open NativeShell window for the process. Open Project still
// rebinds the requesting shell; Open Project in Another Window spawns another
// shell and opens the chosen directory only there.
class ShellHost final : public QObject {
public:
    explicit ShellHost(
        RuntimeOptions runtime_options,
        QObject *parent = nullptr);
    ~ShellHost() override;

    [[nodiscard]] NativeShell &primary();
    [[nodiscard]] NativeShell &shell_at(std::size_t index);
    [[nodiscard]] std::size_t shell_count() const;
    [[nodiscard]] std::size_t unread_total() const noexcept;
    [[nodiscard]] std::size_t open_project_count() const noexcept;

    // Shows a directory picker parented to `requester`, then opens or
    // bootstraps that path in `requester` itself.
    void open_project_for(NativeShell &requester);

    // Shows a directory picker parented to `requester`, then creates a new
    // window and opens or bootstraps that path only in the new shell.
    void open_project_in_new_window(NativeShell &requester);

    // Test / programmatic seam: open `directory` in a newly created window
    // without showing a directory picker.
    void open_path_in_new_window(
        NativeShell &requester,
        const std::filesystem::path &directory);

private:
    [[nodiscard]] NativeShell *spawn_shell();
    void configure_shell(NativeShell &shell);
    void watch_shell(NativeShell *shell);
    void show_lingtai();
    void remove_shell(NativeShell *shell);
    void refresh_unread_presentations();
    [[nodiscard]] std::optional<std::filesystem::path> pick_project_directory(
        QWidget *parent) const;
    void open_or_bootstrap(
        NativeShell &shell,
        const std::filesystem::path &directory);

    RuntimeOptions runtime_options_;
    std::filesystem::path agent_start_fallback_python_;
    ConversationUnreadSession unread_session_;
    std::vector<std::unique_ptr<NativeShell>> shells_;
    std::unordered_set<NativeShell *> closing_shells_;
    NativeShell *active_shell_ = nullptr;
    NativeShell *most_recent_active_shell_ = nullptr;
    DesktopStatusItem *status_item_ = nullptr;
    std::size_t unread_total_ = 0;
    std::size_t open_project_count_ = 0;
    bool shutting_down_ = false;
};

} // namespace lingtai::desktop
