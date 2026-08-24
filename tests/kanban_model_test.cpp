#include "kanban_model.h"
#include "project_attachment.h"

#include <fcntl.h>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::KanbanBoardColumn;
using lingtai::desktop::KanbanSnapshotIndex;
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

void append_file(const fs::path &path, std::string_view bytes) {
    auto stream = std::ofstream(path, std::ios::binary | std::ios::app);
    stream << bytes;
    require(stream.good(), "fixture append must succeed: " + path.string());
}

void sqlite_exec(const fs::path &path, const char *sql) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "sqlite fixture parent must be created");
    sqlite3 *db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK,
        "sqlite fixture must open");
    char *message = nullptr;
    const auto status = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    const auto detail = message ? std::string(message) : std::string();
    sqlite3_free(message);
    sqlite3_close(db);
    require(status == SQLITE_OK, "sqlite fixture SQL must succeed: " + detail);
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
    require(board.running_daemons == 1 && board.total_mails == 0,
        "daemon counts are included; inbox mail totals are skipped");
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

void verify_incremental_snapshot_index(const fs::path &sandbox) {
    const auto project = sandbox / "incremental-index";
    write_minimal_agent(project);
    write_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":10,\"output\":2,"
        "\"model\":\"m0\"}\n");
    write_file(project / ".lingtai/alpha/logs/events.jsonl",
        "{\"type\":\"tool_call\",\"ts\":1787054400}\n");
    write_file(project / ".lingtai/alpha/history/chat_history.jsonl",
        "{\"role\":\"user\",\"content\":\"first\"}\n");
    write_file(project / ".lingtai/alpha/delegates/ledger.jsonl", "");
    const auto attached = attach_project(project);
    require(attached.attachment.has_value(), "incremental fixture must attach");
    auto snapshot = project_agents(*attached.attachment);
    KanbanSnapshotIndex index;
    const auto cold = index.refresh(*attached.attachment, snapshot);
    require(cold.changed && cold.metrics.full_agent_rebuilds == 2,
        "cold refresh builds one complete generation");
    const auto repeated_full = index.refresh(*attached.attachment, snapshot, true);
    require(repeated_full.metrics.payload_opens > 0
            && repeated_full.metrics.payload_bytes > 0,
        "a repeated full-reader generation deterministically reopens payloads");
    const auto unchanged_one = index.refresh(*attached.attachment, snapshot);
    const auto unchanged_two = index.refresh(*attached.attachment, snapshot);
    require(!unchanged_one.changed && !unchanged_two.changed
            && unchanged_one.metrics.payload_opens == 0
            && unchanged_two.metrics.payload_opens == 0
            && unchanged_one.metrics.daemon_enumerations == 0
            && unchanged_two.metrics.daemon_enumerations == 0
            && unchanged_one.metrics.full_agent_rebuilds == 0,
        "unchanged warm cycles perform metadata checks only");

    write_file(project / ".lingtai/alpha/init.json",
        R"({"model":"m-static","provider":"test","mcp":{"fs":{}}})");
    const auto static_update = index.refresh(*attached.attachment, snapshot);
    require(static_update.metrics.payload_opens == 1
            && static_update.metrics.full_agent_rebuilds == 0
            && require_agent(static_update.board, "alpha")->tokens.input == 10
            && require_agent(static_update.board, "alpha")->mcp_names.size() == 1,
        "one changed small source is reread without reopening growing payloads");

    append_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T13:00:00Z\",\"input\":7,\"output\":3,"
        "\"model\":\"m1\"}\n");
    append_file(project / ".lingtai/alpha/logs/events.jsonl",
        "{\"type\":\"tool_call\",\"ts\":1787054401}\n");
    append_file(project / ".lingtai/alpha/history/chat_history.jsonl",
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_call\","
        "\"name\":\"read\"}]}\n");
    append_file(project / ".lingtai/alpha/delegates/ledger.jsonl",
        "{\"event\":\"avatar\",\"name\":\"Beta\","
        "\"working_dir\":\"beta\"}\n");
    const auto appended = index.refresh(*attached.attachment, snapshot);
    const auto *alpha = require_agent(appended.board, "alpha");
    require(appended.changed && appended.metrics.full_agent_rebuilds == 0
            && appended.metrics.appended_lines == 4
            && alpha->tokens.input == 17 && alpha->tokens.output == 5
            && alpha->current_session.tool_calls == 2
            && alpha->context_stats.entries == 2
            && alpha->context_stats.tool_calls == 1
            && appended.board.tree_lines.size() >= 2,
        "complete token/event/chat/delegate appends update once from appended rows");

    append_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T14:00:00Z\",\"input\":11");
    const auto partial = index.refresh(*attached.attachment, snapshot);
    require(require_agent(partial.board, "alpha")->tokens.input == 17,
        "a partial trailing JSONL row is retained but not projected");
    append_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        ",\"output\":1,\"model\":\"m2\"}\n");
    const auto completed = index.refresh(*attached.attachment, snapshot);
    require(require_agent(completed.board, "alpha")->tokens.input == 28
            && completed.metrics.appended_lines == 1,
        "the retained partial row is projected exactly once when completed");
    const auto full_after_appends = board_from_project(project);
    const auto *incremental_alpha = require_agent(completed.board, "alpha");
    const auto *full_alpha = require_agent(full_after_appends, "alpha");
    require(incremental_alpha->tokens.input == full_alpha->tokens.input
            && incremental_alpha->tokens.output == full_alpha->tokens.output
            && incremental_alpha->tokens.api_calls == full_alpha->tokens.api_calls
            && incremental_alpha->current_session.tokens.input
                == full_alpha->current_session.tokens.input
            && incremental_alpha->current_session.tool_calls
                == full_alpha->current_session.tool_calls
            && incremental_alpha->context_stats.entries
                == full_alpha->context_stats.entries
            && incremental_alpha->context_stats.tool_calls
                == full_alpha->context_stats.tool_calls
            && completed.board.tree_lines == full_after_appends.tree_lines,
        "incremental append projection matches full-reader semantics");

    write_file(project / ".lingtai/alpha/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T15:00:00Z\",\"input\":5,\"output\":1,"
        "\"model\":\"reset\"}\n");
    const auto reset = index.refresh(*attached.attachment,
        project_agents(*attached.attachment));
    require(reset.metrics.full_agent_rebuilds == 1
            && require_agent(reset.board, "alpha")->tokens.input == 5,
        "replacement rebuilds only the affected Agent without mixing totals");
}

