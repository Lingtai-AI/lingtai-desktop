#include "conversation_session.h"

#include "posix_descriptor_primitives.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {

bool advance_conversation_session_revision(
        const std::vector<ConversationSessionEntry> &before,
        const std::vector<ConversationSessionEntry> &after,
        std::uint64_t &revision) noexcept {
    if (before == after) return false;
    ++revision;
    return true;
}

namespace {

namespace posix = posix_internal;

constexpr off_t kMaxSessionTailBytes = 256 << 10; // 256KB tail
constexpr std::size_t kMaxSessionEntries = 120;
constexpr int kThinkingPreviewLimit = 120;
constexpr int kToolCallSummaryPreviewLimit = 240;
constexpr int kMaxStoredBodyBytes = 1200;
constexpr int kMaxStoredReasoningBytes = 240;

[[nodiscard]] std::int64_t json_int64(const QJsonValue &value) {
    if (value.isDouble()) {
        return static_cast<std::int64_t>(value.toDouble());
    }
    if (value.isString()) {
        auto ok = false;
        const auto parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : 0;
    }
    return 0;
}

[[nodiscard]] std::string json_string(const QJsonObject &object, const char *key) {
    const auto value = object.value(QLatin1StringView(key));
    return value.isString() ? value.toString().toStdString() : std::string{};
}

[[nodiscard]] std::string timestamp_from_event(const QJsonObject &object) {
    const auto ts = object.value(QLatin1StringView("ts"));
    if (ts.isDouble()) {
        const auto seconds = static_cast<std::int64_t>(ts.toDouble());
        const auto millis = static_cast<std::int64_t>(
            std::llround((ts.toDouble() - static_cast<double>(seconds)) * 1000.0));
        std::tm utc {};
        const auto epoch = static_cast<time_t>(seconds);
        gmtime_r(&epoch, &utc);
        char buffer[32] = {};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%04d-%02d-%02dT%02d:%02d:%02d",
            utc.tm_year + 1900,
            utc.tm_mon + 1,
            utc.tm_mday,
            utc.tm_hour,
            utc.tm_min,
            utc.tm_sec);
        auto result = std::string(buffer);
        if (millis > 0) {
            char fractional[8] = {};
            std::snprintf(fractional, sizeof(fractional), ".%03lld",
                static_cast<long long>(millis));
            result.append(fractional);
        }
        result.append("Z");
        return result;
    }
    return json_string(object, "timestamp");
}

[[nodiscard]] std::int64_t cache_miss(
        std::int64_t cached, std::int64_t input) noexcept {
    const auto miss = input - cached;
    return miss < 0 ? 0 : miss;
}

[[nodiscard]] std::string format_cache_rate(
        std::int64_t cached, std::int64_t input) {
    if (input <= 0) {
        return "—";
    }
    char buffer[32] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.1f%%",
        100.0 * static_cast<double>(cached) / static_cast<double>(input));
    return buffer;
}

[[nodiscard]] std::string humanize_token_count(std::int64_t count) {
    if (count < 0) {
        return humanize_token_count(-count);
    }
    char buffer[32] = {};
    if (count >= 1'000'000) {
        std::snprintf(buffer, sizeof(buffer), "%.1fM",
            static_cast<double>(count) / 1'000'000.0);
        return buffer;
    }
    if (count >= 1'000) {
        std::snprintf(buffer, sizeof(buffer), "%.1fk",
            static_cast<double>(count) / 1'000.0);
        return buffer;
    }
    return std::to_string(count);
}

[[nodiscard]] std::string first_line(std::string_view text) {
    const auto end = text.find_first_of("\r\n");
    return std::string(end == std::string_view::npos ? text : text.substr(0, end));
}

[[nodiscard]] std::string truncate_runes(
        std::string_view text, int limit) {
    if (limit <= 0 || static_cast<int>(text.size()) <= limit) {
        return std::string(text);
    }
    return std::string(text.substr(0, static_cast<std::size_t>(limit))) + "…";
}

[[nodiscard]] QJsonObject tool_args_without_reasoning(const QJsonObject &args) {
    auto rest = QJsonObject();
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() != QLatin1StringView("_reasoning")) {
            rest.insert(it.key(), it.value());
        }
    }
    return rest;
}

