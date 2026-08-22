#include "kanban_model.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <functional>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace lingtai::desktop {
namespace {

namespace posix = posix_internal;

constexpr std::size_t kMaxJsonBytes = 1024U * 1024U;
constexpr std::size_t kMaxLedgerBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxDaemonBytes = 64U * 1024U;
constexpr int kRecentLedgerEntries = 100;
constexpr int kRecentDaemonRuns = 128;
constexpr int kSqliteBusyTimeoutMs = 150;
constexpr int kEventTailBytes = 256 * 1024;
constexpr int kEventTailLines = 1000;

[[nodiscard]] std::string ascii_lower(std::string_view value) {
    auto out = std::string(value);
    for (auto &ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

[[nodiscard]] std::optional<std::string> nonempty_string(
        const QJsonObject &object, const char *key) {
    const auto value = object.value(key);
    if (!value.isString()) return std::nullopt;
    auto text = value.toString().toStdString();
    if (text.empty()) return std::nullopt;
    return text;
}

[[nodiscard]] std::optional<std::int64_t> json_int64(const QJsonValue &value) {
    if (!value.isDouble()) return std::nullopt;
    const auto number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
}

[[nodiscard]] std::string json_display(const QJsonValue &value) {
    if (value.isString()) return value.toString().toStdString();
    if (value.isBool()) return value.toBool() ? "true" : "false";
    if (value.isDouble()) {
        const auto number = value.toDouble();
        if (std::isfinite(number) && std::trunc(number) == number) {
            return std::to_string(static_cast<std::int64_t>(number));
        }
        return QString::number(number, 'g', 15).toStdString();
    }
    if (value.isNull()) return {};
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact))
        .toStdString();
}

[[nodiscard]] std::optional<std::string> read_regular_file(
        int parent_fd, const char *name, std::size_t limit) {
    struct stat leaf {};
    if (::fstatat(parent_fd, name, &leaf, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::nullopt;
    }
    if (S_ISLNK(leaf.st_mode) || !S_ISREG(leaf.st_mode) || leaf.st_size < 0
        || static_cast<std::uintmax_t>(leaf.st_size) > limit) {
        return std::nullopt;
    }
    const auto file = posix::open_regular_file_component(parent_fd, name);
    if (file.get() < 0) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(leaf.st_size), '\0');
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::read(
            file.get(), bytes.data() + total, bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    return bytes;
}

[[nodiscard]] std::optional<std::string> read_nested_file(
        int agent_fd,
        const char *directory,
        const char *name,
        std::size_t limit) {
    const auto dir = posix::open_directory_component(agent_fd, directory);
    if (dir.get() < 0) return std::nullopt;
    return read_regular_file(dir.get(), name, limit);
}

[[nodiscard]] QJsonObject parse_object(const std::string &bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<int>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}

void overlay_init_fields(QJsonObject &raw, const QJsonObject &init) {
    auto merged = init;
    if (const auto llm = init.value("llm"); llm.isObject()) {
        const auto llm_object = llm.toObject();
        for (auto it = llm_object.begin(); it != llm_object.end(); ++it) {
            if (!merged.contains(it.key())) merged.insert(it.key(), it.value());
        }
    }
    if (const auto soul = init.value("soul"); soul.isObject()) {
        const auto delay = soul.toObject().value("delay");
        if (!delay.isUndefined() && !merged.contains("soul_delay")) {
            merged.insert("soul_delay", delay);
        }
    }
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        if (!raw.contains(it.key())) raw.insert(it.key(), it.value());
    }
}

void append_fields(
        std::vector<KanbanField> &out,
        const QJsonObject &raw,
        std::initializer_list<std::pair<const char *, const char *>> keys) {
    for (const auto &[key, label] : keys) {
        const auto value = raw.value(key);
        if (value.isUndefined() || value.isNull()) continue;
        auto text = json_display(value);
        if (text.empty()) continue;
        out.push_back({label, std::move(text)});
    }
}

[[nodiscard]] std::vector<std::string> list_directory_names(int parent_fd) {
    const auto duplicate = ::dup(parent_fd);
    if (duplicate < 0) return {};
    posix::DirectoryStream stream(::fdopendir(duplicate));
    if (!stream.get()) {
        ::close(duplicate);
        return {};
    }
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        const auto entry = ::readdir(stream.get());
        if (!entry) break;
        const auto name = std::string(entry->d_name);
        if (name != "." && name != "..") names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] KanbanDaemonCounts count_daemons(int agent_fd) {
    const auto daemons = posix::open_directory_component(agent_fd, "daemons");
    if (daemons.get() < 0) return {};
    KanbanDaemonCounts counts;
    for (const auto &name : list_directory_names(daemons.get())) {
        std::optional<std::string> bytes;
        if (name == "daemon.json") {
            bytes = read_regular_file(daemons.get(), "daemon.json", kMaxDaemonBytes);
        } else if (posix::safe_leaf(name)) {
            const auto run = posix::open_directory_component(daemons.get(), name);
            if (run.get() < 0) continue;
            bytes = read_regular_file(run.get(), "daemon.json", kMaxDaemonBytes);
        }
        if (!bytes) continue;
        const auto object = parse_object(*bytes);
        if (object.isEmpty()) continue;
        ++counts.total;
        const auto state = ascii_lower(
            object.value("state").toString().toStdString());
        const auto finished = object.value("finished_at");
        const auto finished_text = finished.isString()
            ? finished.toString().trimmed().toStdString()
            : std::string();
        const auto done = !finished.isUndefined() && !finished.isNull()
            && (!finished.isString() || !finished_text.empty());
        if (!done && (state == "running" || state == "active")) {
            ++counts.running;
        }
    }
    return counts;
}

[[nodiscard]] std::vector<std::string> mcp_names_from_init(const QJsonObject &init) {
    const auto mcp = init.value("mcp");
    if (!mcp.isObject()) return {};
    std::vector<std::string> names;
    const auto object = mcp.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        auto name = it.key().toStdString();
        if (!name.empty()) names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

void add_token_line(
        KanbanTokenTotals &totals, const QJsonObject &entry) {
    totals.input += json_int64(entry.value("input")).value_or(0);
    totals.output += json_int64(entry.value("output")).value_or(0);
    totals.thinking += json_int64(entry.value("thinking")).value_or(0);
    totals.cached += json_int64(entry.value("cached")).value_or(0);
    ++totals.api_calls;
}

[[nodiscard]] bool is_daemon_ledger_object(const QJsonObject &entry) {
    const auto source = entry.value("source").toString().trimmed();
    if (source.compare(QStringLiteral("daemon"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (!entry.value("em_id").toString().trimmed().isEmpty()) return true;
    if (!entry.value("run_id").toString().trimmed().isEmpty()) return true;
    return false;
}

KanbanLedgerEntry ledger_entry_from_object(const QJsonObject &object) {
    KanbanLedgerEntry entry;
    entry.ts = object.value("ts").toString().toStdString();
    entry.model = object.value("model").toString().toStdString();
    entry.endpoint = object.value("endpoint").toString().toStdString();
    entry.source = object.value("source").toString().toStdString();
    entry.em_id = object.value("em_id").toString().toStdString();
    entry.run_id = object.value("run_id").toString().toStdString();
    entry.codex_transfer_mode =
        object.value("codex_transfer_mode").toString().toStdString();
    entry.codex_ws_delta_reason =
        object.value("codex_ws_delta_reason").toString().toStdString();
    entry.provider = derive_ledger_provider(entry.endpoint, entry.model);
    entry.input = json_int64(object.value("input")).value_or(0);
    entry.output = json_int64(object.value("output")).value_or(0);
    entry.thinking = json_int64(object.value("thinking")).value_or(0);
    entry.cached = json_int64(object.value("cached")).value_or(0);
    return entry;
}

void sort_provider_spend(std::vector<KanbanProviderSpend> &rows) {
    std::sort(rows.begin(), rows.end(),
        [](const KanbanProviderSpend &a, const KanbanProviderSpend &b) {
            if (a.totals.spend() != b.totals.spend()) {
                return a.totals.spend() > b.totals.spend();
            }
            return a.name < b.name;
        });
}

void add_provider_totals(
        std::map<std::string, KanbanTokenTotals> &by_provider,
        const std::string &name,
        const KanbanTokenTotals &add) {
    auto &into = by_provider[name];
    into.input += add.input;
    into.output += add.output;
    into.thinking += add.thinking;
    into.cached += add.cached;
    into.api_calls += add.api_calls;
}

[[nodiscard]] QDateTime parse_ledger_time(const std::string &ts) {
    auto parsed = QDateTime::fromString(
        QString::fromStdString(ts), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(QString::fromStdString(ts), Qt::ISODate);
    }
    return parsed.toUTC();
}

[[nodiscard]] QDateTime unix_float_utc(double ts) {
    return QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(ts * 1000.0), QTimeZone::utc());
}

[[nodiscard]] bool time_within_bounds(
        const QDateTime &ts, const QDateTime &since, const QDateTime &before) {
    if (!since.isValid() && !before.isValid()) return true;
    if (!ts.isValid()) return false;
    if (since.isValid() && ts < since) return false;
    if (before.isValid() && ts >= before) return false;
    return true;
}

void add_ledger_to_session(KanbanSessionStats &stats, const KanbanLedgerEntry &entry) {
    stats.tokens.input += entry.input;
    stats.tokens.output += entry.output;
    stats.tokens.thinking += entry.thinking;
    stats.tokens.cached += entry.cached;
    ++stats.tokens.api_calls;
    auto mode = ascii_lower(entry.codex_transfer_mode);
    while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.front()))) {
        mode.erase(mode.begin());
    }
    while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.back()))) {
        mode.pop_back();
    }
    if (mode == "full") {
        stats.has_codex_transfer_mode = true;
        ++stats.codex_full;
    } else if (mode == "incremental") {
        stats.has_codex_transfer_mode = true;
        ++stats.codex_incremental;
    }
}