void verify_rebuild_read_to_cursor_race(const fs::path &sandbox) {
    const auto project = sandbox / "rebuild-read-cursor-race";
    write_minimal_agent(project);
    write_file(project / ".lingtai/beta/.agent.json",
        R"({"agent_id":"b001","agent_name":"beta","address":"beta","state":"idle"})");
    const auto ledger = project / ".lingtai/alpha/logs/token_ledger.jsonl";
    const auto events = project / ".lingtai/alpha/logs/events.jsonl";
    const auto chat = project / ".lingtai/alpha/history/chat_history.jsonl";
    const auto delegates = project / ".lingtai/alpha/delegates/ledger.jsonl";
    write_file(ledger,
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":10,"
        "\"output\":1,\"model\":\"before\"}\n");
    write_file(delegates, "");
    write_file(events, "");
    write_file(chat, "");
    const auto attached = attach_project(project);
    require(attached.attachment.has_value(), "race fixture must attach");
    const auto snapshot = project_agents(*attached.attachment);
    KanbanSnapshotIndex index;
    auto appended = false;
    index.set_rebuild_read_complete_hook(
        [&](const lingtai::desktop::AgentRow &row) {
            if (row.directory_key != "alpha" || appended) return;
            appended = true;
            append_file(ledger,
                "{\"ts\":\"2026-08-18T13:00:00Z\",\"input\":7,"
                "\"output\":1,\"model\":\"during\"}\n");
            append_file(events,
                "{\"type\":\"tool_call\",\"ts\":1787054401}\n");
            append_file(chat,
                "{\"role\":\"user\",\"content\":\"during\"}\n");
            append_file(delegates,
                "{\"event\":\"avatar\",\"name\":\"Beta\","
                "\"working_dir\":\"beta\"}\n");
        });
    const auto raced = index.refresh(*attached.attachment, snapshot);
    require(raced.changed && raced.follow_up,
        "a row appended after the full read must not claim authoritative steady state");
    index.set_rebuild_read_complete_hook({});
    const auto repaired = index.refresh(*attached.attachment, snapshot);
    auto full = board_from_project(project);
    require(repaired.metrics.full_agent_rebuilds == 1
            && require_agent(repaired.board, "alpha")->tokens.input
                == require_agent(full, "alpha")->tokens.input
            && require_agent(repaired.board, "alpha")
                ->current_session.tool_calls
                == require_agent(full, "alpha")->current_session.tool_calls
            && require_agent(repaired.board, "alpha")->context_stats.entries
                == require_agent(full, "alpha")->context_stats.entries
            && repaired.board.tree_lines == full.tree_lines,
        "the immediate repair must restore all growing-source parity once");
    const auto settled = index.refresh(*attached.attachment, snapshot);
    require(!settled.changed && !settled.follow_up
            && settled.metrics.full_agent_rebuilds == 0,
        "a repaired generation must remain steady without another follow-up");

    write_file(project / ".lingtai/human/logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T14:00:00Z\",\"input\":99}\n");
    const auto human_source = index.refresh(*attached.attachment, snapshot);
    require(!human_source.changed && !human_source.follow_up,
        "tracked-looking human files must not create a permanent follow-up loop");

    write_file(ledger,
        "{\"ts\":\"2026-08-18T15:00:00Z\",\"input\":5,"
        "\"output\":1,\"model\":\"reset-before\"}\n");
    auto reset_append = false;
    index.set_rebuild_read_complete_hook(
        [&](const lingtai::desktop::AgentRow &row) {
            if (row.directory_key != "alpha" || reset_append) return;
            reset_append = true;
            append_file(ledger,
                "{\"ts\":\"2026-08-18T16:00:00Z\",\"input\":3,"
                "\"output\":1,\"model\":\"reset-during\"}\n");
        });
    const auto reset_raced = index.refresh(*attached.attachment, snapshot);
    require(reset_raced.metrics.full_agent_rebuilds == 1
            && reset_raced.follow_up,
        "movement during an affected-Agent rebuild must request another repair");
    index.set_rebuild_read_complete_hook({});
    const auto reset_repaired = index.refresh(*attached.attachment, snapshot);
    full = board_from_project(project);
    require(reset_repaired.metrics.full_agent_rebuilds == 1
            && require_agent(reset_repaired.board, "alpha")->tokens.input
                == require_agent(full, "alpha")->tokens.input
            && reset_repaired.board.tree_lines == full.tree_lines,
        "the affected-Agent race must converge without missing or duplicate rows");
    const auto reset_settled = index.refresh(*attached.attachment, snapshot);
    require(!reset_settled.changed && !reset_settled.follow_up,
        "the affected-Agent repair must reach stable steady state");
}

