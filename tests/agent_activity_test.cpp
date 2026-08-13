#include "agent_activity.h"
#include "project_attachment.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::AgentActivityToolStatus;
using lingtai::desktop::ProjectAttachment;
using lingtai::desktop::attach_project;
using lingtai::desktop::read_agent_activity;

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
    require(stream.good(), "fixture must be appended: " + path.string());
}

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// Exact byte/type image of the fixture, so "reading never writes" is proven
// against the real tree rather than asserted.
std::map<std::string, std::string> tree_snapshot(const fs::path &root) {
    auto result = std::map<std::string, std::string>();
    if (!fs::exists(root)) return result;
    for (const auto &entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied)) {
        const auto key = entry.path().lexically_relative(root).generic_string();
        const auto status = entry.symlink_status();
        if (fs::is_symlink(status)) {
            result[key] = "symlink:" + fs::read_symlink(entry.path()).string();
        } else if (fs::is_directory(status)) {
            result[key] = "directory";
        } else if (fs::is_regular_file(status)) {
            result[key] = "file:" + read_file(entry.path());
        } else {
            result[key] = "other";
        }
    }
    return result;
}

fs::path events_path(const fs::path &project, std::string_view key) {
    return project / ".lingtai" / std::string(key) / "logs" / "events.jsonl";
}

std::string diary_line(
        std::string_view text,
        bool hidden = false,
        std::optional<std::string_view> visibility = std::nullopt,
        std::optional<double> ts = std::nullopt) {
    std::ostringstream out;
    out << R"({"type":"diary","text":")" << text << R"(")";
    if (hidden) out << R"(,"hidden":true)";
    if (visibility) out << R"(,"visibility":")" << *visibility << R"(")";
    if (ts) out << R"(,"ts":)" << *ts;
    out << "}\n";
    return out.str();
}

std::string tool_call_line(
        std::string_view name, std::string_view action, std::string_view call_id) {
    std::ostringstream out;
    out << R"({"type":"tool_call","tool_name":")" << name
        << R"(","tool_args":{"action":")" << action
        << R"("},"tool_call_id":")" << call_id << R"("})" << "\n";
    return out.str();
}

std::string tool_result_line(
        std::string_view call_id,
        std::string_view status,
        double elapsed_ms,
        std::string_view raw_result_marker) {
    std::ostringstream out;
    out << R"({"type":"tool_result","tool_call_id":")" << call_id
        << R"(","status":")" << status << R"(","elapsed_ms":)" << elapsed_ms
        << R"(,"result":")" << raw_result_marker << R"("})" << "\n";
    return out.str();
}

ProjectAttachment attach(const fs::path &project) {
    auto attached = attach_project(project);
    require(static_cast<bool>(attached),
        "project attachment must succeed: " + project.string());
    return std::move(*attached.attachment);
}

// Exact selected-Agent binding and descriptor containment: reading Agent A
// must never return Agent B's text, and an intermediate `logs` symlink must
// not expose an outside event or write anything.
void verify_selected_binding_and_containment(const fs::path &sandbox) {
    const auto project = sandbox / "binding";
    write_file(events_path(project, "agent-a"), diary_line("AGENT_A_DIARY"));
    write_file(events_path(project, "agent-b"), diary_line("AGENT_B_DIARY"));

    const auto attachment = attach(project);
    const auto project_before = tree_snapshot(project);
    const auto a = read_agent_activity(attachment, "agent-a");
    require(tree_snapshot(project) == project_before,
        "reading the selected Agent's activity must never write the project");
    require(a.available, "an existing readable log must be available");
    require(a.rows.size() == 1 && a.rows[0].text == "AGENT_A_DIARY",
        "reading Agent A must return only Agent A's own row");
    for (const auto &row : a.rows) {
        require(row.text.find("AGENT_B_DIARY") == std::string::npos,
            "Agent A's snapshot must never include Agent B's text");
    }

    std::error_code mkdir_error;
    fs::create_directories(project / ".lingtai" / "agent-c", mkdir_error);
    require(!mkdir_error, "agent-c directory must be created");
    const auto outside = sandbox / "binding-outside";
    write_file(outside / "events.jsonl", diary_line("OUTSIDE_SHOULD_NOT_APPEAR"));
    std::error_code link_error;
    fs::create_directory_symlink(
        outside, project / ".lingtai" / "agent-c" / "logs", link_error);
    require(!link_error, "symlinked logs fixture must be created");

    const auto outside_before = tree_snapshot(outside);
    const auto c = read_agent_activity(attachment, "agent-c");
    require(tree_snapshot(outside) == outside_before,
        "reading through a blocked intermediate symlink must never write "
        "outside state");
    require(!c.available,
        "a symlinked `logs` component must reduce to unavailable, not be "
        "followed");
    require(c.rows.empty(), "no row may come from outside the project");
}

