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

} // namespace lingtai::desktop