[[nodiscard]] std::string render_ltp_tool_params(const QJsonObject &args) {
    auto rest = QJsonObject();
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() == QLatin1StringView("action")
                || it.key() == QLatin1StringView("_reasoning")) {
            continue;
        }
        if (it.key() == QLatin1StringView("input") && it.value().isObject()) {
            const auto inner = it.value().toObject();
            for (auto inner_it = inner.begin(); inner_it != inner.end(); ++inner_it) {
                rest.insert(inner_it.key(), inner_it.value());
            }
            continue;
        }
        rest.insert(it.key(), it.value());
    }
    if (rest.isEmpty()) {
        return {};
    }
    return QJsonDocument(rest).toJson(QJsonDocument::Compact).toStdString();
}

[[nodiscard]] std::string tool_call_body(const QJsonObject &object) {
    const auto name = json_string(object, "tool_name");
    const auto args_value = object.value(QLatin1StringView("tool_args"));
    if (args_value.isString()) {
        return name + "(" + args_value.toString().toStdString() + ")";
    }
    if (args_value.isObject()) {
        const auto args = args_value.toObject();
        const auto action = json_string(args, "action");
        if (!action.empty()) {
            if (const auto params = render_ltp_tool_params(args); !params.empty()) {
                return name + "." + action + "(" + params + ")";
            }
            return name + "." + action;
        }
        const auto rest = tool_args_without_reasoning(args);
        return name + "("
            + QJsonDocument(rest).toJson(QJsonDocument::Compact).toStdString()
            + ")";
    }
    return name + "()";
}

[[nodiscard]] std::string display_tool_result_value(const QJsonValue &result) {
    // Compact only — indented dumps of large tool payloads froze the chat
    // rebuild when verbose mode interleaved dozens of results.
    if (result.isString()) {
        return truncate_runes(result.toString().toStdString(), kMaxStoredBodyBytes);
    }
    if (result.isArray()) {
        return truncate_runes(
            QJsonDocument(result.toArray())
                .toJson(QJsonDocument::Compact)
                .toStdString(),
            kMaxStoredBodyBytes);
    }
    if (result.isObject()) {
        return truncate_runes(
            QJsonDocument(result.toObject())
                .toJson(QJsonDocument::Compact)
                .toStdString(),
            kMaxStoredBodyBytes);
    }
    return {};
}

[[nodiscard]] std::string tool_result_body(const QJsonObject &object) {
    const auto name = json_string(object, "tool_name");
    const auto status = json_string(object, "status");
    auto elapsed = std::string{};
    const auto elapsed_ms = json_int64(object.value(QLatin1StringView("elapsed_ms")));
    if (elapsed_ms > 0) {
        elapsed = " " + std::to_string(elapsed_ms) + "ms";
    }
    auto header = name + " → " + (status.empty() ? "?" : status) + elapsed;

    const auto result = object.value(QLatin1StringView("result"));
    if (result.isUndefined() || result.isNull()) {
        return header;
    }
    const auto body = display_tool_result_value(result);
    if (body.empty()) {
        return header;
    }
    return header + "\nresult: " + body;
}

[[nodiscard]] bool is_verbose_event_type(std::string_view type) {
    return type == "thinking"
        || type == "tool_call"
        || type == "tool_result"
        || type == "llm_response";
}

[[nodiscard]] std::string extract_event_body(
        const QJsonObject &object, const std::string &type) {
    if (type == "thinking") {
        return json_string(object, "text");
    }
    if (type == "tool_call") {
        return tool_call_body(object);
    }
    if (type == "tool_result") {
        return tool_result_body(object);
    }
    return {};
}

[[nodiscard]] bool is_api_grouped_type(std::string_view type) {
    return type == "thinking" || type == "tool_call" || type == "tool_result";
}

} // namespace

ConversationVerboseLevel cycle_conversation_verbose_level(
        ConversationVerboseLevel level) noexcept {
    switch (level) {
    case ConversationVerboseLevel::off:
        return ConversationVerboseLevel::thinking;
    case ConversationVerboseLevel::thinking:
        return ConversationVerboseLevel::extended;
    case ConversationVerboseLevel::extended:
        return ConversationVerboseLevel::off;
    }
    return ConversationVerboseLevel::off;
}

