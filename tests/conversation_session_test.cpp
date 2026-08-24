#include "conversation_session.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using lingtai::desktop::ConversationSessionEntry;
using lingtai::desktop::ConversationVerboseLevel;
using lingtai::desktop::SessionTokenUsage;
using lingtai::desktop::advance_conversation_session_revision;
using lingtai::desktop::conversation_api_group_separator_before;
using lingtai::desktop::conversation_verbose_event_body;
using lingtai::desktop::conversation_verbose_event_visible;
using lingtai::desktop::cycle_conversation_verbose_level;
using lingtai::desktop::format_token_usage_footer;
using lingtai::desktop::parse_conversation_session_line;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void verify_verbose_cycle() {
    require(
        cycle_conversation_verbose_level(ConversationVerboseLevel::off)
            == ConversationVerboseLevel::thinking,
        "off cycles to thinking");
    require(
        cycle_conversation_verbose_level(ConversationVerboseLevel::thinking)
            == ConversationVerboseLevel::extended,
        "thinking cycles to extended");
    require(
        cycle_conversation_verbose_level(ConversationVerboseLevel::extended)
            == ConversationVerboseLevel::off,
        "extended cycles to off");
}

void verify_token_footer() {
    SessionTokenUsage usage;
    usage.input = 181600;
    usage.output = 2300;
    usage.cached = 180200;
    usage.api_duration_ms = 12300;
    const auto footer = format_token_usage_footer(usage);
    require(
        footer.find("181.6k") != std::string::npos,
        "footer humanizes input tokens");
    require(
        footer.find("2.3k") != std::string::npos,
        "footer humanizes output tokens");
    require(
        footer.find("99.2%") != std::string::npos,
        "footer includes cache rate");
    require(
        footer.find("12.3 s") != std::string::npos,
        "footer includes API duration");

    usage.estimated = true;
    require(
        format_token_usage_footer(usage).front() == '~',
        "estimated rounds prefix with ~");
}

void verify_parse_llm_response() {
    const auto line = R"({"type":"llm_response","api_call_id":"abc","input_tokens":100,"output_tokens":20,"cached_tokens":80,"provider_wait_ms":500})";
    const auto parsed = parse_conversation_session_line(line);
    require(parsed.has_value(), "llm_response parses");
    require(parsed->type == "llm_response", "type preserved");
    require(parsed->api_call_id == "abc", "api_call_id preserved");
    require(parsed->token_usage.has_value(), "token usage captured");
    require(parsed->token_usage->input == 100, "input tokens captured");
}

void verify_parse_thinking_and_tool() {
    const auto thinking = parse_conversation_session_line(
        R"({"type":"thinking","text":"alpha beta","api_call_id":"g1"})");
    require(thinking.has_value(), "thinking parses");
    require(thinking->body == "alpha beta", "thinking body captured");

    const auto ignored = parse_conversation_session_line(
        R"({"type":"text_input","text":"prompt","api_call_id":"g1"})");
    require(!ignored.has_value(), "text_input stays hidden");

    const auto tool = parse_conversation_session_line(
        R"({"type":"tool_call","tool_name":"file","tool_args":{"action":"read","input":{"file_path":"/tmp/x"}},"api_call_id":"g1"})");
    require(tool.has_value(), "tool_call parses");
    require(
        tool->body == R"(file.read({"file_path":"/tmp/x"}))",
        "tool_call body uses LTP formatting");

    const auto result = parse_conversation_session_line(
        R"({"type":"tool_result","tool_name":"bash","status":"ok","elapsed_ms":42,"result":"done"})");
    require(result.has_value(), "tool_result parses");
    require(
        result->body.find("bash → ok 42ms") != std::string::npos,
        "tool_result body uses status header");
}

void verify_verbose_body_levels() {
    ConversationSessionEntry thinking;
    thinking.type = "thinking";
    thinking.body = std::string(200, 'x');
    require(
        conversation_verbose_event_body(
            thinking, ConversationVerboseLevel::thinking).size() <= 153,
        "thinking preview truncates at thinking level");

    ConversationSessionEntry tool;
    tool.type = "tool_result";
    tool.body = "bash → ok 12ms\nresult: long output";
    require(
        conversation_verbose_event_body(
            tool, ConversationVerboseLevel::thinking) == "bash → ok 12ms",
        "tool_result preview keeps status line only");
}

void verify_group_separator() {
    ConversationSessionEntry first;
    first.type = "tool_result";
    first.api_call_id = "a";
    ConversationSessionEntry second;
    second.type = "tool_call";
    second.api_call_id = "b";
    require(
        conversation_api_group_separator_before(&first, second),
        "api_call_id change inserts separator");

    ConversationSessionEntry llm;
    llm.type = "llm_response";
    require(
        !conversation_verbose_event_visible(
            llm, ConversationVerboseLevel::thinking),
        "llm_response without usage stays hidden");
    llm.token_usage = SessionTokenUsage{};
    llm.token_usage->input = 1;
    require(
        conversation_verbose_event_visible(
            llm, ConversationVerboseLevel::thinking),
        "llm_response with usage is retained for footers");
}

void verify_session_revision_idempotence() {
    auto revision = std::uint64_t{0};
    auto entries = std::vector<ConversationSessionEntry>();
    require(!advance_conversation_session_revision(entries, entries, revision)
            && revision == 0,
        "an identical session result must not advance presentation revision");
    auto changed = entries;
    changed.push_back({.timestamp = "1", .type = "thinking", .body = "work"});
    require(advance_conversation_session_revision(entries, changed, revision)
            && revision == 1,
        "a real session-event result must advance revision exactly once");
    require(!advance_conversation_session_revision(changed, changed, revision)
            && revision == 1,
        "a duplicate completed result must be revision-idempotent");
    require(advance_conversation_session_revision(changed, {}, revision)
            && revision == 2,
        "a real session clear must advance revision");
    require(!advance_conversation_session_revision({}, {}, revision)
            && revision == 2,
        "a repeated empty session clear must remain revision-idempotent");
}

} // namespace

int main() {
    try {
        verify_verbose_cycle();
        verify_token_footer();
        verify_parse_llm_response();
        verify_parse_thinking_and_tool();
        verify_verbose_body_levels();
        verify_group_separator();
        verify_session_revision_idempotence();
        std::cout << "conversation_session_test: ok\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "conversation_session_test: " << error.what() << '\n';
        return 1;
    }
}
