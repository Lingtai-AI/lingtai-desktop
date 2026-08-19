#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lingtai::desktop {

// One composer-local slash command. Classification preserves the command and
// argument text; support and side effects belong to the later dispatch owner.
struct SlashCommand {
    std::string name;
    std::string args;
};

// One Desktop composer offer shown in the leading-slash popup. Names match
// the commands `NativeShell::handle_send_message` actually dispatches.
struct SlashCommandOffer {
    const char *name = nullptr;
    const char *description = nullptr;
};

// Matches the TUI composer boundary: only a leading slash with at least one
// following character is a command, and only the first literal ASCII space
// separates its name from optional trimmed arguments.
[[nodiscard]] std::optional<SlashCommand>
parse_slash_command(std::string_view raw_text);

// Prefix-filters the Desktop slash catalog for the live composer popup. A
// bare `/` offers every command; a following space (arguments) yields none;
// an exact unique name also yields none so the popup can dismiss itself
// after a completed choice.
[[nodiscard]] std::vector<SlashCommandOffer>
matching_slash_commands(std::string_view typed);

} // namespace lingtai::desktop
