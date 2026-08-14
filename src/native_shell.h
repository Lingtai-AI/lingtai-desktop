#pragma once

#include "agent_launch.h"
#include "agent_projection.h"
#include "agent_sleep.h"
#include "agent_task_card.h"
#include "project_bootstrap.h"
#include "ui/agent_roster.h"
#include "workspace_selection.h"

// Vendored composer widget headers pull in ui/text/text_entity.h, whose
// aggregate brace init omits a field. Targets that include these headers (the
// shell implementation and its behavior test) build with -Werror, which would
// promote that vendored warning to an error. Vendored sources are out of
// scope, so scope the suppression to exactly this include boundary.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#pragma GCC diagnostic pop

#include <rpl/lifetime.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QDialog;
class QPushButton;
class QTimer;

namespace Ui {
class PlainShadow;
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

// The one compact secondary page treatment for the selected Agent: the chat
// (conversation) is the default surface, and the three read-only sources each
// own one page behind a small nav row so only one content surface shows at a
// time.
enum class AgentDetailPage {
    conversation,
    activity,
    task_card,
    presets,
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
    // The one Desktop-configured TUI executable for explicit first-project
    // bootstrap: the shipped `lingtai-tui` or a focused test fixture. It is
    // never interpreted as a shell command string; only exact separate argv
    // is ever passed to it. Defaults to empty (New Project fails closed).
    void set_tui_executable(std::filesystem::path executable);
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
    void request_new_project();
    void handle_presets_finished(PresetDiscoveryResult result);
    void handle_spawn_finished(SpawnOutcome outcome);
    void handle_create_and_start();
    void handle_cancel_bootstrap();
    void handle_browse_destination();
    void set_bootstrap_actions_enabled(bool enabled);
    void set_bootstrap_status(const QString &text);
    void show_bootstrap_dialog(const std::vector<PresetEntry> &presets);
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
    // Telegram's one mode recompute: derives OneColumn vs Normal from the
    // body's available column space on every real resize, and shows exactly
    // one full-width surface (roster or selected detail + Back) below the
    // two-surface threshold.
    void recompute_layout(int body_width);
    // Telegram's OneColumn history-back path: the narrow detail returns to
    // the roster and drops the selection. No-op outside the narrow detail.
    void handle_detail_back();
    // Switches the selected-Agent detail to exactly one page: the chat
    // (conversation) by default, or one of the three read-only secondary
    // sources, so only one content surface dominates at a time.
    void show_detail_page(AgentDetailPage page);
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
    // The persistent left 260px project/Agent list column. It owns the
    // project identity header, the compact Open/New Project actions, and the
    // Agent rows; the shell wires its row clicks and the action buttons.
    AgentRoster *agent_roster_ = nullptr;
    // The flexible right content pane beside the roster, hidden in OneColumn
    // roster mode and full-width in OneColumn detail mode.
    Ui::RpWidget *content_ = nullptr;
    Ui::PlainShadow *separator_ = nullptr;
    // The one compact palette-owned Back control in the detail header, visible
    // only in Telegram's narrow OneColumn detail view.
    QPushButton *detail_back_button_ = nullptr;
    Ui::RpWidget *empty_route_ = nullptr;
    Ui::RpWidget *project_route_ = nullptr;
    Ui::RpWidget *open_error_surface_ = nullptr;
    AgentSnapshot agents_;
    OpenProjectRequestHandler open_project_request_handler_;
    std::filesystem::path agent_start_fallback_python_;
    // One narrow injectable dependency: the configured TUI executable used
    // only by the explicit New Project flow.
    std::filesystem::path tui_executable_;
    // The one async owner of the headless `presets`/`spawn` subprocess calls.
    // Owned by the shell; no PID, lock, retry, or rollback machinery.
    std::unique_ptr<ProjectBootstrapRunner> bootstrap_runner_;
    // The one small Desktop-owned New Project dialog, built once in the
    // constructor and hidden until a successful preset discovery.
    QDialog *bootstrap_dialog_ = nullptr;
    Ui::RpWidget *bootstrap_status_surface_ = nullptr;
    // Owns the vendored composer input's Enter-to-send subscription for the
    // whole shell lifetime. The button click uses the button's own lifetime.
    rpl::lifetime submits_lifetime_;
    // Owns the single body-resize mode recompute subscription for the whole
    // shell lifetime; Telegram derives OneColumn vs Normal on every chats
    // resize, so Desktop's one recompute rides the same event stream.
    rpl::lifetime layout_lifetime_;
    // True while a New Project subprocess is pending (preset discovery or
    // spawn). While true the New Project and Open Project actions are
    // disabled so duplicate activation is impossible.
    bool bootstrap_pending_ = false;
    // View-scoped: exists only for the shell's own lifetime, re-invokes the
    // same stateless snapshot reader every second, and owns no cursor/offset
    // state of its own.
    QTimer *activity_timer_ = nullptr;
    std::optional<SleepObservation> pending_sleep_observation_;
    std::optional<StartObservation> pending_start_observation_;
    // The one compact selected-Agent page navigation: exactly one nav control
    // per AgentDetailPage, wired by the shell to `show_detail_page`. The
    // three secondary section owners that the nav reveals are captured in the
    // same construction order so `show_detail_page` can show exactly one.
    std::vector<QPushButton *> page_nav_buttons_;
    std::vector<Ui::RpWidget *> secondary_pages_;
    // The current selected target's last valid Task Card projection
    // (active or inactive), preserved only so a transient unavailable
    // observation does not clear or misreport it. Reset immediately on
    // project open or Agent selection change; never persisted.
    std::optional<AgentTaskCardSnapshot> task_card_last_valid_;
};

} // namespace lingtai::desktop
