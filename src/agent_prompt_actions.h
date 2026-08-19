#pragma once

#include "project_attachment.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace lingtai::desktop {

// Local write outcomes for `.prompt` and `.inquiry`. `written` proves the
// bytes landed. `already_pending` is the TUI one-at-a-time no-op when
// `.inquiry` or `.inquiry.taken` already exists. Neither is Agent
// acceptance; both are local filesystem facts.
enum class AgentPromptWriteResult { written, already_pending, failed_local };

struct AgentGoalRequestResult {
    bool ok = false;
    std::string event_id;
};

// Writes `<agent>/.prompt` with the given content, walked descriptor-relative
// and no-follow from the accepted project root. The selected Agent directory
// is never created.
[[nodiscard]] AgentPromptWriteResult write_agent_prompt(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key,
    std::string_view content) noexcept;

// Writes `<agent>/.inquiry` as `source\nquestion`. Returns
// `already_pending` without mutating the tree when `.inquiry` or
// `.inquiry.taken` already exists.
[[nodiscard]] AgentPromptWriteResult write_agent_inquiry(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key,
    std::string_view source,
    std::string_view question) noexcept;

// TUI `/insights`: one insight inquiry with the canonical auto-question.
[[nodiscard]] AgentPromptWriteResult write_insight_inquiry(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

// TUI `/molt`: localized mandatory molt prompt from the Agent's init.json
// language, defaulting to English.
[[nodiscard]] AgentPromptWriteResult write_molt_prompt(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

// TUI `/export` and `/export recipe`: the canonical recipe-export prompt.
[[nodiscard]] AgentPromptWriteResult write_export_recipe_prompt(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key) noexcept;

// TUI `/goal [draft]`: appends one `source=goal.request` event to
// `<agent>/.notification/system.json`, creating `.notification` when absent
// and capping `data.events` at 20.
[[nodiscard]] AgentGoalRequestResult write_agent_goal_request(
    const ProjectAttachment &attachment,
    const std::filesystem::path &selected_directory_key,
    std::string_view human_request) noexcept;

} // namespace lingtai::desktop
