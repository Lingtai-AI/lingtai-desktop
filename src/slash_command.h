#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lingtai::desktop {

// One composer-local slash command. Classification preserves the command and
// argument text; support and side effects belong to the later dispatch owner.
struct SlashCommand {
    std::string name;
    std::string args;
};

// Matches the TUI composer boundary: only a leading slash with at least one
// following character is a command, and only the first literal ASCII space
// separates its name from optional trimmed arguments.
[[nodiscard]] std::optional<SlashCommand>
parse_slash_command(std::string_view raw_text);

} // namespace lingtai::desktop
