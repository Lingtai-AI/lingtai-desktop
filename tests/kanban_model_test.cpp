#include "kanban_model.h"
#include "project_attachment.h"

#include <fcntl.h>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::KanbanBoardColumn;
using lingtai::desktop::attach_project;
using lingtai::desktop::derive_ledger_provider;
using lingtai::desktop::kanban_column_for_state;
using lingtai::desktop::project_agents;
using lingtai::desktop::read_kanban_board;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const fs::path &path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "fixture parent must be created: " + path.string());
    auto stream = std::ofstream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture must be written: " + path.string());
}

void verify_provider_derivation() {
    require(derive_ledger_provider("https://api.anthropic.com/v1", "")
            == "anthropic",
        "anthropic endpoint maps to anthropic");
    require(derive_ledger_provider("", "claude-opus") == "anthropic",
        "claude model prefix maps to anthropic");
    require(derive_ledger_provider("", "gpt-4.1") == "openai",
        "gpt model prefix maps to openai");
    require(kanban_column_for_state("active") == KanbanBoardColumn::active
            && kanban_column_for_state("STUCK") == KanbanBoardColumn::stuck,
        "state names map onto board columns");
}

void verify_board_reads_network_sources(const fs::path &sandbox) {
    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"h001","agent_name":"Ted","address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{"orchestrator":true},"agent_id":"a001","agent_name":"alpha","nickname":"Alpha",)"
        R"("address":"alpha","state":"active","language":"en","stamina":0.8,)"
        R"("created_at":"2026-08-01T00:00:00Z"})");
    write_file(project / ".lingtai/alpha/init.json",
        R"({"model":"claude-opus","provider":"anthropic",)"
        R"("mcp":{"fs":{},"browser":{}},"soul":{"delay":1.5}})");
    write_file(project / ".lingtai/alpha/.status.json",
        R"({"tokens":{"context":{"window_size":200000,"system_tokens":1200,)"
        R"("tools_tokens":800,"history_tokens":4000,"total_tokens":6000,)"
        R"("usage_pct":3.0}}})");
    write_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":100,\"output\":40,"
        "\"thinking\":10,\"cached\":20,\"model\":\"claude-opus\","
        "\"endpoint\":\"https://api.anthropic.com/v1\"}\n");
    write_file(project / ".lingtai/alpha/daemons/run-1/daemon.json",
        R"({"state":"running","backend":"claude-p",)"
        R"("cli_tokens":{"input":10,"output":5,"thinking":1,"cached":2,"calls":1}})");
    write_file(project / ".lingtai/alpha/history/chat_history.jsonl",
        R"({"role":"user","content":[{"type":"text","text":"hi"}]})" "\n"
        R"({"role":"assistant","content":[{"type":"tool_call","name":"read"}]})" "\n");
    write_file(project / ".lingtai/alpha/mailbox/inbox/m1/message.json",
        R"({"id":"m1","from":"human","to":"alpha","message":"hello"})");
    write_file(project / ".lingtai/alpha/delegates/ledger.jsonl",
        R"({"event":"avatar","name":"beta","working_dir":"beta"})" "\n");
    write_file(project / ".lingtai/beta/.agent.json",
        R"({"admin":{},"agent_id":"b001","agent_name":"beta","address":"beta","state":"idle"})");

    const auto attached = attach_project(project);
    require(static_cast<bool>(attached) && attached.attachment.has_value(),
        "fixture project must attach");
    const auto snapshot = project_agents(*attached.attachment);
    const auto board = read_kanban_board(*attached.attachment, snapshot);
    require(board.agent_count == 2 && board.human_count == 1,
        "board counts non-human agents separately from the human");
    require(board.active == 1 && board.idle == 1,
        "alpha is active and beta is idle");
    require(board.network_tokens.input == 100
            && board.network_tokens.output == 40
            && board.network_tokens.api_calls == 1,
        "network token totals come from the ledger");
    require(board.running_daemons == 1 && board.total_mails == 1,
        "daemon and inbox mail counts are included");
    require(board.network_created == "2026-08-01T00:00:00Z",
        "admin created_at becomes network created");

    const lingtai::desktop::KanbanAgent *alpha = nullptr;
    for (const auto &agent : board.agents) {
        if (agent.directory_key == "alpha") alpha = &agent;
    }
    require(alpha != nullptr, "alpha is on the board");
    require(alpha->display_name == "Alpha"
            && alpha->tokens.cached == 20
            && alpha->providers.size() == 1
            && alpha->providers.front().name == "anthropic"
            && alpha->daemon_providers.size() == 1
            && alpha->daemon_providers.front().name == "claude-p"
            && alpha->current_session.tokens.api_calls == 1
            && alpha->mcp_names.size() == 2
            && alpha->daemons.running == 1
            && alpha->daemon_runs_scanned == 1
            && alpha->daemon_runs_total == 1
            && alpha->context_stats.entries == 2
            && alpha->context_stats.tool_calls == 1
            && alpha->context && alpha->context->window_size == 200000,
        "alpha inspector facts are filled from ledger, init, status, and history");
    require(!board.tree_lines.empty(),
        "avatar ledger produces a network tree");
}

