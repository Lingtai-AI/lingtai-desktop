#include "agent_activity.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QLatin1StringView>
#include <QtCore/QString>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lingtai::desktop {
namespace {

namespace posix = posix_internal;

constexpr std::size_t kMaxReadBytes = std::size_t{512} * 1024;
constexpr std::size_t kMaxRows = 100;
constexpr int kMaxDiaryTextChars = 4096;
constexpr int kMaxToolFieldChars = 128;

std::string bounded(const QString &text, int max_chars) {
    return text.left(max_chars).toStdString();
}

std::optional<QString> string_field(const QJsonObject &object, const char *key) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isString()) return std::nullopt;
    return value.toString();
}

std::string format_timestamp(double ts) {
    if (!std::isfinite(ts)) return {};
    // A finite double can still fall outside time_t's representable range;
    // the cast below is undefined behavior unless that range is checked
    // first. The upper bound is strict: numeric_limits<time_t>::max() is not
    // always exactly representable as a double, and rounding it up would let
    // an unrepresentable value pass the guard.
    constexpr auto kTimeTMin =
        static_cast<double>(std::numeric_limits<std::time_t>::min());
    constexpr auto kTimeTMax =
        static_cast<double>(std::numeric_limits<std::time_t>::max());
    if (ts < kTimeTMin || ts >= kTimeTMax) return {};
    const auto seconds = static_cast<std::time_t>(ts);
    std::tm utc{};
    if (!::gmtime_r(&seconds, &utc)) return {};
    char buffer[32];
    const auto written =
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &utc);
    return written == 0 ? std::string() : std::string(buffer, written);
}

std::string format_ts_field(const QJsonObject &object) {
    const auto ts = object.value(QLatin1StringView("ts"));
    if (!ts.isDouble()) return {};
    return format_timestamp(ts.toDouble());
}

// Opens exactly `<attachment root>/.lingtai/<selected key>/logs/events.jsonl`,
// one no-follow leaf at a time. The selected key is re-validated with the
// same primitive every other reader uses: C1's own grammar is defense in
// depth, not a reason for this boundary to trust the caller.
bool open_events_file(
        const ProjectAttachment &attachment,
        const std::filesystem::path &selected_directory_key,
        posix::FileDescriptor &file) {
    if (!posix::safe_leaf(selected_directory_key)) return false;
    const auto root = posix::open_root_directory(attachment.root());
    if (root.get() < 0) return false;
    const auto lingtai =
        posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return false;
    const auto agent = posix::open_directory_component(
        lingtai.get(), selected_directory_key);
    if (agent.get() < 0) return false;
    const auto logs = posix::open_directory_component(agent.get(), "logs");
    if (logs.get() < 0) return false;
    file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    return file.get() >= 0;
}

// Reads at most the newest `kMaxReadBytes` of the file on the actual read,
// not just an advisory size check, then discards a leading mid-record
// fragment when that read did not begin at byte zero.
bool read_bounded_suffix(int fd, std::string &bytes) {
    struct stat opened {};
    if (::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode)) return false;
    const auto size = static_cast<std::uintmax_t>(opened.st_size);
    const auto offset =
        size > kMaxReadBytes ? size - kMaxReadBytes : std::uintmax_t{0};
    if (offset > 0 && ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return false;
    }
    const auto to_read = static_cast<std::size_t>(
        std::min<std::uintmax_t>(size - offset, kMaxReadBytes));
    bytes.resize(to_read);
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count =
            ::read(fd, bytes.data() + total, bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    // Bound the read itself, not just the size check: a file that grows
    // between fstat and read still cannot be read past the fixed suffix.
    bytes.resize(total);
    if (offset > 0) {
        const auto first_lf = bytes.find('\n');
        bytes.erase(0,
            first_lf == std::string::npos ? bytes.size() : first_lf + 1);
    }
    return true;
}