const char *conversation_verbose_level_label(
        ConversationVerboseLevel level) noexcept {
    switch (level) {
    case ConversationVerboseLevel::off:
        return "off";
    case ConversationVerboseLevel::thinking:
        return "thinking";
    case ConversationVerboseLevel::extended:
        return "extended";
    }
    return "off";
}

std::string format_token_usage_footer(const SessionTokenUsage &usage) {
    if (usage.input == 0 && usage.output == 0 && usage.cached == 0) {
        return {};
    }
    auto line = std::string("tokens: input ")
        + humanize_token_count(usage.input)
        + " · cache miss "
        + humanize_token_count(cache_miss(usage.cached, usage.input))
        + " · output "
        + humanize_token_count(usage.output)
        + " · cache rate "
        + format_cache_rate(usage.cached, usage.input);
    if (usage.api_duration_ms > 0) {
        char api[32] = {};
        std::snprintf(
            api,
            sizeof(api),
            " · API: %.1f s",
            static_cast<double>(usage.api_duration_ms) / 1000.0);
        line.append(api);
    }
    if (usage.estimated) {
        line.insert(0, "~");
    }
    return line;
}

std::optional<ConversationSessionEntry> parse_conversation_session_line(
        std::string_view json_line) {
    if (json_line.empty()) {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto value = QJsonDocument::fromJson(
        QByteArray(json_line.data(), static_cast<int>(json_line.size())),
        &error);
    if (error.error != QJsonParseError::NoError || !value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.object();
    auto type = json_string(object, "type");
    if (!is_verbose_event_type(type)) {
        return std::nullopt;
    }

    ConversationSessionEntry entry;
    entry.timestamp = timestamp_from_event(object);
    entry.type = std::move(type);
    entry.api_call_id = json_string(object, "api_call_id");

    if (entry.type == "llm_response") {
        const auto input = json_int64(object.value(QLatin1StringView("input_tokens")));
        const auto output = json_int64(object.value(QLatin1StringView("output_tokens")));
        const auto cached = json_int64(object.value(QLatin1StringView("cached_tokens")));
        if (input != 0 || output != 0 || cached != 0) {
            SessionTokenUsage usage;
            usage.input = input;
            usage.output = output;
            usage.cached = cached;
            usage.estimated = object.value(QLatin1StringView("estimated")).toBool(false);
            usage.api_duration_ms = json_int64(
                object.value(QLatin1StringView("provider_wait_ms")));
            entry.token_usage = usage;
        }
        return entry;
    }

    entry.body = truncate_runes(
        extract_event_body(object, entry.type), kMaxStoredBodyBytes);
    if (entry.body.empty()) {
        return std::nullopt;
    }
    if (entry.type == "tool_call") {
        const auto args = object.value(QLatin1StringView("tool_args"));
        if (args.isObject()) {
            entry.reasoning = truncate_runes(
                json_string(args.toObject(), "_reasoning"),
                kMaxStoredReasoningBytes);
        }
    }
    return entry;
}

bool conversation_session_log_present(
        const std::filesystem::path &project_root,
        const std::filesystem::path &agent_directory_key) noexcept {
    return conversation_session_log_stat(project_root, agent_directory_key)
        .present;
}

SessionLogStat conversation_session_log_stat(
        const std::filesystem::path &project_root,
        const std::filesystem::path &agent_directory_key) noexcept {
    SessionLogStat stat;
    if (project_root.empty() || agent_directory_key.empty()
            || agent_directory_key.has_root_path()
            || !agent_directory_key.parent_path().empty()
            || agent_directory_key == "." || agent_directory_key == "..") {
        return stat;
    }
    const auto root = posix::open_root_directory(project_root);
    if (root.get() < 0) return stat;
    const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return stat;
    const auto agent = posix::open_directory_component(
        lingtai.get(), agent_directory_key);
    if (agent.get() < 0) return stat;
    const auto logs = posix::open_directory_component(agent.get(), "logs");
    if (logs.get() < 0) return stat;
    const auto file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    if (file.get() < 0) return stat;
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0) {
        return stat;
    }
    if (!S_ISREG(opened.st_mode) || S_ISLNK(opened.st_mode) || opened.st_size <= 0) {
        return stat;
    }
    stat.present = true;
    stat.size = static_cast<std::int64_t>(opened.st_size);
    stat.mtime = opened.st_mtime;
    return stat;
}

std::vector<ConversationSessionEntry> read_conversation_session_events(
        const std::filesystem::path &project_root,
        const std::filesystem::path &agent_directory_key) noexcept {
    auto entries = std::vector<ConversationSessionEntry>{};
    if (project_root.empty() || agent_directory_key.empty()
            || agent_directory_key.has_root_path()
            || !agent_directory_key.parent_path().empty()
            || agent_directory_key == "." || agent_directory_key == "..") {
        return entries;
    }

    const auto root = posix::open_root_directory(project_root);
    if (root.get() < 0) return entries;
    const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return entries;
    const auto agent = posix::open_directory_component(
        lingtai.get(), agent_directory_key);
    if (agent.get() < 0) return entries;
    const auto logs = posix::open_directory_component(agent.get(), "logs");
    if (logs.get() < 0) return entries;
    const auto file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    if (file.get() < 0) return entries;

    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)) {
        return entries;
    }
    if (S_ISLNK(opened.st_mode) || opened.st_size <= 0) {
        return entries;
    }

    const auto size = opened.st_size;
    const auto start_at = size > kMaxSessionTailBytes
        ? size - kMaxSessionTailBytes
        : off_t{0};
    if (::lseek(file.get(), start_at, SEEK_SET) < 0) {
        return entries;
    }

    std::string bytes(static_cast<std::size_t>(size - start_at), '\0');
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::read(
            file.get(), bytes.data() + total, bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return entries;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    if (start_at > 0) {
        const auto nl = bytes.find('\n');
        if (nl != std::string::npos) {
            bytes.erase(0, nl + 1);
        }
    }

    entries = std::vector<ConversationSessionEntry>{};
    std::size_t line_start = 0;
    for (std::size_t index = 0; index <= bytes.size(); ++index) {
        if (index != bytes.size() && bytes[index] != '\n') {
            continue;
        }
        const auto line = std::string_view(bytes).substr(
            line_start, index - line_start);
        line_start = index + 1;
        if (line.empty()) continue;
        if (auto entry = parse_conversation_session_line(line)) {
            entries.push_back(std::move(*entry));
        }
    }
    if (entries.size() > kMaxSessionEntries) {
        entries.erase(
            entries.begin(),
            entries.begin() + static_cast<std::ptrdiff_t>(
                entries.size() - kMaxSessionEntries));
    }
    return entries;
}