void write_minimal_agent(const fs::path &project) {
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"h001","agent_name":"Ted","address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{"orchestrator":true},"agent_id":"a001","agent_name":"alpha",)"
        R"("address":"alpha","state":"active"})");
}

[[nodiscard]] lingtai::desktop::KanbanBoard board_from_project(const fs::path &project) {
    const auto attached = attach_project(project);
    require(static_cast<bool>(attached) && attached.attachment.has_value(),
        "fixture project must attach: " + project.string());
    return read_kanban_board(*attached.attachment, project_agents(*attached.attachment));
}

[[nodiscard]] const lingtai::desktop::KanbanAgent *require_agent(
        const lingtai::desktop::KanbanBoard &board, std::string_view key) {
    for (const auto &agent : board.agents) {
        if (agent.directory_key.string() == key) return &agent;
    }
    throw std::runtime_error("missing agent " + std::string(key));
}

void verify_large_ledger_is_streamed(const fs::path &sandbox) {
    const auto project = sandbox / "large-ledger";
    write_minimal_agent(project);
    const auto path = project / ".lingtai/alpha/logs/token_ledger.jsonl";
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "large ledger parent must be created");
    auto stream = std::ofstream(path, std::ios::binary);
    const auto pad = std::string(2U * 1024U * 1024U + 1U, '\n');
    stream << pad;
    stream << "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":7,\"output\":1,"
              "\"thinking\":0,\"cached\":0,\"model\":\"claude-opus\","
              "\"endpoint\":\"https://api.anthropic.com/v1\"}\n";
    require(stream.good(), "oversized ledger must be written");
    stream.close();
    require(fs::file_size(path) > 2U * 1024U * 1024U,
        "ledger fixture must exceed the old 2MB whole-file cap");

    const auto board = board_from_project(project);
    const auto *alpha = require_agent(board, "alpha");
    require(alpha->tokens.input == 7 && alpha->tokens.output == 1
            && alpha->providers.size() == 1
            && alpha->providers.front().name == "anthropic",
        "ledgers larger than 2MB must still be read, matching TUI");
}

void set_mtime(const fs::path &path, std::time_t seconds) {
    struct timespec times[2] = {};
    times[0].tv_sec = seconds;
    times[1].tv_sec = seconds;
    require(::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0,
        "run directory mtime must be pinned: " + path.string());
}

void verify_daemon_run_window(const fs::path &sandbox) {
    const auto project = sandbox / "daemon-window";
    write_minimal_agent(project);
    const auto daemons = project / ".lingtai/alpha/daemons";
    constexpr auto stamp = static_cast<std::time_t>(1'700'000'000);
    for (int i = 0; i < 129; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "run-%03d", i);
        const auto dir = daemons / name;
        if (i == 0) {
            write_file(dir / "daemon.json",
                R"({"state":"done","backend":"claude-p",)"
                R"("cli_tokens":{"input":9999,"output":9999,"thinking":0,"cached":0,"calls":1}})");
        } else {
            write_file(dir / "daemon.json",
                R"({"state":"done","backend":"chatgpt.com",)"
                R"("cli_tokens":{"input":1,"output":1,"thinking":0,"cached":0,"calls":1}})");
        }
        set_mtime(dir, stamp);
    }

    const auto board = board_from_project(project);
    const auto *alpha = require_agent(board, "alpha");
    require(alpha->daemon_runs_total == 129 && alpha->daemon_runs_scanned == 128,
        "detail must scan only the newest 128 daemon runs");
    require(alpha->daemon_providers.size() == 1
            && alpha->daemon_providers.front().name == "chatgpt.com"
            && alpha->daemon_providers.front().totals.api_calls == 128
            && alpha->daemon_providers.front().totals.input == 128,
        "same-mtime runs keep the 128 newest names and drop the oldest claude-p card");
}