// File byte order and the public allowlist: visible public diary text and a
// tool_call reduced/completed by its matching tool_result render in file
// order, while thinking/text_input/hidden diary/private diary/raw tool
// result/unknown types are absent, a finite ts does not suppress a row that
// lacks one, and a finite but out-of-`time_t`-range ts still renders the row
// with an empty display timestamp rather than being suppressed or undefined.
void verify_order_and_allowlist(const fs::path &sandbox) {
    const auto project = sandbox / "allowlist";
    const auto path = events_path(project, "agent");
    std::string content;
    content += diary_line("PUBLIC_DIARY_1");
    content += R"({"type":"thinking","text":"SHOULD_NOT_APPEAR_THINKING"})"
        "\n";
    content += R"({"type":"text_input","text":"SHOULD_NOT_APPEAR_TEXT_INPUT"})"
        "\n";
    content += diary_line("SHOULD_NOT_APPEAR_HIDDEN_DIARY", true);
    content += diary_line(
        "SHOULD_NOT_APPEAR_PRIVATE_DIARY", false, "private");
    content += tool_call_line("shell", "ls", "call-1");
    content += tool_result_line(
        "call-1", "ok", 42.0, "RAW_RESULT_SHOULD_NOT_APPEAR");
    content += R"({"type":"llm_call","model":"x"})" "\n";
    content += diary_line("PUBLIC_DIARY_2", false, "public", 1765600000.0);
    content += diary_line("PUBLIC_DIARY_3_OUT_OF_RANGE_TS", false, "public", 1e300);
    write_file(path, content);

    const auto attachment = attach(project);
    const auto project_before = tree_snapshot(project);
    const auto snapshot = read_agent_activity(attachment, "agent");
    require(tree_snapshot(project) == project_before,
        "an ordinary read must never write the project");
    require(snapshot.available, "an ordinary readable log must be available");
    require(snapshot.rows.size() == 4,
        "only the allowlisted diary/tool rows may render");
    require(!snapshot.rows[0].is_tool
            && snapshot.rows[0].text == "PUBLIC_DIARY_1",
        "file order: the first public diary row renders first");
    require(snapshot.rows[1].is_tool
            && snapshot.rows[1].tool_name == "shell"
            && snapshot.rows[1].tool_action == "ls"
            && snapshot.rows[1].tool_status == AgentActivityToolStatus::success
            && snapshot.rows[1].tool_elapsed_ms == 42.0,
        "the tool_call row must be completed by its matching tool_result");
    require(!snapshot.rows[2].is_tool
            && snapshot.rows[2].text == "PUBLIC_DIARY_2",
        "file order: the second public diary row renders before the final "
        "out-of-range-timestamp row");
    require(!snapshot.rows[2].timestamp_text.empty(),
        "a finite numeric ts renders as a bounded display timestamp");
    require(snapshot.rows[0].timestamp_text.empty(),
        "a missing ts must not suppress an otherwise valid row");
    require(!snapshot.rows[3].is_tool
            && snapshot.rows[3].text == "PUBLIC_DIARY_3_OUT_OF_RANGE_TS",
        "file order: a finite but out-of-time_t-range ts must still render "
        "its row, last");
    require(snapshot.rows[3].timestamp_text.empty(),
        "a finite ts outside time_t range must yield an empty display "
        "timestamp, not suppress the row or invoke undefined behavior");

    for (const auto &row : snapshot.rows) {
        require(row.text.find("SHOULD_NOT_APPEAR") == std::string::npos
                && row.tool_name.find("SHOULD_NOT_APPEAR") == std::string::npos,
            "thinking/text_input/hidden/private diary rows must never render");
        require(row.text.find("RAW_RESULT") == std::string::npos
                && row.tool_action.find("RAW_RESULT") == std::string::npos,
            "raw tool_result content must never render");
    }
    require(snapshot.skipped == 0,
        "unknown types and filtered-but-valid rows are not counted as skipped");
}

