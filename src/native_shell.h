#pragma once

#include "workspace_selection.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Ui {
class RpWidget;
class RpWindow;
} // namespace Ui

namespace lingtai::desktop {

struct NativeShellSnapshot {
    std::string window_class;
    std::string window_object;
    std::string body_object;
    std::string sidebar_object;
    std::string content_object;
    bool shown = false;
    bool empty_route_visible = false;
    bool positioned_offscreen = false;
};

enum class ProjectOpenDisposition {
    compatible,
    degraded,
    blocked,
    failed,
};

struct ProjectOpenOutcome {
    ProjectOpenDisposition disposition = ProjectOpenDisposition::failed;
    bool commands_allowed = false;
    ProjectPathFailure failure = ProjectPathFailure::none;
};

// C5-owned native composition. C1's WorkspaceSelectionState remains the only
// active project/Agent truth; an open request proposes no state transition.
class NativeShell final {
public:
    using OpenProjectRequestHandler = std::function<void()>;

    NativeShell();
    ~NativeShell();

    NativeShell(const NativeShell &) = delete;
    NativeShell &operator=(const NativeShell &) = delete;

    void show();
    void show_offscreen();
    void set_open_project_request_handler(OpenProjectRequestHandler handler);
    [[nodiscard]] ProjectOpenOutcome open_project(
        const std::filesystem::path &selected_directory,
        const std::filesystem::path &install_receipt_path,
        const std::optional<std::filesystem::path> &agent_relative_directory
            = std::nullopt);

    [[nodiscard]] Ui::RpWindow &window() noexcept;
    [[nodiscard]] const Ui::RpWindow &window() const noexcept;
    [[nodiscard]] const WorkspaceSelectionState &selection_state()
        const noexcept;
    [[nodiscard]] std::size_t open_project_request_count() const noexcept;
    [[nodiscard]] NativeShellSnapshot snapshot() const;

private:
    void request_open_project();
    void refresh_route();
    [[nodiscard]] ProjectOpenOutcome show_open_error(
        ProjectPathFailure failure,
        std::string message);

    WorkspaceSelectionState selection_state_;
    std::unique_ptr<Ui::RpWindow> window_;
    Ui::RpWidget *empty_route_ = nullptr;
    Ui::RpWidget *project_route_ = nullptr;
    Ui::RpWidget *open_error_surface_ = nullptr;
    OpenProjectRequestHandler open_project_request_handler_;
    std::size_t open_project_request_count_ = 0;
};

} // namespace lingtai::desktop
