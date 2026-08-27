#pragma once

#include "project_attachment.h"

#include <filesystem>

namespace lingtai::desktop {

// The complete set of kernel lifecycle leaves Desktop may write or remove.
// Content is owned by the signal kind: sleep/suspend/interrupt/refresh are
// zero-byte markers; clear is exactly `desktop\n`; refresh_taken is cleanup-
// only and can never be written through this seam.
enum class AgentSignalKind {
    sleep,
    suspend,
    interrupt,
    clear,
    refresh,
    refresh_taken,
};

enum class AgentSignalWriteResult { written, refused };
enum class AgentSignalRemoveResult { removed, absent, refused };
enum class AgentSignalObservation { present, absent, refused };

// Descriptor-relative, no-follow create-or-truncate of exactly one signal
// leaf below an already accepted project and exact Agent key. Neither the
// `.lingtai` directory nor the Agent directory is ever created.
[[nodiscard]] AgentSignalWriteResult write_agent_signal(
    const ProjectAttachment &attachment,
    const std::filesystem::path &agent_key,
    AgentSignalKind kind) noexcept;

// Descriptor-relative, no-follow removal of exactly one regular signal leaf.
// Symlinked/non-regular leaves are refused and never replaced or followed.
[[nodiscard]] AgentSignalRemoveResult remove_agent_signal(
    const ProjectAttachment &attachment,
    const std::filesystem::path &agent_key,
    AgentSignalKind kind) noexcept;

// Distinguishes a safely absent marker from an unsafe/uninspectable leaf so a
// waiter never mistakes a symlink swap for kernel consumption.
[[nodiscard]] AgentSignalObservation observe_agent_signal(
    const ProjectAttachment &attachment,
    const std::filesystem::path &agent_key,
    AgentSignalKind kind) noexcept;

} // namespace lingtai::desktop
