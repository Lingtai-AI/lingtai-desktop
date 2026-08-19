#include "slash_command.h"

#include <cctype>

namespace lingtai::desktop {
namespace {

[[nodiscard]] std::string_view trim_argument_edges(std::string_view value) {
    while (!value.empty()
            && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty()
            && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace

std::optional<SlashCommand> parse_slash_command(std::string_view raw_text) {
    if (raw_text.size() <= 1 || raw_text.front() != '/') return std::nullopt;

    const auto body = raw_text.substr(1);
    const auto separator = body.find(' ');
    if (separator == std::string_view::npos) {
        return SlashCommand{std::string(body), {}};
    }

    const auto args = trim_argument_edges(body.substr(separator + 1));
    return SlashCommand{
        std::string(body.substr(0, separator)), std::string(args)};
}

namespace {

constexpr SlashCommandOffer kDesktopSlashCatalog[] = {
    {"agents", "Show the Agent list"},
    {"presets", "List this Agent's allowed presets"},
    {"setup", "Set up a project (presets, agents, review)"},
    {"kanban", "Open the agent network board"},
    {"sleep", "Request sleep"},
    {"cpr", "Start this Agent"},
    {"clear", "Clear the conversation"},
    {"refresh", "Refresh this Agent"},
    {"suspend", "Suspend this Agent"},
    {"btw", "Ask the agent a side question"},
    {"insights", "Request an insight from the agent now"},
    {"goal", "Guide goal creation"},
    {"export", "Export a recipe for sharing"},
    {"molt", "Force agent to molt now"},
    {"help", "List available commands"},
    {"quit", "Quit LingTai Desktop"},
};

} // namespace

std::vector<SlashCommandOffer> matching_slash_commands(std::string_view typed) {
    auto matches = std::vector<SlashCommandOffer>();
    if (typed.empty() || typed.front() != '/') return matches;
    const auto rest = typed.substr(1);
    if (rest.find(' ') != std::string_view::npos) return matches;
    for (const auto &offer : kDesktopSlashCatalog) {
        const auto name = std::string_view(offer.name);
        if (name.size() >= rest.size() && name.substr(0, rest.size()) == rest) {
            matches.push_back(offer);
        }
    }
    if (matches.size() == 1 && std::string_view(matches.front().name) == rest) {
        return {};
    }
    return matches;
}

} // namespace lingtai::desktop