bool conversation_verbose_event_visible(
        const ConversationSessionEntry &entry,
        ConversationVerboseLevel level) noexcept {
    if (level == ConversationVerboseLevel::off) {
        return false;
    }
    if (entry.type == "llm_response") {
        return entry.token_usage.has_value();
    }
    return entry.type == "thinking"
        || entry.type == "tool_call"
        || entry.type == "tool_result";
}

std::string conversation_verbose_event_body(
        const ConversationSessionEntry &entry,
        ConversationVerboseLevel level) {
    if (level == ConversationVerboseLevel::extended) {
        return entry.body;
    }
    if (entry.type == "thinking") {
        return truncate_runes(entry.body, kThinkingPreviewLimit);
    }
    if (entry.type == "tool_call") {
        return truncate_runes(first_line(entry.body), kToolCallSummaryPreviewLimit);
    }
    if (entry.type == "tool_result") {
        return first_line(entry.body);
    }
    return entry.body;
}

bool conversation_api_group_separator_before(
        const ConversationSessionEntry *previous,
        const ConversationSessionEntry &current) noexcept {
    if (previous == nullptr
            || !is_api_grouped_type(previous->type)
            || !is_api_grouped_type(current.type)) {
        return false;
    }
    if (!previous->api_call_id.empty() || !current.api_call_id.empty()) {
        return previous->api_call_id != current.api_call_id;
    }
    return previous->type == "tool_result" && current.type == "tool_call";
}

} // namespace lingtai::desktop
