#pragma once

#include "agent_launch.h"
#include "agent_process.h"
#include "agent_projection.h"
#include "project_attachment.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {

enum class AgentLifecycleCommand { sleep, suspend, cpr, clear, refresh };

enum class AgentLifecyclePhase {
    validation,
    signal_write,
    sleep_observation,
    suspend_observation,
    lease_wait,
    process_scan,
    terminate_wait,
    kill_wait,
    stale_cleanup,
    preset_update,
    launch,
    heartbeat_wait,
    clear_observation,
    temporary_suspend,
    complete,
};

enum class AgentLifecycleOutcomeKind {
    requested,
    applied,
    already_online,
    skipped,
    timed_out,
    partial,
    failed,
};

struct AgentLifecycleTargetResult {
    std::filesystem::path agent_key;
    AgentLifecycleOutcomeKind outcome = AgentLifecycleOutcomeKind::failed;
    AgentLifecyclePhase phase = AgentLifecyclePhase::validation;
    std::string detail;
};

struct AgentLifecycleResult {
    AgentLifecycleCommand command = AgentLifecycleCommand::sleep;
    bool all = false;
    std::filesystem::path bound_project_root;
    std::string bound_generation;
    std::vector<AgentLifecycleTargetResult> targets;
};

enum class AgentLifecycleStartResult {
    started,
    busy,
    invalid_argument,
    no_target,
};

struct AgentLifecycleRequest {
    ProjectAttachment attachment;
    AgentSnapshot snapshot;
    std::optional<std::filesystem::path> selected_agent_key;
    AgentLifecycleCommand command = AgentLifecycleCommand::sleep;
    std::string argument;
    std::filesystem::path fallback_python;
    std::string generation;
};

struct AgentLifecycleProcessAdapter {
    std::function<AgentProcessObservation(const std::filesystem::path &)> observe;
    std::function<bool(const std::filesystem::path &, AgentProcessId,
        AgentTerminationSignal)> signal;
};

struct AgentLifecycleLauncher {
    std::function<AgentLaunchOutcome(const ProjectAttachment &,
        const std::filesystem::path &, const std::filesystem::path &)> launch;
};

struct AgentLifecycleClock {
    std::function<std::chrono::steady_clock::time_point()> monotonic_now;
    std::function<double()> wall_seconds;
};

struct AgentLifecycleDependencies {
    AgentLifecycleProcessAdapter processes;
    AgentLifecycleLauncher launcher;
    AgentLifecycleClock clock;
    bool automatic_poll = true;
    std::chrono::milliseconds poll_interval{100};
};

[[nodiscard]] AgentLifecycleDependencies
production_agent_lifecycle_dependencies();

// Pure target policy shared by production and the component contract. A
// valid selected non-human Agent wins; otherwise the first valid Main is the
// single-target fallback. `all` returns every valid non-human Main/Agent in
// snapshot order, optionally restricted to heartbeat-live rows.
[[nodiscard]] std::vector<std::filesystem::path> resolve_lifecycle_targets(
    const AgentSnapshot &snapshot,
    const std::optional<std::filesystem::path> &selected_agent_key,
    bool all,
    bool live_only) noexcept;

[[nodiscard]] const char *agent_lifecycle_phase_name(
    AgentLifecyclePhase phase) noexcept;

[[nodiscard]] std::string agent_lifecycle_result_text(
    const AgentLifecycleResult &result);

// Desktop-owned asynchronous lifecycle state machine. `run` performs only
// bounded immediate filesystem work, then advances wait/escalation phases on
// timer ticks. It never sleeps or waits on the caller/UI thread. Exactly one
// operation is pending; `all` targets run serially and always aggregate every
// independently reachable terminal result.
class AgentLifecycleController final {
public:
    using Done = std::function<void(AgentLifecycleResult)>;

    explicit AgentLifecycleController(
        AgentLifecycleDependencies dependencies =
            production_agent_lifecycle_dependencies());
    ~AgentLifecycleController();

    AgentLifecycleController(const AgentLifecycleController &) = delete;
    AgentLifecycleController &operator=(const AgentLifecycleController &) = delete;

    [[nodiscard]] bool is_pending() const noexcept;
    [[nodiscard]] AgentLifecycleStartResult run(
        AgentLifecycleRequest request, Done done);

    // Deterministic/manual poll seam. Production also calls it from the
    // controller's short QTimer; component tests disable automatic polling
    // and advance an injected monotonic clock explicitly.
    void tick();

    // Suppresses future delivery and polling. It does not claim to roll back
    // a marker the kernel may already have consumed.
    void cancel() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lingtai::desktop