// A partial final row is absent before its closing LF and appears on the
// next stateless snapshot once the write completes it.
void verify_partial_tail_completes_on_next_snapshot(const fs::path &sandbox) {
    const auto project = sandbox / "partial";
    const auto path = events_path(project, "agent");
    write_file(path, diary_line("BEFORE_PARTIAL"));
    append_file(path, R"({"type":"diary","text":"PARTIAL_ROW")");

    const auto attachment = attach(project);
    const auto first = read_agent_activity(attachment, "agent");
    require(first.available,
        "a log with a trailing partial line is still available");
    require(first.rows.size() == 1 && first.rows[0].text == "BEFORE_PARTIAL",
        "an unterminated final row must be absent from the snapshot");
    require(first.skipped == 0, "a merely incomplete tail is not a malformed row");

    append_file(path, "}\n");
    const auto second = read_agent_activity(attachment, "agent");
    require(second.rows.size() == 2 && second.rows[1].text == "PARTIAL_ROW",
        "completing the row on disk must make it appear on the next snapshot");
}

// A large older prefix cannot make the actual read exceed the fixed 512 KiB
// suffix bound, and the newest row remains available regardless.
void verify_bounded_suffix_ignores_large_prefix(const fs::path &sandbox) {
    const auto project = sandbox / "bounded";
    const auto path = events_path(project, "agent");
    std::string content = diary_line("OLDEST_MARKER_MUST_NOT_APPEAR");
    const auto filler = diary_line(std::string(4000, 'f'));
    while (content.size() < (std::size_t{1} << 20)) content += filler;
    content += diary_line("NEWEST_MARKER_WITHIN_WINDOW");
    write_file(path, content);

    const auto attachment = attach(project);
    const auto snapshot = read_agent_activity(attachment, "agent");
    require(snapshot.available, "a large log must still be available");
    require(!snapshot.rows.empty()
            && snapshot.rows.back().text == "NEWEST_MARKER_WITHIN_WINDOW",
        "the newest row must always be present");
    for (const auto &row : snapshot.rows) {
        require(row.text != "OLDEST_MARKER_MUST_NOT_APPEAR",
            "a marker written well before the fixed suffix bound must never "
            "be read: the bound applies to the actual read, not just parsing");
    }
    require(snapshot.rows.size() <= 100,
        "the projected window is capped at the latest 100 rows");
}

// Malformed complete data coexists with later valid rows and produces only
// the one coarse skip signal; unknown types are not double-counted here.
void verify_malformed_rows_are_skipped_with_valid_neighbors(
        const fs::path &sandbox) {
    const auto project = sandbox / "malformed";
    const auto path = events_path(project, "agent");
    std::string content;
    content += diary_line("VALID_BEFORE_MALFORMED");
    content += "{\"type\":\"diary\",\"text\":\n"; // malformed but LF-terminated
    content += "[1,2,3]\n";                        // complete, but not an object
    content += std::string("\xFF\xFE not valid utf-8\n");
    content += diary_line("VALID_AFTER_MALFORMED");
    write_file(path, content);

    const auto attachment = attach(project);
    const auto snapshot = read_agent_activity(attachment, "agent");
    require(snapshot.available,
        "a log with malformed neighbors is still available");
    require(snapshot.rows.size() == 2
            && snapshot.rows[0].text == "VALID_BEFORE_MALFORMED"
            && snapshot.rows[1].text == "VALID_AFTER_MALFORMED",
        "malformed complete rows must never hide a valid neighbor");
    require(snapshot.skipped == 3,
        "each malformed/non-object/invalid-UTF-8 complete row increments "
        "the one coarse skip count");
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

        verify_selected_binding_and_containment(sandbox);
        verify_order_and_allowlist(sandbox);
        verify_partial_tail_completes_on_next_snapshot(sandbox);
        verify_bounded_suffix_ignores_large_prefix(sandbox);
        verify_malformed_rows_are_skipped_with_valid_neighbors(sandbox);

        fs::remove_all(sandbox, error);
        require(!error, "sandbox must be removed");
    } catch (const std::exception &failure) {
        std::cerr << "agent_activity: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "agent_activity: all checks passed\n";
    return 0;
}