void parse_jsonl_line(
        std::string_view line,
        const std::function<void(const QJsonObject &)> &fn) {
    while (!line.empty()
            && std::isspace(static_cast<unsigned char>(line.front()))) {
        line.remove_prefix(1);
    }
    while (!line.empty()
            && std::isspace(static_cast<unsigned char>(line.back()))) {
        line.remove_suffix(1);
    }
    if (line.empty()) return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(line.data(), static_cast<int>(line.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    fn(document.object());
}

void for_each_jsonl_file(
        int parent_fd,
        const char *name,
        const std::function<void(const QJsonObject &)> &fn,
        const std::function<bool(std::string_view)> &keep = {}) {
    if (!posix::safe_leaf(name)) return;
    struct stat leaf {};
    if (::fstatat(parent_fd, name, &leaf, AT_SYMLINK_NOFOLLOW) != 0) return;
    if (S_ISLNK(leaf.st_mode) || !S_ISREG(leaf.st_mode)) return;
    const auto file = posix::open_regular_file_component(parent_fd, name);
    if (file.get() < 0) return;
    std::string pending;
    char buf[65536];
    for (;;) {
        const auto count = ::read(file.get(), buf, sizeof(buf));
        if (count < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (count == 0) break;
        pending.append(buf, static_cast<std::size_t>(count));
        auto start = std::size_t{0};
        for (;;) {
            const auto nl = pending.find('\n', start);
            if (nl == std::string::npos) {
                if (start > 0) pending.erase(0, start);
                break;
            }
            const auto line = std::string_view(pending).substr(start, nl - start);
            start = nl + 1;
            if (keep && !keep(line)) continue;
            parse_jsonl_line(line, fn);
        }
    }
    if (!pending.empty() && (!keep || keep(pending))) {
        parse_jsonl_line(pending, fn);
    }
}

void for_each_nested_jsonl(
        int agent_fd,
        const char *directory,
        const char *name,
        const std::function<void(const QJsonObject &)> &fn,
        const std::function<bool(std::string_view)> &keep = {}) {
    const auto dir = posix::open_directory_component(agent_fd, directory);
    if (dir.get() < 0) return;
    for_each_jsonl_file(dir.get(), name, fn, keep);
}

[[nodiscard]] bool jsonl_type_is(std::string_view line, std::string_view type) {
    auto needle = std::string("\"type\":\"");
    needle.append(type);
    needle.push_back('"');
    return line.find(needle) != std::string_view::npos;
}

[[nodiscard]] std::int64_t stat_mtime_ns(const struct stat &st) {
#ifdef __APPLE__
    return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1'000'000'000LL
        + static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#else
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL
        + static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#endif
}

void read_token_ledger(
        int agent_fd,
        KanbanTokenTotals &totals,
        std::map<std::string, KanbanTokenTotals> &by_provider,
        std::vector<KanbanLedgerEntry> &recent) {
    for_each_nested_jsonl(agent_fd, "logs", "token_ledger.jsonl",
        [&](const QJsonObject &object) {
            if (is_daemon_ledger_object(object)) return;
            add_token_line(totals, object);
            auto entry = ledger_entry_from_object(object);
            add_token_line(by_provider[entry.provider], object);
            recent.push_back(std::move(entry));
        });
    if (static_cast<int>(recent.size()) > kRecentLedgerEntries) {
        recent.erase(recent.begin(),
            recent.end() - kRecentLedgerEntries);
    }
    std::reverse(recent.begin(), recent.end());
}

[[nodiscard]] std::string daemon_fallback_provider(const QJsonObject &card) {
    if (const auto preset = nonempty_string(card, "preset_provider")) {
        return *preset;
    }
    const auto backend = card.value("backend").toString().trimmed().toStdString();
    if (!backend.empty() && backend != "lingtai") return backend;
    auto model = card.value("preset_model").toString().toStdString();
    if (model.empty()) model = card.value("model").toString().toStdString();
    if (!model.empty()) {
        const auto derived = derive_ledger_provider("", model);
        if (derived != "unknown") return derived;
    }
    if (!backend.empty()) return backend;
    const auto raw_model = card.value("model").toString().toStdString();
    if (!raw_model.empty()) return raw_model;
    return "daemon";
}

void read_daemon_ledger_summary(
        int agent_fd,
        std::vector<KanbanProviderSpend> &providers,
        std::vector<KanbanDaemonLedgerEntry> &recent,
        int &scanned_runs,
        int &total_runs) {
    scanned_runs = 0;
    total_runs = 0;
    const auto daemons = posix::open_directory_component(agent_fd, "daemons");
    if (daemons.get() < 0) return;
    struct DaemonRunRef {
        std::string name;
        std::int64_t mtime = 0;
    };
    std::vector<DaemonRunRef> runs;
    for (const auto &name : list_directory_names(daemons.get())) {
        if (!posix::safe_leaf(name)) continue;
        struct stat st {};
        if (::fstatat(daemons.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) continue;
        runs.push_back({name, stat_mtime_ns(st)});
    }
    std::sort(runs.begin(), runs.end(),
        [](const DaemonRunRef &a, const DaemonRunRef &b) {
            if (a.mtime != b.mtime) return a.mtime > b.mtime;
            return a.name > b.name;
        });
    total_runs = static_cast<int>(runs.size());
    if (static_cast<int>(runs.size()) > kRecentDaemonRuns) {
        runs.resize(static_cast<std::size_t>(kRecentDaemonRuns));
    }
    scanned_runs = static_cast<int>(runs.size());

    std::map<std::string, KanbanTokenTotals> by_provider;
    std::vector<KanbanDaemonLedgerEntry> all;
    for (const auto &ref : runs) {
        const auto run = posix::open_directory_component(daemons.get(), ref.name);
        if (run.get() < 0) continue;
        auto card = QJsonObject();
        if (const auto bytes = read_regular_file(
                run.get(), "daemon.json", kMaxDaemonBytes)) {
            card = parse_object(*bytes);
        }
        auto run_id = card.value("run_id").toString().toStdString();
        if (run_id.empty()) run_id = ref.name;
        const auto handle = card.value("handle").toString().toStdString();
        const auto state = card.value("state").toString().toStdString();
        const auto backend = card.value("backend").toString().toStdString();

        std::vector<KanbanLedgerEntry> run_entries;
        for_each_nested_jsonl(run.get(), "logs", "token_ledger.jsonl",
            [&](const QJsonObject &object) {
                run_entries.push_back(ledger_entry_from_object(object));
            });
        if (!run_entries.empty()) {
            for (auto &entry : run_entries) {
                add_provider_totals(by_provider, entry.provider, {
                    entry.input, entry.output, entry.thinking, entry.cached, 1});
                KanbanDaemonLedgerEntry tagged;
                static_cast<KanbanLedgerEntry &>(tagged) = entry;
                tagged.run_id = run_id;
                tagged.handle = handle;
                tagged.state = state;
                tagged.backend = backend;
                all.push_back(std::move(tagged));
            }
            continue;
        }

        KanbanTokenTotals fallback;
        const auto cli = card.value("cli_tokens");
        const auto legacy = card.value("tokens");
        if (cli.isObject()) {
            const auto object = cli.toObject();
            fallback.input = json_int64(object.value("input")).value_or(0);
            fallback.output = json_int64(object.value("output")).value_or(0);
            fallback.thinking = json_int64(object.value("thinking")).value_or(0);
            fallback.cached = json_int64(object.value("cached")).value_or(0);
            fallback.api_calls = json_int64(object.value("calls")).value_or(0);
            if (fallback.input + fallback.output + fallback.thinking
                    + fallback.cached != 0 || fallback.api_calls != 0) {
                fallback.input += fallback.cached;
            } else {
                fallback = {};
            }
        }
        if (fallback.input + fallback.output + fallback.thinking + fallback.cached == 0
                && fallback.api_calls == 0 && legacy.isObject()) {
            const auto object = legacy.toObject();
            fallback.input = json_int64(object.value("input")).value_or(0);
            fallback.output = json_int64(object.value("output")).value_or(0);
            fallback.thinking = json_int64(object.value("thinking")).value_or(0);
            fallback.cached = json_int64(object.value("cached")).value_or(0);
        }
        if (fallback.input + fallback.output + fallback.thinking + fallback.cached == 0
                && fallback.api_calls == 0) {
            continue;
        }
        add_provider_totals(by_provider, daemon_fallback_provider(card), fallback);
    }
    std::sort(all.begin(), all.end(),
        [](const KanbanDaemonLedgerEntry &a, const KanbanDaemonLedgerEntry &b) {
            return a.ts > b.ts;
        });
    if (static_cast<int>(all.size()) > kRecentLedgerEntries) {
        all.resize(static_cast<std::size_t>(kRecentLedgerEntries));
    }
    for (auto &[name, totals] : by_provider) {
        providers.push_back({name, totals});
    }
    sort_provider_spend(providers);
    recent = std::move(all);
}

struct MoltWindows {
    QDateTime current_since;
    QDateTime last_since;
    QDateTime last_before;
};

struct SqliteCloser {
    sqlite3 *db = nullptr;
    ~SqliteCloser() {
        if (db) sqlite3_close(db);
    }
};

[[nodiscard]] sqlite3 *open_log_sqlite(
        int agent_fd, const std::filesystem::path &agent_dir) {
    const auto logs = posix::open_directory_component(agent_fd, "logs");
    if (logs.get() < 0) return nullptr;
    struct stat st {};
    if (::fstatat(logs.get(), "log.sqlite", &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return nullptr;
    }
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return nullptr;
    const auto path = (agent_dir / "logs" / "log.sqlite").string();
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(
            path.c_str(),
            &db,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
            nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, kSqliteBusyTimeoutMs);
    return db;
}

[[nodiscard]] bool sqlite_query_doubles(
        sqlite3 *db,
        const char *sql,
        std::vector<double> &out,
        int limit) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    auto ok = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(sqlite3_column_double(stmt, 0));
        if (limit > 0 && static_cast<int>(out.size()) >= limit) break;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) ok = false;
    return ok;
}

[[nodiscard]] bool read_molt_windows_sqlite(
        sqlite3 *db, MoltWindows &windows) {
    std::vector<double> times;
    if (!sqlite_query_doubles(db,
            "SELECT ts FROM events WHERE type='psyche_molt' "
            "ORDER BY ts DESC LIMIT 2",
            times, 2)) {
        return false;
    }
    if (times.empty() || times.front() <= 0) return true;
    windows.current_since = unix_float_utc(times.front());
    if (times.size() > 1 && times[1] > 0) {
        windows.last_since = unix_float_utc(times[1]);
        windows.last_before = windows.current_since;
    }
    return true;
}

[[nodiscard]] bool read_tool_call_counts_sqlite(
        sqlite3 *db,
        const MoltWindows &windows,
        std::int64_t &current,
        std::int64_t &last) {
    const auto cs = windows.current_since.isValid()
        ? windows.current_since.toMSecsSinceEpoch() / 1000.0
        : 0.0;
    const auto ls = windows.last_since.isValid()
        ? windows.last_since.toMSecsSinceEpoch() / 1000.0
        : 0.0;
    const auto lb = windows.last_before.isValid()
        ? windows.last_before.toMSecsSinceEpoch() / 1000.0
        : 0.0;
    auto scan_lower = cs;
    if (windows.last_before.isValid()) scan_lower = ls;
    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT "
        "COUNT(CASE WHEN ts >= ?1 THEN 1 END), "
        "COUNT(CASE WHEN ?2 > 0 AND ts >= ?3 AND ts < ?4 THEN 1 END) "
        "FROM events WHERE type='tool_call' "
        "AND (scope IS NULL OR scope != 'daemon') "
        "AND (source_kind IS NULL OR source_kind != 'daemon_events') "
        "AND ts >= ?5";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_double(stmt, 1, cs);
    sqlite3_bind_double(stmt, 2, lb);
    sqlite3_bind_double(stmt, 3, ls);
    sqlite3_bind_double(stmt, 4, lb);
    sqlite3_bind_double(stmt, 5, scan_lower);
    auto ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        current = sqlite3_column_int64(stmt, 0);
        last = sqlite3_column_int64(stmt, 1);
        ok = true;
    }
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<std::int64_t> sqlite_event_times_ms(
        sqlite3 *db, const char *type, int limit) {
    auto sql = std::string(
        "SELECT ts FROM events WHERE type='") + type
        + "' ORDER BY ts DESC LIMIT " + std::to_string(limit);
    std::vector<double> times;
    if (!sqlite_query_doubles(db, sql.c_str(), times, limit)) return {};
    std::vector<std::int64_t> out;
    for (const auto ts : times) {
        if (ts <= 0) continue;
        out.push_back(unix_float_utc(ts).toMSecsSinceEpoch());
    }
    return out;
}

void read_molt_windows_jsonl(int agent_fd, MoltWindows &windows) {
    for_each_nested_jsonl(agent_fd, "logs", "events.jsonl",
        [&](const QJsonObject &object) {
            if (object.value("type").toString() != QStringLiteral("psyche_molt")) {
                return;
            }
            const auto ts = object.value("ts").toDouble();
            if (ts <= 0) return;
            windows.last_since = windows.current_since;
            windows.last_before = unix_float_utc(ts);
            windows.current_since = windows.last_before;
        },
        [](std::string_view line) {
            return jsonl_type_is(line, "psyche_molt");
        });
}

void count_tool_calls_jsonl(
        int agent_fd,
        const MoltWindows &windows,
        KanbanSessionStats &current,
        KanbanSessionStats &last) {
    for_each_nested_jsonl(agent_fd, "logs", "events.jsonl",
        [&](const QJsonObject &object) {
            if (object.value("type").toString() != QStringLiteral("tool_call")) {
                return;
            }
            const auto ts = unix_float_utc(object.value("ts").toDouble());
            if (time_within_bounds(ts, windows.current_since, QDateTime())) {
                ++current.tool_calls;
            }
            if (windows.last_before.isValid()
                    && time_within_bounds(
                        ts, windows.last_since, windows.last_before)) {
                ++last.tool_calls;
            }
        },
        [](std::string_view line) {
            return jsonl_type_is(line, "tool_call");
        });
}

std::vector<std::int64_t> tail_event_times_ms(
        int agent_fd, const char *type, int limit) {
    const auto logs = posix::open_directory_component(agent_fd, "logs");
    if (logs.get() < 0) return {};
    struct stat st {};
    if (::fstatat(logs.get(), "events.jsonl", &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return {};
    }
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return {};
    }
    const auto file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    if (file.get() < 0) return {};
    const auto size = static_cast<off_t>(st.st_size);
    const auto start_at = size > kEventTailBytes
        ? size - static_cast<off_t>(kEventTailBytes)
        : static_cast<off_t>(0);
    if (::lseek(file.get(), start_at, SEEK_SET) < 0) return {};
    std::string bytes;
    bytes.resize(static_cast<std::size_t>(size - start_at));
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::read(
            file.get(), bytes.data() + total, bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return {};
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    if (start_at > 0) {
        const auto nl = bytes.find('\n');
        if (nl != std::string::npos) bytes.erase(0, nl + 1);
    }
    std::vector<std::int64_t> times;
    auto pos = bytes.size();
    auto lines = 0;
    while (pos > 0 && lines < kEventTailLines) {
        const auto end = pos;
        const auto prev = bytes.rfind('\n', pos - 1);
        const auto begin = prev == std::string::npos ? 0 : prev + 1;
        const auto line = std::string_view(bytes).substr(begin, end - begin);
        pos = prev == std::string::npos ? 0 : prev;
        ++lines;
        if (!jsonl_type_is(line, type)) continue;
        parse_jsonl_line(line, [&](const QJsonObject &object) {
            if (object.value("type").toString().toStdString() != type) return;
            const auto ts = object.value("ts").toDouble();
            if (ts <= 0) return;
            times.push_back(unix_float_utc(ts).toMSecsSinceEpoch());
        });
        if (static_cast<int>(times.size()) >= limit) break;
    }
    return times;
}

void read_molt_session_stats(
        int agent_fd,
        const std::filesystem::path &agent_dir,
        KanbanSessionStats &current,
        KanbanSessionStats &last,
        std::vector<std::int64_t> &molt_times_ms,
        std::vector<std::int64_t> &refresh_times_ms) {
    MoltWindows windows;
    auto counted_tools = false;
    SqliteCloser sqlite;
    sqlite.db = open_log_sqlite(agent_fd, agent_dir);
    if (sqlite.db && read_molt_windows_sqlite(sqlite.db, windows)) {
        std::int64_t current_tools = 0;
        std::int64_t last_tools = 0;
        if (read_tool_call_counts_sqlite(
                sqlite.db, windows, current_tools, last_tools)) {
            current.tool_calls = current_tools;
            last.tool_calls = last_tools;
            counted_tools = true;
        }
        molt_times_ms = sqlite_event_times_ms(
            sqlite.db, "psyche_molt", kRecentLedgerEntries);
        refresh_times_ms = sqlite_event_times_ms(
            sqlite.db, "refresh_complete", kRecentLedgerEntries);
    } else {
        read_molt_windows_jsonl(agent_fd, windows);
    }
    if (!counted_tools) {
        count_tool_calls_jsonl(agent_fd, windows, current, last);
    }
    if (molt_times_ms.empty()) {
        molt_times_ms = tail_event_times_ms(
            agent_fd, "psyche_molt", kRecentLedgerEntries);
    }
    if (refresh_times_ms.empty()) {
        refresh_times_ms = tail_event_times_ms(
            agent_fd, "refresh_complete", kRecentLedgerEntries);
    }
    for_each_nested_jsonl(agent_fd, "logs", "token_ledger.jsonl",
        [&](const QJsonObject &object) {
            if (is_daemon_ledger_object(object)) return;
            const auto entry = ledger_entry_from_object(object);
            const auto ts = parse_ledger_time(entry.ts);
            if (time_within_bounds(ts, windows.current_since, QDateTime())) {
                add_ledger_to_session(current, entry);
            }
            if (windows.last_before.isValid()
                    && time_within_bounds(
                        ts, windows.last_since, windows.last_before)) {
                add_ledger_to_session(last, entry);
            }
        });
}

KanbanContextStats read_context_stats(int agent_fd) {
    KanbanContextStats stats;
    const auto bytes = read_nested_file(
        agent_fd, "history", "chat_history.jsonl", kMaxLedgerBytes);
    if (!bytes) return stats;
    std::map<std::string, KanbanToolCount> tools;
    auto start = std::size_t{0};
    while (start < bytes->size()) {
        const auto end = bytes->find('\n', start);
        const auto line = std::string_view(*bytes).substr(
            start, (end == std::string::npos ? bytes->size() : end) - start);
        start = end == std::string::npos ? bytes->size() : end + 1;
        auto trimmed = line;
        while (!trimmed.empty()
                && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
            trimmed.remove_prefix(1);
        }
        if (trimmed.empty()) continue;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(
            QByteArray(trimmed.data(), static_cast<int>(trimmed.size())),
            &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const auto object = document.object();
        ++stats.entries;
        const auto role = object.value("role").toString().toStdString();
        if (role == "system") ++stats.system_messages;
        else if (role == "assistant") ++stats.assistant_messages;
        else if (role == "user") ++stats.user_messages;

        const auto content = object.value("content");
        if (content.isString() && !content.toString().isEmpty()) {
            if (role == "assistant") ++stats.text_outputs;
            else if (role != "system") ++stats.text_inputs;
        }
        if (!content.isArray()) continue;
        for (const auto &block_value : content.toArray()) {
            if (!block_value.isObject()) continue;
            const auto block = block_value.toObject();
            const auto type = block.value("type").toString().toStdString();
            auto name = block.value("name").toString().toStdString();
            if (name.empty()) name = "unknown";
            if (type == "tool_call") {
                ++stats.tool_calls;
                tools[name].name = name;
                ++tools[name].calls;
            } else if (type == "tool_result") {
                ++stats.tool_results;
                tools[name].name = name;
                ++tools[name].results;
            } else if (type == "text") {
                if (role == "assistant") ++stats.text_outputs;
                else if (role != "system") ++stats.text_inputs;
            }
        }
    }
    for (auto &[_, count] : tools) stats.tool_counts.push_back(count);
    std::sort(stats.tool_counts.begin(), stats.tool_counts.end(),
        [](const KanbanToolCount &a, const KanbanToolCount &b) {
            if (a.calls != b.calls) return a.calls > b.calls;
            return a.name < b.name;
        });
    return stats;
}

struct AvatarEdge {
    std::string parent_key;
    std::string child_key;
    std::string child_name;
};

std::vector<AvatarEdge> read_avatar_edges(
        int agent_fd, const std::string &parent_key) {
    const auto bytes = read_nested_file(
        agent_fd, "delegates", "ledger.jsonl", kMaxJsonBytes);
    if (!bytes) return {};
    std::vector<AvatarEdge> edges;
    auto start = std::size_t{0};
    while (start < bytes->size()) {
        const auto end = bytes->find('\n', start);
        const auto line = std::string_view(*bytes).substr(
            start, (end == std::string::npos ? bytes->size() : end) - start);
        start = end == std::string::npos ? bytes->size() : end + 1;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(
            QByteArray(line.data(), static_cast<int>(line.size())), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const auto object = document.object();
        if (object.value("event").toString() != QStringLiteral("avatar")) {
            continue;
        }
        auto working = object.value("working_dir").toString().toStdString();
        if (working.empty()) continue;
        auto slash = working.find_last_of("/\\");
        auto child_key = slash == std::string::npos
            ? working : working.substr(slash + 1);
        if (child_key.empty()) continue;
        edges.push_back({parent_key, std::move(child_key),
            object.value("name").toString().toStdString()});
    }
    return edges;
}

[[nodiscard]] std::string uppercase_state(std::string_view state) {
    auto out = std::string(state);
    for (auto &ch : out) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

[[nodiscard]] std::string agent_state(const AgentRow &row) {
    if (row.identity && row.identity->state && !row.identity->state->empty()) {
        return uppercase_state(*row.identity->state);
    }
    if (row.status && row.status->state && !row.status->state->empty()) {
        return uppercase_state(*row.status->state);
    }
    return {};
}

[[nodiscard]] std::string display_name(const AgentRow &row) {
    if (row.identity) {
        if (row.identity->nickname && !row.identity->nickname->empty()) {
            return *row.identity->nickname;
        }
        if (row.identity->true_name && !row.identity->true_name->empty()) {
            return *row.identity->true_name;
        }
    }
    return row.directory_key.string();
}

void add_totals(KanbanTokenTotals &into, const KanbanTokenTotals &from) {
    into.input += from.input;
    into.output += from.output;
    into.thinking += from.thinking;
    into.cached += from.cached;
    into.api_calls += from.api_calls;
}

[[nodiscard]] std::string activity_status(
        int active_agents, int running_daemons, bool has_idle, bool has_asleep) {
    if (active_agents > 0) return "active";
    if (running_daemons > 0) return "daemon-active";
    if (has_idle) return "idle";
    if (has_asleep) return "asleep";
    return "suspend";
}

std::vector<std::string> render_tree(
        const std::vector<KanbanAgent> &agents,
        const std::vector<AvatarEdge> &edges) {
    std::map<std::string, std::string> names;
    for (const auto &agent : agents) {
        names[agent.directory_key.string()] = agent.display_name;
    }
    std::map<std::string, std::vector<std::string>> children;
    std::set<std::string> child_set;
    for (const auto &edge : edges) {
        children[edge.parent_key].push_back(edge.child_key);
        child_set.insert(edge.child_key);
        if (!edge.child_name.empty() && !names.contains(edge.child_key)) {
            names[edge.child_key] = edge.child_name;
        }
    }
    std::vector<std::string> roots;
    for (const auto &agent : agents) {
        const auto key = agent.directory_key.string();
        if (agent.is_human) {
            roots.insert(roots.begin(), key);
        } else if (!child_set.contains(key)) {
            roots.push_back(key);
        }
    }
    std::vector<std::string> lines;
    const auto walk = [&](auto &&self, const std::string &key,
            const std::string &prefix, bool is_last, bool is_root) -> void {
        auto name = names.contains(key) ? names[key] : key;
        if (!is_root) {
            lines.push_back(prefix + (is_last ? "└ " : "├ ") + name);
        } else {
            lines.push_back(name);
        }
        const auto &kids = children[key];
        auto child_prefix = prefix;
        if (!is_root) child_prefix += is_last ? "  " : "│ ";
        for (auto index = std::size_t{0}; index != kids.size(); ++index) {
            self(self, kids[index], child_prefix, index + 1 == kids.size(), false);
        }
    };
    for (const auto &root : roots) walk(walk, root, "", true, true);
    return lines;
}

} // namespace

KanbanBoardColumn kanban_column_for_state(std::string_view state) noexcept {
    const auto upper = uppercase_state(state);
    if (upper == "ACTIVE") return KanbanBoardColumn::active;
    if (upper == "IDLE" || upper == "STUCK") {
        return upper == "STUCK" ? KanbanBoardColumn::stuck
                                : KanbanBoardColumn::idle;
    }
    if (upper == "ASLEEP") return KanbanBoardColumn::asleep;
    return KanbanBoardColumn::suspended;
}

std::string_view kanban_column_title(KanbanBoardColumn column) noexcept {
    switch (column) {
    case KanbanBoardColumn::active: return "Active";
    case KanbanBoardColumn::idle: return "Idle";
    case KanbanBoardColumn::stuck: return "Stuck";
    case KanbanBoardColumn::asleep: return "Asleep";
    case KanbanBoardColumn::suspended: return "Suspended";
    }
    return "Suspended";
}

std::string derive_ledger_provider(
        std::string_view endpoint, std::string_view model) {
    const auto ep = ascii_lower(endpoint);
    if (!ep.empty()) {
        if (ep.find("minimaxi.com") != std::string::npos
                || ep.find("minimax.chat") != std::string::npos) {
            return "minimax";
        }
        if (ep.find("deepseek.com") != std::string::npos) return "deepseek";
        if (ep.find("z.ai") != std::string::npos
                || ep.find("bigmodel.cn") != std::string::npos) {
            return "zhipu";
        }
        if (ep.find("xiaomimimo.com") != std::string::npos) return "mimo";
        if (ep.find("openai.com") != std::string::npos) return "openai";
        if (ep.find("anthropic.com") != std::string::npos) return "anthropic";
        if (ep.find("googleapis.com") != std::string::npos
                || ep.find("generativelanguage") != std::string::npos) {
            return "gemini";
        }
        if (ep.find("openrouter.ai") != std::string::npos) return "openrouter";
        if (ep.find("api.nvidia.com") != std::string::npos) return "nvidia";
        auto host = ep;
        const auto scheme = host.find("://");
        if (scheme != std::string::npos) host = host.substr(scheme + 3);
        const auto slash = host.find('/');
        if (slash != std::string::npos) host = host.substr(0, slash);
        if (host.starts_with("www.")) host.erase(0, 4);
        if (!host.empty()) return host;
    }
    const auto mp = ascii_lower(model);
    if (mp.starts_with("minimax-")) return "minimax";
    if (mp.starts_with("deepseek-")) return "deepseek";
    if (mp.starts_with("glm-")) return "zhipu";
    if (mp.starts_with("mimo-")) return "mimo";
    if (mp.starts_with("gpt-") || mp.starts_with("o1-") || mp.starts_with("o3-")) {
        return "openai";
    }
    if (mp.starts_with("claude-")) return "anthropic";
    if (mp.starts_with("gemini-")) return "gemini";
    return "unknown";
}

KanbanBoard read_kanban_board(
        const ProjectAttachment &attachment,
        const AgentSnapshot &snapshot) noexcept {
    KanbanBoard board;
    board.network_root = attachment.root() / ".lingtai";
    std::vector<AvatarEdge> edges;
    auto active_agents = 0;
    auto has_idle = false;
    auto has_asleep = false;

    for (const auto &row : snapshot.items) {
        if (row.manifest_kind != AgentManifestKind::valid) continue;
        KanbanAgent agent;
        agent.directory_key = row.directory_key;
        agent.directory_path = row.directory_path;
        agent.display_name = display_name(row);
        agent.state = agent_state(row);
        agent.is_human = row.role == AgentRole::human;
        agent.role = row.role;
        agent.presence = row.presence;
        if (row.identity) {
            agent.model = row.identity->llm.model;
            agent.provider = row.identity->llm.provider;
            agent.capabilities = row.identity->capabilities.display_names;
        }
        if (row.status) {
            agent.context = row.status->context;
        }

        const auto agent_dir = posix::open_root_directory(row.directory_path);
        auto raw = QJsonObject();
        auto init = QJsonObject();
        if (agent_dir.get() >= 0) {
            if (const auto manifest = read_regular_file(
                    agent_dir.get(), ".agent.json", kMaxJsonBytes)) {
                raw = parse_object(*manifest);
            }
            if (const auto init_bytes = read_regular_file(
                    agent_dir.get(), "init.json", kMaxJsonBytes)) {
                init = parse_object(*init_bytes);
                overlay_init_fields(raw, init);
            }
            if (!agent.is_human) {
                std::map<std::string, KanbanTokenTotals> by_provider;
                read_token_ledger(
                    agent_dir.get(), agent.tokens, by_provider, agent.recent);
                for (auto &[name, totals] : by_provider) {
                    agent.providers.push_back({name, totals});
                }
                sort_provider_spend(agent.providers);
                add_totals(board.network_tokens, agent.tokens);
                read_daemon_ledger_summary(
                    agent_dir.get(), agent.daemon_providers, agent.daemon_recent,
                    agent.daemon_runs_scanned, agent.daemon_runs_total);
                read_molt_session_stats(
                    agent_dir.get(), row.directory_path,
                    agent.current_session, agent.last_session,
                    agent.molt_times_ms, agent.refresh_times_ms);
                agent.context_stats = read_context_stats(agent_dir.get());
                agent.mcp_names = mcp_names_from_init(init);
                agent.daemons = count_daemons(agent_dir.get());
                board.running_daemons += agent.daemons.running;
                // Inbox totals are intentionally skipped: Desktop kanban never
                // renders total_mails, and a full inbox walk is O(messages).
                // Matches TUI SkipMailEdges for the live board path.
                auto spawned = read_avatar_edges(
                    agent_dir.get(), row.directory_key.string());
                edges.insert(edges.end(), spawned.begin(), spawned.end());
            }
            if (row.role == AgentRole::main && board.orchestrator_path.empty()) {
                board.orchestrator_path = row.directory_path;
            }
            if (row.role == AgentRole::main && board.network_created.empty()) {
                if (const auto created = nonempty_string(raw, "created_at")) {
                    board.network_created = *created;
                } else if (const auto started = nonempty_string(raw, "started_at")) {
                    board.network_created = *started;
                }
            }
        }

        append_fields(agent.identity_fields, raw, {
            {"agent_name", "name"},
            {"nickname", "nickname"},
            {"agent_id", "id"},
            {"state", "state"},
            {"address", "address"},
            {"language", "language"},
            {"started_at", "started_at"},
            {"combo", "combo"},
        });
        append_fields(agent.llm_fields, raw, {
            {"model", "model"},
            {"provider", "provider"},
            {"base_url", "base_url"},
            {"api_compat", "api_compat"},
            {"api_key_env", "api_key_env"},
            {"streaming", "streaming"},
            {"context_limit", "context_limit"},
        });
        append_fields(agent.runtime_fields, raw, {
            {"stamina", "stamina"},
            {"molt_pressure", "molt_pressure"},
            {"soul_delay", "soul_delay"},
            {"molt_count", "molt_count"},
            {"max_turns", "max_turns"},
            {"max_rpm", "max_rpm"},
        });
        if (const auto admin = raw.value("admin"); admin.isObject()) {
            const auto object = admin.toObject();
            auto keys = object.keys();
            keys.sort();
            for (const auto &key : keys) {
                auto text = json_display(object.value(key));
                if (!text.empty()) {
                    agent.admin_fields.push_back({key.toStdString(), std::move(text)});
                }
            }
        }
        agent.presets = read_agent_preset_summary(attachment, row.directory_key);
        if (const auto estimated = raw.value("tokens"); estimated.isObject()
                && estimated.toObject().value("estimated").toBool()) {
            agent.tokens_estimated = true;
        }
        if (row.status) {
            // Status tokens.estimated is not projected; keep false unless JSON said so.
        }

        if (agent.is_human) ++board.human_count;
        else ++board.agent_count;

        if (!agent.is_human) {
            const auto column = kanban_column_for_state(agent.state);
            switch (column) {
            case KanbanBoardColumn::active:
                ++board.active;
                ++active_agents;
                break;
            case KanbanBoardColumn::idle:
                ++board.idle;
                has_idle = true;
                break;
            case KanbanBoardColumn::stuck:
                ++board.stuck;
                has_idle = true;
                break;
            case KanbanBoardColumn::asleep:
                ++board.asleep;
                has_asleep = true;
                break;
            case KanbanBoardColumn::suspended:
                ++board.suspended;
                break;
            }
        }
        board.agents.push_back(std::move(agent));
    }

    board.activity_status = activity_status(
        active_agents, board.running_daemons, has_idle, has_asleep);
    board.tree_lines = render_tree(board.agents, edges);
    return board;
}

} // namespace lingtai::desktop