void verify_session_windows_and_recent_calls(const fs::path &sandbox) {
    const auto project = sandbox / "session-windows";
    write_minimal_agent(project);
    write_file(project / ".lingtai/alpha/logs/events.jsonl",
        "{\"type\":\"psyche_molt\",\"ts\":1785888000}\n"
        "{\"type\":\"tool_call\",\"ts\":1785974400}\n"
        "{\"type\":\"psyche_molt\",\"ts\":1786320000}\n"
        "{\"type\":\"tool_call\",\"ts\":1787054400}\n");
    write_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-06T00:00:00Z\",\"input\":10,\"output\":2,"
        "\"thinking\":0,\"cached\":0,\"model\":\"m-last\"}\n"
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":20,\"output\":4,"
        "\"thinking\":0,\"cached\":0,\"model\":\"m-current\"}\n");

    const auto board = board_from_project(project);
    const auto *alpha = require_agent(board, "alpha");
    require(alpha->current_session.tokens.input == 20
            && alpha->current_session.tokens.api_calls == 1
            && alpha->current_session.tool_calls == 1
            && alpha->last_session.tokens.input == 10
            && alpha->last_session.tokens.api_calls == 1
            && alpha->last_session.tool_calls == 1,
        "current/last session API must follow the latest two psyche_molt windows");
    require(alpha->recent.size() == 2
            && alpha->recent.front().model == "m-current"
            && alpha->recent.back().model == "m-last",
        "recent calls must be newest first");
}

void verify_recent_keeps_last_hundred(const fs::path &sandbox) {
    const auto project = sandbox / "recent-100";
    write_minimal_agent(project);
    std::string ledger;
    ledger.reserve(120 * 80);
    for (int i = 0; i < 120; ++i) {
        ledger += "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":";
        ledger += std::to_string(i + 1);
        ledger += ",\"output\":1,\"model\":\"m";
        ledger += std::to_string(i);
        ledger += "\"}\n";
    }
    write_file(project / ".lingtai/alpha/logs/token_ledger.jsonl", ledger);
    const auto board = board_from_project(project);
    const auto *alpha = require_agent(board, "alpha");
    require(alpha->recent.size() == 100
            && alpha->recent.front().model == "m119"
            && alpha->recent.back().model == "m20",
        "recent calls keep the last 100 ledger rows, newest first");
}

void verify_session_skips_noise_event_lines(const fs::path &sandbox) {
    const auto project = sandbox / "noisy-events";
    write_minimal_agent(project);
    const auto path = project / ".lingtai/alpha/logs/events.jsonl";
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "noisy events parent must be created");
    auto stream = std::ofstream(path, std::ios::binary);
    const auto junk = std::string(64, 'x');
    for (int i = 0; i < 20000; ++i) {
        stream << "{\"type\":\"tool_result\",\"ts\":1,\"payload\":\"" << junk
               << "\"}\n";
    }
    stream << "{\"type\":\"psyche_molt\",\"ts\":1786320000}\n";
    stream << "{\"type\":\"tool_call\",\"ts\":1787054400}\n";
    require(stream.good(), "noisy events must be written");
    stream.close();
    write_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":9,\"output\":1,"
        "\"thinking\":0,\"cached\":0,\"model\":\"m-now\"}\n");
    const auto board = board_from_project(project);
    const auto *alpha = require_agent(board, "alpha");
    require(alpha->current_session.tokens.input == 9
            && alpha->current_session.tool_calls == 1
            && alpha->last_session.tokens.api_calls == 0,
        "session API must ignore bulky non-molt event lines");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <sandbox>\n";
        return 2;
    }
    const auto sandbox = fs::path(argv[1]);
    try {
        std::error_code error;
        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be reset");
        fs::create_directories(sandbox, error);
        require(!error, "sandbox must be created");
        verify_provider_derivation();
        verify_board_reads_network_sources(sandbox);
        verify_large_ledger_is_streamed(sandbox);
        verify_daemon_run_window(sandbox);
        verify_session_windows_and_recent_calls(sandbox);
        verify_recent_keeps_last_hundred(sandbox);
        verify_session_skips_noise_event_lines(sandbox);
        std::cout << "kanban model: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "kanban model: " << error.what() << '\n';
        return 1;
    }
}
