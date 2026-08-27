#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lingtai::desktop {

using AgentProcessId = std::int64_t;

struct AgentProcessObservation {
    // False means the host process table was unavailable. An available empty
    // result is distinct and proves that no exact process was observed.
    bool available = false;
    std::vector<AgentProcessId> pids;
};

enum class AgentTerminationSignal { terminate, kill };

// Pure exact-argv policy. Only an argv vector shaped exactly as
// `<python> -m lingtai run <canonical-agent-dir>` matches; wrappers, console
// scripts, substrings, relative paths, extra options, and sibling directories
// do not.
[[nodiscard]] bool matches_exact_agent_process(
    const std::vector<std::string> &argv,
    const std::filesystem::path &canonical_agent_dir) noexcept;

// Production process-table adapter. On macOS it reads KERN_PROCARGS2 so paths
// containing spaces remain separate argv values; Linux uses `/proc/*/cmdline`.
// Unsupported/unavailable mechanisms fail closed with `available=false`.
[[nodiscard]] AgentProcessObservation observe_exact_agent_processes(
    const std::filesystem::path &canonical_agent_dir) noexcept;

// Re-reads the PID's exact argv immediately before signaling, preventing a
// stale observation or PID reuse from terminating an unrelated process.
[[nodiscard]] bool signal_exact_agent_process(
    const std::filesystem::path &canonical_agent_dir,
    AgentProcessId pid,
    AgentTerminationSignal signal) noexcept;

} // namespace lingtai::desktop
