#pragma once

#include "agent_launch.h"
#include "agent_projection.h"
#include "agent_sleep.h"
#include "agent_task_card.h"
#include "workspace_selection.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QTimer;

namespace Ui {
class RpWidget;
class RpWindow;
} // namespace Ui

namespace lingtai::desktop {

enum class ProjectOpenDisposition {
    opened,
    failed,
};

struct ProjectOpenOutcome {
    ProjectOpenDisposition disposition = ProjectOpenDisposition::failed;
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
    // Application composition's one concrete fallback interpreter, used only
    // when a selected Agent's own `init.json.venv_path` is absent or its
    // platform Python does not exist. Defaults to an empty path.
    void set_agent_start_fallback_python(std::filesystem::path fallback_python);
    [[nodiscard]] ProjectOpenOutcome open_project(
        const std::filesystem::path &selected_directory,
        const std::optional<std::filesystem::path> &agent_relative_directory
            = std::nullopt);

    [[nodiscard]] Ui::RpWindow &window() noexcept;
    [[nodiscard]] const Ui::RpWindow &window() const noexcept;
    [[nodiscard]] const WorkspaceSelectionState &selection_state()
        const noexcept;

    // True once the window, body, sidebar, and content are constructed,
    // correctly named, and actually shown offscreen. `main.cpp`'s `--smoke`
    // entry point is the sole caller; this is real product readiness, not a
    // public test seam.
    [[nodiscard]] bool smoke_ready() const noexcept;

private:
    void request_open_project();
    void refresh_route();
    void render_roster();
    void render_conversation();
    void render_agent_activity();
    void render_agent_task_card();
    void render_agent_preset_summary();
    void reset_composer();
    void handle_send_message();
    void handle_agent_selection(const std::filesystem::path &directory_key);
    void render_agent_sleep_status();
    void handle_request_sleep();
    void tick_agent_sleep_observation();
    void render_agent_start_status();
    void handle_start_agent();
    void tick_agent_start_observation();
    [[nodiscard]] ProjectOpenOutcome show_open_error(
        ProjectPathFailure failure,
        std::string message);

    // The one action-local pending sleep observation for at most three
    // wall-clock seconds after a successful write; discarded whenever the
    // project or Agent selection changes, and never persisted. It is not a
    // command ledger.
    struct SleepObservation {
        std::filesystem::path project_root, directory_key;
        AgentSleepEventBaseline baseline;
        std::chrono::steady_clock::time_point deadline;
    };

    // The one action-local pending Start observation for at most ten
    // wall-clock seconds after a successful detached start; discarded
    // whenever the project or Agent selection changes, and never
    // persisted. Success is proven solely by the sole `project_agents`
    // projection later reporting this exact selection `alive`; there is no
    // separate heartbeat baseline or second reader.
    struct StartObservation {
        std::filesystem::path project_root, directory_key;
        std::chrono::steady_clock::time_point deadline;
    };

    WorkspaceSelectionState selection_state_;
    std::unique_ptr<Ui::RpWindow> window_;
    Ui::RpWidget *empty_route_ = nullptr;
    Ui::RpWidget *project_route_ = nullptr;
    Ui::RpWidget *open_error_surface_ = nullptr;
    Ui::RpWidget *roster_rows_ = nullptr;
    AgentSnapshot agents_;
    OpenProjectRequestHandler open_project_request_handler_;
    std::filesystem::path agent_start_fallback_python_;
    // View-scoped: exists only for the shell's own lifetime, re-invokes the
    // same stateless snapshot reader every second, and owns no cursor/offset
    // state of its own.
    QTimer *activity_timer_ = nullptr;
    std::optional<SleepObservation> pending_sleep_observation_;
    std::optional<StartObservation> pending_start_observation_;
    // The current selected target's last valid Task Card projection
    // (active or inactive), preserved only so a transient unavailable
    // observation does not clear or misreport it. Reset immediately on
    // project open or Agent selection change; never persisted.
    std::optional<AgentTaskCardSnapshot> task_card_last_valid_;
};

} // namespace lingtai::desktop