void verify_capture_incapable_cursor_waits_for_change(
        const fs::path &sandbox) {
    const auto project = sandbox / "capture-incapable-cursor";
    write_minimal_agent(project);
    const auto ledger = project / ".lingtai/alpha/logs/token_ledger.jsonl";
    auto incomplete = std::string(
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":13,"
        "\"output\":2,\"model\":\"large\",\"padding\":\"");
    incomplete.append(1024U * 1024U + 1U, 'x');
    write_file(ledger, incomplete);
    const auto attached = attach_project(project);
    require(attached.attachment.has_value(),
        "capture-incapable fixture must attach");
    const auto snapshot = project_agents(*attached.attachment);
    KanbanSnapshotIndex index;

    const auto cold = index.refresh(*attached.attachment, snapshot);
    require(!cold.follow_up
            && require_agent(cold.board, "alpha")->tokens.api_calls == 0,
        "an unchanged capture-incapable trailing row must not request repair");
    const auto unchanged = index.refresh(*attached.attachment, snapshot);
    require(!unchanged.changed && !unchanged.follow_up
            && unchanged.metrics.full_agent_rebuilds == 0,
        "an unchanged capture-incapable cursor must not chain rebuilds");

    append_file(ledger, "\"}\n");
    const auto completed = index.refresh(*attached.attachment, snapshot);
    const auto full = board_from_project(project);
    require(completed.changed && !completed.follow_up
            && completed.metrics.full_agent_rebuilds == 1
            && require_agent(completed.board, "alpha")->tokens.input == 13
            && require_agent(completed.board, "alpha")->tokens.input
                == require_agent(full, "alpha")->tokens.input,
        "a changed capture-incapable source must rebuild once to full parity");
    const auto settled = index.refresh(*attached.attachment, snapshot);
    require(!settled.changed && !settled.follow_up
            && settled.metrics.full_agent_rebuilds == 0,
        "the completed source must remain at stable steady state");
}

