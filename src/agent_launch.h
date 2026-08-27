#pragma once

#include "project_attachment.h"

#include <cstdint>
#include <filesystem>

namespace lingtai::desktop {

enum class AgentLaunchResult { started, refused };

struct AgentLaunchOutcome {
    AgentLaunchResult result = AgentLaunchResult::refused;
    std::int64_t pid = 0;
    std::filesystem::path log_path;
};

// Starts exactly one canonical kernel CLI, detached and without a shell, for
// the exact selected Agent directory under the given attachment:
// `<python> -m lingtai run <absolute-selected-agent-dir>`. The interpreter
// is the selected Agent's own top-level `init.json.venv_path` platform
// Python when that exact absolute path's `bin/python` file exists,
// otherwise `fallback_python`. This performs no provisioning, import-
// probing, or repair of either interpreter: a present but otherwise broken
// configured interpreter is still attempted rather than silently replaced,
// and the caller's own heartbeat observation is the only truth for whether
// the Agent actually came up. Stdout/stderr are redirected to the selected
// Agent's own `logs/agent.log`, creating `logs/` first if absent, matching
// the current TUI launcher's own redirection so a failure message pointing
// there is truthful. Redirection inherits the descriptor-validated no-follow
// append file, avoiding a pathname reopen race. The kernel child owns
// configuration validation, duplicate defense, workdir lease, signal cleanup,
// and lifetime; this seam never waits on or manages the detached process.
[[nodiscard]] AgentLaunchResult start_agent(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key,
    const std::filesystem::path &fallback_python) noexcept;

// The lifecycle controller variant also returns the detached PID and the
// exact log leaf. PID is observation-only: every later signal still rechecks
// the process's exact argv, so PID reuse can never authorize termination.
[[nodiscard]] AgentLaunchOutcome launch_agent(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key,
    const std::filesystem::path &fallback_python) noexcept;

} // namespace lingtai::desktop