// Parses each complete LF-terminated line independently and projects only
// the public diary/tool_call/tool_result allowlist, in file order. A
// trailing line with no LF is simply never visited, so an in-progress append
// never renders a half-written row.
void project_rows(const std::string &bytes, AgentActivitySnapshot &snapshot) {
    auto rows = std::vector<AgentActivityRow>();
    auto tool_call_index = std::unordered_map<std::string, std::size_t>();
    std::size_t begin = 0;
    for (;;) {
        const auto newline = bytes.find('\n', begin);
        if (newline == std::string::npos) break;
        const auto line =
            std::string_view(bytes).substr(begin, newline - begin);
        begin = newline + 1;
        if (line.empty()) continue;

        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(
            QByteArray(line.data(), static_cast<int>(line.size())), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            ++snapshot.skipped;
            continue;
        }
        const auto object = document.object();
        const auto type = string_field(object, "type");
        if (!type) continue; // no usable type: filtered, not malformed

        if (*type == QStringLiteral("diary")) {
            const auto text = string_field(object, "text");
            if (!text || text->trimmed().isEmpty()) continue;
            const auto hidden = object.value(QLatin1StringView("hidden"));
            if (hidden.isBool() && hidden.toBool()) continue;
            const auto visibility =
                object.value(QLatin1StringView("visibility"));
            if (!visibility.isUndefined() && !visibility.isNull()
                && (!visibility.isString()
                    || visibility.toString() != QStringLiteral("public"))) {
                continue;
            }
            AgentActivityRow row;
            row.text = bounded(*text, kMaxDiaryTextChars);
            row.timestamp_text = format_ts_field(object);
            rows.push_back(std::move(row));
        } else if (*type == QStringLiteral("tool_call")) {
            const auto name = string_field(object, "tool_name");
            const auto call_id = string_field(object, "tool_call_id");
            if (!name || name->trimmed().isEmpty() || !call_id
                || call_id->isEmpty()) {
                continue;
            }
            AgentActivityRow row;
            row.is_tool = true;
            row.tool_name = bounded(*name, kMaxToolFieldChars);
            const auto args = object.value(QLatin1StringView("tool_args"));
            if (args.isObject()) {
                if (const auto action =
                        string_field(args.toObject(), "action")) {
                    row.tool_action = bounded(*action, kMaxToolFieldChars);
                }
            }
            row.timestamp_text = format_ts_field(object);
            rows.push_back(std::move(row));
            tool_call_index[call_id->toStdString()] = rows.size() - 1;
        } else if (*type == QStringLiteral("tool_result")) {
            const auto call_id = string_field(object, "tool_call_id");
            if (!call_id) continue;
            const auto found = tool_call_index.find(call_id->toStdString());
            if (found == tool_call_index.end()) continue;
            auto &row = rows[found->second];
            const auto status = string_field(object, "status");
            row.tool_status = status
                    && (*status == QStringLiteral("ok")
                        || *status == QStringLiteral("success"))
                ? AgentActivityToolStatus::success
                : status && *status == QStringLiteral("error")
                    ? AgentActivityToolStatus::error
                    : AgentActivityToolStatus::unknown;
            const auto elapsed = object.value(QLatin1StringView("elapsed_ms"));
            if (elapsed.isDouble() && std::isfinite(elapsed.toDouble())
                && elapsed.toDouble() >= 0.0) {
                row.tool_elapsed_ms = elapsed.toDouble();
            }
        }
        // Every other type -- thinking, text_input, llm_call/response,
        // notifications, and anything unrecognized -- is silently filtered.
    }

    if (rows.size() > kMaxRows) {
        rows.erase(rows.begin(), rows.begin() + (rows.size() - kMaxRows));
    }
    snapshot.rows = std::move(rows);
}

} // namespace

AgentActivitySnapshot read_agent_activity(
        const ProjectAttachment &attachment,
        const std::filesystem::path &selected_directory_key) noexcept {
    try {
        posix::FileDescriptor file;
        if (!open_events_file(attachment, selected_directory_key, file)) {
            return {};
        }
        auto bytes = std::string();
        if (!read_bounded_suffix(file.get(), bytes)) {
            return {};
        }
        AgentActivitySnapshot snapshot;
        snapshot.available = true;
        project_rows(bytes, snapshot);
        return snapshot;
    } catch (...) {
        // Only row allocation can throw here. Show nothing rather than a
        // partial snapshot that looks complete.
        return {};
    }
}

} // namespace lingtai::desktop