void verify_daemon_index_is_incremental(const fs::path &sandbox) {
    const auto project = sandbox / "daemon-index";
    write_minimal_agent(project);
    const auto daemons = project / ".lingtai/alpha/daemons";
    constexpr auto stamp = static_cast<std::time_t>(1'700'000'000);
    for (int i = 0; i < 300; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "run-%03d", i);
        write_file(daemons / name / "daemon.json",
            R"({"state":"done","backend":"chatgpt.com",)"
            R"("cli_tokens":{"input":1,"output":1,"calls":1}})");
        set_mtime(daemons / name, stamp);
    }
    const auto attached = attach_project(project);
    require(attached.attachment.has_value(), "daemon index fixture must attach");
    auto snapshot = project_agents(*attached.attachment);
    KanbanSnapshotIndex index;
    const auto cold = index.refresh(*attached.attachment, snapshot);
    require(require_agent(cold.board, "alpha")->daemon_runs_total == 300
            && require_agent(cold.board, "alpha")->daemon_runs_scanned == 128,
        "cold daemon inventory preserves total/newest-128 semantics");
    const auto unchanged = index.refresh(*attached.attachment, snapshot);
    require(unchanged.metrics.daemon_enumerations == 0
            && unchanged.metrics.daemon_records_opened == 0,
        "unchanged daemon inventory reopens no historical record");
    write_file(daemons / "run-300/daemon.json",
        R"({"state":"running","backend":"claude-p",)"
        R"("cli_tokens":{"input":2,"output":1,"calls":1}})");
    set_mtime(daemons / "run-300", stamp + 1);
    const auto added = index.refresh(*attached.attachment, snapshot);
    require(require_agent(added.board, "alpha")->daemon_runs_total == 301
            && added.metrics.daemon_enumerations == 1
            && added.metrics.daemon_records_opened == 1,
        "one new daemon run reads only its new record (opened="
            + std::to_string(added.metrics.daemon_records_opened) + ")");
    write_file(daemons / "run-300/daemon.json",
        R"({"state":"done","finished_at":"2026-08-24T00:00:00Z",)"
        R"("backend":"claude-p","cli_tokens":{"input":3,"output":1,"calls":1}})");
    const auto terminal = index.refresh(*attached.attachment, snapshot);
    require(terminal.metrics.daemon_enumerations == 0
            && terminal.metrics.daemon_records_opened == 1
            && require_agent(terminal.board, "alpha")->daemons.running == 0,
        "a nonterminal record update rereads only that changed daemon.json");

    std::error_code replacement_error;
    fs::remove_all(daemons / "run-300", replacement_error);
    require(!replacement_error, "daemon replacement fixture must remove old run");
    write_file(daemons / "run-300/daemon.json",
        R"({"state":"running","backend":"replacement",)"
        R"("cli_tokens":{"input":4,"output":1,"calls":1}})");
    set_mtime(daemons / "run-300", stamp + 2);
    const auto replaced = index.refresh(*attached.attachment, snapshot);
    require(replaced.metrics.daemon_enumerations == 1
            && replaced.metrics.daemon_records_opened == 1
            && require_agent(replaced.board, "alpha")->daemons.running == 1,
        "same-name run-directory replacement invalidates the cached generation");

    fs::remove_all(daemons / "run-300", replacement_error);
    require(!replacement_error, "daemon removal fixture must remove run");
    const auto removed = index.refresh(*attached.attachment, snapshot);
    require(removed.metrics.daemon_enumerations == 1
            && removed.metrics.daemon_records_opened == 0
            && require_agent(removed.board, "alpha")->daemon_runs_total == 300,
        "run removal updates membership without reopening historical records");
}

