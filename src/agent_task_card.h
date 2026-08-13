#pragma once

#include "project_attachment.h"

#include <filesystem>
#include <string>

namespace lingtai::desktop {

enum class AgentTaskCardState { active, inactive, unavailable };

// One stateless read-only projection of the selected Agent's own
// `<root>/.lingtai/<selected key>/taskcard/{status,taskcard.md}`. `body` is
// meaningful only when `state == active`.
struct AgentTaskCardSnapshot {
    AgentTaskCardState state = AgentTaskCardState::unavailable;
    std::string body;
};

// Reads exact status bytes `active` or `inactive` from `taskcard/status`
// (no trimming, case folding, or inference), and, only when exactly
// `active`, a complete bounded UTF-8 nonblank body from
// `taskcard/taskcard.md`. Every other observation -- missing, unsafe,
// unreadable, over-bound, invalid-UTF-8, or blank-with-active-status --
// reduces to `unavailable`: no new valid projection, never `inactive` and
// never an error attributed to the Agent. Every walked path component,
// including `taskcard` itself, is opened one no-follow leaf at a time, and
// both leaves must be real regular files. Stateless: every call is one
// independent snapshot with no durable cursor, and it writes nothing.
[[nodiscard]] AgentTaskCardSnapshot read_agent_task_card(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

} // namespace lingtai::desktop
