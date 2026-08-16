#include <iostream>
#include <string_view>

#if __has_include("slash_command.h")
#include "slash_command.h"
#define LINGTAI_HAS_SLASH_COMMAND 1
#else
#define LINGTAI_HAS_SLASH_COMMAND 0
#endif

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

#if LINGTAI_HAS_SLASH_COMMAND
using lingtai::desktop::parse_slash_command;

void expect_command(std::string_view raw, std::string_view name,
        std::string_view args, std::string_view message) {
    const auto parsed = parse_slash_command(raw);
    expect(parsed.has_value(), message);
    if (!parsed) return;
    expect(parsed->name == name && parsed->args == args, message);
}
#endif
} // namespace

int main() {
#if !LINGTAI_HAS_SLASH_COMMAND
    expect(false, "the pure conversation slash parser is present");
#else
    expect_command("/sleep", "sleep", "", "a command without args is parsed");
    expect_command("/refresh codex", "refresh", "codex",
        "the first ASCII space separates command from args");
    expect_command("/clear   now  later  ", "clear", "now  later",
        "argument edges are trimmed while internal spaces are preserved");
    expect_command("/unknown x", "unknown", "x",
        "classification preserves an unknown command for later dispatch");
    expect_command("/sleep\tall", "sleep\tall", "",
        "only a literal ASCII space separates arguments");

    expect(!parse_slash_command("/"), "a bare slash is ordinary input");
    expect(!parse_slash_command(" /sleep"),
        "leading whitespace keeps slash text out of the command path");
    expect(!parse_slash_command("hello"), "ordinary text is not consumed");
    expect(!parse_slash_command(""), "empty text is not a command");
#endif

    if (failures != 0) return 1;
    std::cout << "slash command parser: OK\n";
    return 0;
}