void verify_incremental_parity_for_malformed_and_unsafe(
        const fs::path &sandbox) {
    const auto project = sandbox / "unsafe-parity";
    write_minimal_agent(project);
    write_file(project / "outside-ledger.jsonl",
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":999}\n");
    std::error_code link_error;
    fs::create_directories(project / ".lingtai/alpha/logs", link_error);
    require(!link_error, "unsafe parity logs directory must be created");
    fs::create_symlink(project / "outside-ledger.jsonl",
        project / ".lingtai/alpha/logs/token_ledger.jsonl", link_error);
    require(!link_error, "unsafe parity token symlink must be created");
    write_file(project / ".lingtai/alpha/logs/events.jsonl",
        "not-json\n{\"type\":\"tool_call\",\"ts\":1787054400}\n");
    write_file(project / ".lingtai/alpha/history/chat_history.jsonl",
        "{broken}\n{\"role\":\"user\",\"content\":\"safe\"}\n");
    fs::create_directories(project / ".lingtai/alpha/daemons", link_error);
    require(!link_error, "unsafe parity daemons directory must be created");
    fs::create_directory_symlink(project,
        project / ".lingtai/alpha/daemons/escaped", link_error);
    require(!link_error, "unsafe parity daemon symlink must be created");
    const auto attached = attach_project(project);
    require(attached.attachment.has_value(), "unsafe parity fixture must attach");
    const auto snapshot = project_agents(*attached.attachment);
    const auto full = read_kanban_board(*attached.attachment, snapshot);
    KanbanSnapshotIndex index;
    const auto incremental = index.refresh(*attached.attachment, snapshot);
    const auto *full_alpha = require_agent(full, "alpha");
    const auto *incremental_alpha = require_agent(incremental.board, "alpha");
    require(full_alpha->tokens.input == 0
            && incremental_alpha->tokens.input == 0
            && full_alpha->current_session.tool_calls
                == incremental_alpha->current_session.tool_calls
            && full_alpha->context_stats.entries
                == incremental_alpha->context_stats.entries
            && full_alpha->daemon_runs_total
                == incremental_alpha->daemon_runs_total,
        "incremental cold projection matches malformed/unsafe full semantics");
}

void verify_sqlite_session_freshness(const fs::path &sandbox) {
    const auto project = sandbox / "sqlite-freshness";
    write_minimal_agent(project);
    const auto database = project / ".lingtai/alpha/logs/log.sqlite";
    sqlite_exec(database,
        "CREATE TABLE events(type TEXT, ts REAL, scope TEXT, source_kind TEXT);"
        "INSERT INTO events VALUES('psyche_molt', 1786320000, NULL, NULL);"
        "INSERT INTO events VALUES('tool_call', 1787054400, NULL, NULL);");
    const auto attached = attach_project(project);
    require(attached.attachment.has_value(), "sqlite fixture must attach");
    const auto snapshot = project_agents(*attached.attachment);
    KanbanSnapshotIndex index;
    auto inserted_during_rebuild = false;
    index.set_rebuild_read_complete_hook(
        [&](const lingtai::desktop::AgentRow &row) {
            if (row.directory_key != "alpha" || inserted_during_rebuild) return;
            inserted_during_rebuild = true;
            sqlite_exec(database,
                "INSERT INTO events VALUES('tool_call', 1787054401, NULL, NULL);");
        });
    const auto cold = index.refresh(*attached.attachment, snapshot);
    require(cold.follow_up
            && require_agent(cold.board, "alpha")->current_session.tool_calls == 1,
        "SQLite movement after the full read must request a repair");
    index.set_rebuild_read_complete_hook({});
    const auto repaired = index.refresh(*attached.attachment, snapshot);
    require(repaired.metrics.full_agent_rebuilds == 1
            && require_agent(repaired.board, "alpha")
                ->current_session.tool_calls == 2,
        "SQLite movement must repair even when the database stops changing");
    sqlite_exec(database,
        "INSERT INTO events VALUES('tool_call', 1787054402, NULL, NULL);");
    const auto tool_append = index.refresh(*attached.attachment, snapshot);
    require(tool_append.metrics.full_agent_rebuilds == 0
            && tool_append.metrics.payload_opens == 1
            && require_agent(tool_append.board, "alpha")
                ->current_session.tool_calls == 3,
        "sqlite append performs one bounded query without historical rebuild");
    sqlite_exec(database,
        "INSERT INTO events VALUES('psyche_molt', 1787140800, NULL, NULL);");
    const auto boundary = index.refresh(*attached.attachment, snapshot);
    require(boundary.metrics.full_agent_rebuilds == 1
            && !require_agent(boundary.board, "alpha")->molt_times_ms.empty(),
        "a new session boundary takes the rare affected-Agent repartition path");
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
        verify_incremental_snapshot_index(sandbox);
        verify_capture_incapable_cursor_waits_for_change(sandbox);
        verify_rebuild_read_to_cursor_race(sandbox);
        verify_daemon_index_is_incremental(sandbox);
        verify_incremental_parity_for_malformed_and_unsafe(sandbox);
        verify_sqlite_session_freshness(sandbox);
        std::cout << "kanban model: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "kanban model: " << error.what() << '\n';
        return 1;
    }
}
