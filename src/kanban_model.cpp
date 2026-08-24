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

thread_local KanbanRefreshMetrics *active_refresh_metrics = nullptr;

struct RefreshMetricsScope {
    explicit RefreshMetricsScope(KanbanRefreshMetrics &metrics)
        : previous(active_refresh_metrics) {
        active_refresh_metrics = &metrics;
    }
    ~RefreshMetricsScope() { active_refresh_metrics = previous; }
    KanbanRefreshMetrics *previous = nullptr;
};

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
    if (active_refresh_metrics) {
        ++active_refresh_metrics->payload_opens;
        active_refresh_metrics->payload_bytes +=
            static_cast<std::uint64_t>(leaf.st_size);
        if (std::string_view(name) == "daemon.json") {
            ++active_refresh_metrics->daemon_records_opened;
        }
    }
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
    if (active_refresh_metrics) ++active_refresh_metrics->payload_opens;
    std::string pending;
    char buf[65536];
    for (;;) {
        const auto count = ::read(file.get(), buf, sizeof(buf));
        if (count < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (count == 0) break;
        if (active_refresh_metrics) {
            active_refresh_metrics->payload_bytes +=
                static_cast<std::uint64_t>(count);
        }
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
    // A trailing fragment is not a published JSONL record. Incremental
    // cursors retain it and project it once a later append supplies '\n'.
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
    if (active_refresh_metrics) {
        ++active_refresh_metrics->payload_opens;
        active_refresh_metrics->payload_bytes +=
            static_cast<std::uint64_t>(st.st_size);
    }
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
    const auto add_object = [&](const QJsonObject &object) {
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
        if (!content.isArray()) return;
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
    };
    auto start = std::size_t{0};
    while (start < bytes->size()) {
        const auto end = bytes->find('\n', start);
        if (end == std::string::npos) break;
        const auto line = std::string_view(*bytes).substr(
            start, end - start);
        start = end + 1;
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
        add_object(document.object());
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
        if (end == std::string::npos) break;
        const auto line = std::string_view(*bytes).substr(
            start, end - start);
        start = end + 1;
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

namespace {

struct IndexedFileStamp {
    int state = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;

    friend bool operator==(const IndexedFileStamp &, const IndexedFileStamp &)
        = default;
};

struct JsonlCursor {
    IndexedFileStamp stamp;
    std::uint64_t offset = 0;
    std::string carry;
    bool compatible = true;
    bool repair_required = false;
};

struct RebuildSourceStamps {
    IndexedFileStamp token;
    IndexedFileStamp events;
    IndexedFileStamp chat;
    IndexedFileStamp delegates;
    IndexedFileStamp sqlite;
    IndexedFileStamp sqlite_wal;
};

[[nodiscard]] IndexedFileStamp indexed_stamp_at(int parent_fd, const char *name) {
    struct stat st {};
    if (::fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return {};
    if (S_ISLNK(st.st_mode)) return {.state = 2};
    IndexedFileStamp result;
    result.state = S_ISREG(st.st_mode) ? 1 : (S_ISDIR(st.st_mode) ? 3 : 2);
    result.device = static_cast<std::uint64_t>(st.st_dev);
    result.inode = static_cast<std::uint64_t>(st.st_ino);
    result.size = st.st_size < 0 ? 0 : static_cast<std::uint64_t>(st.st_size);
    result.mtime_ns = stat_mtime_ns(st);
    return result;
}

[[nodiscard]] JsonlCursor cursor_at_end(
        int agent_fd, const char *directory, const char *name) {
    JsonlCursor cursor;
    const auto dir = posix::open_directory_component(agent_fd, directory);
    if (dir.get() < 0) return cursor;
    cursor.stamp = indexed_stamp_at(dir.get(), name);
    if (cursor.stamp.state != 1) return cursor;
    cursor.offset = cursor.stamp.size;
    if (cursor.offset == 0) return cursor;
    const auto file = posix::open_regular_file_component(dir.get(), name);
    if (file.get() < 0) {
        cursor.compatible = false;
        return cursor;
    }
    char last = '\0';
    if (::pread(file.get(), &last, 1, static_cast<off_t>(cursor.offset - 1)) != 1) {
        cursor.compatible = false;
        return cursor;
    }
    if (last == '\n') return cursor;
    constexpr auto kMaxCarry = std::uint64_t{1024U * 1024U};
    const auto begin = cursor.offset > kMaxCarry ? cursor.offset - kMaxCarry : 0;
    std::string tail(static_cast<std::size_t>(cursor.offset - begin), '\0');
    const auto count = ::pread(file.get(), tail.data(), tail.size(),
        static_cast<off_t>(begin));
    if (count < 0) {
        cursor.compatible = false;
        return cursor;
    }
    tail.resize(static_cast<std::size_t>(count));
    const auto nl = tail.rfind('\n');
    if (nl == std::string::npos && begin != 0) {
        cursor.compatible = false;
        return cursor;
    }
    cursor.carry = nl == std::string::npos ? std::move(tail)
                                           : tail.substr(nl + 1);
    return cursor;
}

[[nodiscard]] IndexedFileStamp indexed_nested_stamp(
        int agent_fd, const char *directory, const char *name) {
    const auto dir = posix::open_directory_component(agent_fd, directory);
    return dir.get() < 0 ? IndexedFileStamp{}
                         : indexed_stamp_at(dir.get(), name);
}

[[nodiscard]] RebuildSourceStamps capture_rebuild_source_stamps(
        const AgentRow &row) {
    RebuildSourceStamps stamps;
    const auto fd = posix::open_root_directory(row.directory_path);
    if (fd.get() < 0) return stamps;
    stamps.token = indexed_nested_stamp(
        fd.get(), "logs", "token_ledger.jsonl");
    stamps.events = indexed_nested_stamp(fd.get(), "logs", "events.jsonl");
    stamps.chat = indexed_nested_stamp(
        fd.get(), "history", "chat_history.jsonl");
    stamps.delegates = indexed_nested_stamp(
        fd.get(), "delegates", "ledger.jsonl");
    stamps.sqlite = indexed_nested_stamp(fd.get(), "logs", "log.sqlite");
    stamps.sqlite_wal = indexed_nested_stamp(
        fd.get(), "logs", "log.sqlite-wal");
    return stamps;
}

enum class AppendDisposition { unchanged, appended, reset };

template <typename Callback>
AppendDisposition consume_appended_jsonl(
        int agent_fd,
        const char *directory,
        const char *name,
        JsonlCursor &cursor,
        KanbanRefreshMetrics &metrics,
        Callback &&callback) {
    const auto dir = posix::open_directory_component(agent_fd, directory);
    const auto now = dir.get() < 0 ? IndexedFileStamp{}
                                   : indexed_stamp_at(dir.get(), name);
    if (cursor.repair_required) return AppendDisposition::reset;
    if (now == cursor.stamp) return AppendDisposition::unchanged;
    if (!cursor.compatible || now.state != 1 || cursor.stamp.state != 1
        || now.device != cursor.stamp.device || now.inode != cursor.stamp.inode
        || now.size < cursor.offset
        || (now.size == cursor.offset && now.mtime_ns != cursor.stamp.mtime_ns)) {
        return AppendDisposition::reset;
    }
    const auto file = posix::open_regular_file_component(dir.get(), name);
    if (file.get() < 0) return AppendDisposition::reset;
    ++metrics.payload_opens;
    const auto added = now.size - cursor.offset;
    std::string bytes(static_cast<std::size_t>(added), '\0');
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::pread(file.get(), bytes.data() + total,
            bytes.size() - total, static_cast<off_t>(cursor.offset + total));
        if (count < 0) {
            if (errno == EINTR) continue;
            return AppendDisposition::reset;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    metrics.payload_bytes += total;
    metrics.appended_bytes += total;
    cursor.offset += total;
    cursor.stamp = now;
    cursor.carry.append(bytes);
    auto start = std::size_t{0};
    for (;;) {
        const auto nl = cursor.carry.find('\n', start);
        if (nl == std::string::npos) break;
        const auto line = std::string_view(cursor.carry).substr(start, nl - start);
        callback(line);
        ++metrics.appended_lines;
        start = nl + 1;
    }
    if (start > 0) cursor.carry.erase(0, start);
    if (cursor.carry.size() > 1024U * 1024U) cursor.compatible = false;
    return AppendDisposition::appended;
}

void add_context_line(KanbanContextStats &stats, std::string_view line) {
    parse_jsonl_line(line, [&](const QJsonObject &object) {
        ++stats.entries;
        const auto role = object.value("role").toString().toStdString();
        if (role == "system") ++stats.system_messages;
        else if (role == "assistant") ++stats.assistant_messages;
        else if (role == "user") ++stats.user_messages;
        auto counts = std::map<std::string, KanbanToolCount>();
        for (const auto &old : stats.tool_counts) counts[old.name] = old;
        const auto content = object.value("content");
        if (content.isString() && !content.toString().isEmpty()) {
            if (role == "assistant") ++stats.text_outputs;
            else if (role != "system") ++stats.text_inputs;
        } else if (content.isArray()) {
            for (const auto &value : content.toArray()) {
                if (!value.isObject()) continue;
                const auto block = value.toObject();
                const auto type = block.value("type").toString().toStdString();
                auto name = block.value("name").toString().toStdString();
                if (name.empty()) name = "unknown";
                if (type == "tool_call") {
                    ++stats.tool_calls;
                    counts[name].name = name;
                    ++counts[name].calls;
                } else if (type == "tool_result") {
                    ++stats.tool_results;
                    counts[name].name = name;
                    ++counts[name].results;
                } else if (type == "text") {
                    if (role == "assistant") ++stats.text_outputs;
                    else if (role != "system") ++stats.text_inputs;
                }
            }
        }
        stats.tool_counts.clear();
        for (auto &[key, value] : counts) {
            static_cast<void>(key);
            stats.tool_counts.push_back(value);
        }
        std::sort(stats.tool_counts.begin(), stats.tool_counts.end(),
            [](const KanbanToolCount &a, const KanbanToolCount &b) {
                return a.calls != b.calls ? a.calls > b.calls : a.name < b.name;
            });
    });
}

void add_token_append(KanbanAgent &agent, std::string_view line) {
    parse_jsonl_line(line, [&](const QJsonObject &object) {
        if (is_daemon_ledger_object(object)) return;
        add_token_line(agent.tokens, object);
        auto entry = ledger_entry_from_object(object);
        auto provider = std::find_if(agent.providers.begin(), agent.providers.end(),
            [&](const KanbanProviderSpend &row) { return row.name == entry.provider; });
        if (provider == agent.providers.end()) {
            agent.providers.push_back({entry.provider, {}});
            provider = std::prev(agent.providers.end());
        }
        provider->totals.input += entry.input;
        provider->totals.output += entry.output;
        provider->totals.thinking += entry.thinking;
        provider->totals.cached += entry.cached;
        ++provider->totals.api_calls;
        sort_provider_spend(agent.providers);
        add_ledger_to_session(agent.current_session, entry);
        agent.recent.insert(agent.recent.begin(), std::move(entry));
        if (static_cast<int>(agent.recent.size()) > kRecentLedgerEntries) {
            agent.recent.resize(kRecentLedgerEntries);
        }
    });
}

[[nodiscard]] std::optional<std::int64_t> event_time_ms(
        const QJsonObject &object) {
    const auto ts = object.value("ts").toDouble();
    if (ts <= 0) return std::nullopt;
    return unix_float_utc(ts).toMSecsSinceEpoch();
}

void refresh_lifecycle(KanbanAgent &agent, const AgentRow &row) {
    agent.directory_key = row.directory_key;
    agent.directory_path = row.directory_path;
    agent.display_name = display_name(row);
    agent.state = agent_state(row);
    agent.is_human = row.role == AgentRole::human;
    agent.role = row.role;
    agent.presence = row.presence;
    agent.model.reset();
    agent.provider.reset();
    agent.capabilities.clear();
    if (row.identity) {
        agent.model = row.identity->llm.model;
        agent.provider = row.identity->llm.provider;
        agent.capabilities = row.identity->capabilities.display_names;
    }
    agent.context = row.status ? row.status->context
                               : std::optional<AgentContextFacts>();
}

[[nodiscard]] bool same_context(
        const std::optional<AgentContextFacts> &a,
        const std::optional<AgentContextFacts> &b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->window_size == b->window_size
        && a->system_tokens == b->system_tokens
        && a->tools_tokens == b->tools_tokens
        && a->history_tokens == b->history_tokens
        && a->total_tokens == b->total_tokens
        && a->fixed_tokens == b->fixed_tokens
        && a->growing_tokens == b->growing_tokens
        && a->usage_percent == b->usage_percent;
}

} // namespace

struct KanbanSnapshotIndex::Impl {
    struct AgentCache {
        struct DaemonRun {
            std::string name;
            std::int64_t directory_mtime = 0;
            IndexedFileStamp directory_stamp;
            IndexedFileStamp card_stamp;
            IndexedFileStamp ledger_stamp;
            QJsonObject card;
            std::vector<KanbanLedgerEntry> ledger;
            bool ledger_loaded = false;
            bool terminal = false;
        };
        KanbanAgent agent;
        JsonlCursor token;
        JsonlCursor events;
        JsonlCursor chat;
        JsonlCursor delegates;
        IndexedFileStamp manifest;
        IndexedFileStamp init;
        IndexedFileStamp status;
        IndexedFileStamp preset;
        IndexedFileStamp sqlite;
        IndexedFileStamp sqlite_wal;
        IndexedFileStamp daemon_directory;
        IndexedFileStamp daemon_root_card_stamp;
        QJsonObject daemon_root_card;
        std::map<std::string, DaemonRun> daemon_runs;
        QJsonObject raw;
        QJsonObject init_object;
        std::vector<AvatarEdge> edges;
        bool rebuild_required = false;
    };

    std::filesystem::path root;
    std::optional<KanbanBoard> board;
    std::map<std::string, AgentCache> agents;
    RebuildReadCompleteHook rebuild_read_complete_hook;

    static void project_static(AgentCache &cache) {
        auto raw = cache.raw;
        overlay_init_fields(raw, cache.init_object);
        cache.agent.identity_fields.clear();
        cache.agent.llm_fields.clear();
        cache.agent.runtime_fields.clear();
        cache.agent.admin_fields.clear();
        append_fields(cache.agent.identity_fields, raw, {
            {"agent_name", "name"}, {"nickname", "nickname"},
            {"agent_id", "id"}, {"state", "state"},
            {"address", "address"}, {"language", "language"},
            {"started_at", "started_at"}, {"combo", "combo"},
        });
        append_fields(cache.agent.llm_fields, raw, {
            {"model", "model"}, {"provider", "provider"},
            {"base_url", "base_url"}, {"api_compat", "api_compat"},
            {"api_key_env", "api_key_env"}, {"streaming", "streaming"},
            {"context_limit", "context_limit"},
        });
        append_fields(cache.agent.runtime_fields, raw, {
            {"stamina", "stamina"}, {"molt_pressure", "molt_pressure"},
            {"soul_delay", "soul_delay"}, {"molt_count", "molt_count"},
            {"max_turns", "max_turns"}, {"max_rpm", "max_rpm"},
        });
        if (const auto admin = raw.value("admin"); admin.isObject()) {
            const auto object = admin.toObject();
            auto keys = object.keys();
            keys.sort();
            for (const auto &key : keys) {
                auto value = json_display(object.value(key));
                if (!value.empty()) {
                    cache.agent.admin_fields.push_back(
                        {key.toStdString(), std::move(value)});
                }
            }
        }
        cache.agent.mcp_names = mcp_names_from_init(cache.init_object);
        cache.agent.tokens_estimated = false;
        if (const auto tokens = raw.value("tokens"); tokens.isObject()) {
            cache.agent.tokens_estimated =
                tokens.toObject().value("estimated").toBool();
        }
    }

    static void update_terminal(AgentCache::DaemonRun &run) {
        const auto state = ascii_lower(
            run.card.value("state").toString().toStdString());
        const auto finished = run.card.value("finished_at");
        const auto finished_text = finished.isString()
            ? finished.toString().trimmed().toStdString() : std::string();
        run.terminal = (!finished.isUndefined() && !finished.isNull()
            && (!finished.isString() || !finished_text.empty()))
            || state == "done" || state == "completed" || state == "failed"
            || state == "cancelled";
    }

    static void read_run_card(int daemons_fd, AgentCache::DaemonRun &run) {
        const auto directory = posix::open_directory_component(
            daemons_fd, run.name);
        if (directory.get() < 0) {
            run.card = {};
            run.card_stamp = {};
            run.terminal = false;
            return;
        }
        run.card_stamp = indexed_stamp_at(directory.get(), "daemon.json");
        run.card = {};
        if (const auto bytes = read_regular_file(
                directory.get(), "daemon.json", kMaxDaemonBytes)) {
            run.card = parse_object(*bytes);
        }
        update_terminal(run);
    }

    static void read_run_ledger(int daemons_fd, AgentCache::DaemonRun &run) {
        const auto directory = posix::open_directory_component(
            daemons_fd, run.name);
        if (directory.get() < 0) return;
        run.ledger.clear();
        const auto logs = posix::open_directory_component(directory.get(), "logs");
        run.ledger_stamp = logs.get() < 0 ? IndexedFileStamp{}
            : indexed_stamp_at(logs.get(), "token_ledger.jsonl");
        for_each_nested_jsonl(directory.get(), "logs", "token_ledger.jsonl",
            [&](const QJsonObject &object) {
                run.ledger.push_back(ledger_entry_from_object(object));
            });
        run.ledger_loaded = true;
    }

    static KanbanTokenTotals fallback_daemon_totals(const QJsonObject &card) {
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
        if (fallback.spend() + fallback.cached == 0 && fallback.api_calls == 0
                && legacy.isObject()) {
            const auto object = legacy.toObject();
            fallback.input = json_int64(object.value("input")).value_or(0);
            fallback.output = json_int64(object.value("output")).value_or(0);
            fallback.thinking = json_int64(object.value("thinking")).value_or(0);
            fallback.cached = json_int64(object.value("cached")).value_or(0);
        }
        return fallback;
    }

    static void project_daemons(int daemons_fd, AgentCache &cache) {
        struct Ref { std::string name; std::int64_t mtime = 0; };
        std::vector<Ref> refs;
        refs.reserve(cache.daemon_runs.size());
        for (const auto &[name, run] : cache.daemon_runs) {
            refs.push_back({name, run.directory_mtime});
        }
        std::sort(refs.begin(), refs.end(), [](const Ref &a, const Ref &b) {
            return a.mtime != b.mtime ? a.mtime > b.mtime : a.name > b.name;
        });
        cache.agent.daemon_runs_total = static_cast<int>(refs.size());
        if (refs.size() > kRecentDaemonRuns) refs.resize(kRecentDaemonRuns);
        cache.agent.daemon_runs_scanned = static_cast<int>(refs.size());
        std::map<std::string, KanbanTokenTotals> providers;
        std::vector<KanbanDaemonLedgerEntry> recent;
        for (const auto &ref : refs) {
            auto &run = cache.daemon_runs.at(ref.name);
            if (!run.ledger_loaded) read_run_ledger(daemons_fd, run);
            auto run_id = run.card.value("run_id").toString().toStdString();
            if (run_id.empty()) run_id = run.name;
            const auto handle = run.card.value("handle").toString().toStdString();
            const auto state = run.card.value("state").toString().toStdString();
            const auto backend = run.card.value("backend").toString().toStdString();
            if (!run.ledger.empty()) {
                for (const auto &entry : run.ledger) {
                    add_provider_totals(providers, entry.provider, {
                        entry.input, entry.output, entry.thinking, entry.cached, 1});
                    KanbanDaemonLedgerEntry tagged;
                    static_cast<KanbanLedgerEntry &>(tagged) = entry;
                    tagged.run_id = run_id;
                    tagged.handle = handle;
                    tagged.state = state;
                    tagged.backend = backend;
                    recent.push_back(std::move(tagged));
                }
            } else {
                const auto fallback = fallback_daemon_totals(run.card);
                if (fallback.spend() + fallback.cached != 0
                        || fallback.api_calls != 0) {
                    add_provider_totals(
                        providers, daemon_fallback_provider(run.card), fallback);
                }
            }
        }
        cache.agent.daemon_providers.clear();
        for (auto &[name, totals] : providers) {
            cache.agent.daemon_providers.push_back({name, totals});
        }
        sort_provider_spend(cache.agent.daemon_providers);
        std::sort(recent.begin(), recent.end(),
            [](const KanbanDaemonLedgerEntry &a,
                    const KanbanDaemonLedgerEntry &b) { return a.ts > b.ts; });
        if (recent.size() > kRecentLedgerEntries) recent.resize(kRecentLedgerEntries);
        cache.agent.daemon_recent = std::move(recent);
        cache.agent.daemons = {};
        const auto count_card = [&](const QJsonObject &card) {
            if (card.isEmpty()) return;
            ++cache.agent.daemons.total;
            const auto state = ascii_lower(
                card.value("state").toString().toStdString());
            const auto finished = card.value("finished_at");
            const auto finished_text = finished.isString()
                ? finished.toString().trimmed().toStdString() : std::string();
            const auto done = !finished.isUndefined() && !finished.isNull()
                && (!finished.isString() || !finished_text.empty());
            if (!done && (state == "running" || state == "active")) {
                ++cache.agent.daemons.running;
            }
        };
        count_card(cache.daemon_root_card);
        for (const auto &[name, run] : cache.daemon_runs) {
            static_cast<void>(name);
            count_card(run.card);
        }
    }

    static void capture_daemons(int agent_fd, AgentCache &cache) {
        const auto daemons = posix::open_directory_component(agent_fd, "daemons");
        if (daemons.get() < 0) return;
        if (active_refresh_metrics) {
            ++active_refresh_metrics->daemon_enumerations;
        }
        cache.daemon_directory = indexed_stamp_at(agent_fd, "daemons");
        cache.daemon_root_card_stamp = indexed_stamp_at(
            daemons.get(), "daemon.json");
        if (const auto bytes = read_regular_file(
                daemons.get(), "daemon.json", kMaxDaemonBytes)) {
            cache.daemon_root_card = parse_object(*bytes);
        }
        for (const auto &name : list_directory_names(daemons.get())) {
            if (!posix::safe_leaf(name)) continue;
            struct stat st {};
            if (::fstatat(daemons.get(), name.c_str(), &st,
                    AT_SYMLINK_NOFOLLOW) != 0
                || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) continue;
            AgentCache::DaemonRun run;
            run.name = name;
            run.directory_mtime = stat_mtime_ns(st);
            run.directory_stamp = indexed_stamp_at(daemons.get(), name.c_str());
            read_run_card(daemons.get(), run);
            cache.daemon_runs.emplace(name, std::move(run));
        }
        project_daemons(daemons.get(), cache);
    }

    static bool refresh_daemons(
            int agent_fd, AgentCache &cache, KanbanRefreshMetrics &metrics) {
        const auto daemons = posix::open_directory_component(agent_fd, "daemons");
        const auto directory_stamp = indexed_stamp_at(agent_fd, "daemons");
        if (daemons.get() < 0) {
            if (directory_stamp == cache.daemon_directory) return false;
            cache.daemon_directory = directory_stamp;
            cache.daemon_root_card = {};
            cache.daemon_root_card_stamp = {};
            cache.daemon_runs.clear();
            cache.agent.daemon_providers.clear();
            cache.agent.daemon_recent.clear();
            cache.agent.daemons = {};
            cache.agent.daemon_runs_scanned = 0;
            cache.agent.daemon_runs_total = 0;
            ++metrics.daemon_enumerations;
            return true;
        }
        auto changed = false;
        const auto root_card_stamp = indexed_stamp_at(daemons.get(), "daemon.json");
        if (root_card_stamp != cache.daemon_root_card_stamp) {
            cache.daemon_root_card_stamp = root_card_stamp;
            cache.daemon_root_card = {};
            if (const auto bytes = read_regular_file(
                    daemons.get(), "daemon.json", kMaxDaemonBytes)) {
                cache.daemon_root_card = parse_object(*bytes);
            }
            changed = true;
        }
        if (directory_stamp != cache.daemon_directory) {
            ++metrics.daemon_enumerations;
            cache.daemon_directory = directory_stamp;
            auto present = std::set<std::string>();
            for (const auto &name : list_directory_names(daemons.get())) {
                if (!posix::safe_leaf(name)) continue;
                struct stat st {};
                if (::fstatat(daemons.get(), name.c_str(), &st,
                        AT_SYMLINK_NOFOLLOW) != 0
                    || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) continue;
                present.insert(name);
                auto found = cache.daemon_runs.find(name);
                if (found == cache.daemon_runs.end()) {
                    AgentCache::DaemonRun run;
                    run.name = name;
                    run.directory_mtime = stat_mtime_ns(st);
                    run.directory_stamp = indexed_stamp_at(
                        daemons.get(), name.c_str());
                    read_run_card(daemons.get(), run);
                    cache.daemon_runs.emplace(name, std::move(run));
                    changed = true;
                } else {
                    found->second.directory_mtime = stat_mtime_ns(st);
                    const auto run_stamp = indexed_stamp_at(
                        daemons.get(), name.c_str());
                    if (run_stamp.device != found->second.directory_stamp.device
                            || run_stamp.inode
                                != found->second.directory_stamp.inode) {
                        found->second.directory_stamp = run_stamp;
                        found->second.ledger.clear();
                        found->second.ledger_loaded = false;
                        read_run_card(daemons.get(), found->second);
                        changed = true;
                    } else {
                        found->second.directory_stamp = run_stamp;
                    }
                }
            }
            for (auto it = cache.daemon_runs.begin(); it != cache.daemon_runs.end();) {
                if (!present.contains(it->first)) {
                    it = cache.daemon_runs.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        for (auto &[name, run] : cache.daemon_runs) {
            if (run.terminal) continue;
            const auto directory = posix::open_directory_component(
                daemons.get(), name);
            if (directory.get() < 0) continue;
            const auto card_stamp = indexed_stamp_at(directory.get(), "daemon.json");
            const auto logs = posix::open_directory_component(directory.get(), "logs");
            const auto ledger_stamp = logs.get() < 0 ? IndexedFileStamp{}
                : indexed_stamp_at(logs.get(), "token_ledger.jsonl");
            if (card_stamp != run.card_stamp) {
                read_run_card(daemons.get(), run);
                changed = true;
            }
            if (run.ledger_loaded && ledger_stamp != run.ledger_stamp) {
                read_run_ledger(daemons.get(), run);
                changed = true;
            }
        }
        if (changed) project_daemons(daemons.get(), cache);
        return changed;
    }

    static bool refresh_sqlite_session(
            int agent_fd, const AgentRow &row, AgentCache &cache,
            bool &needs_repartition) {
        const auto logs = posix::open_directory_component(agent_fd, "logs");
        const auto sqlite_stamp = logs.get() < 0 ? IndexedFileStamp{}
            : indexed_stamp_at(logs.get(), "log.sqlite");
        const auto wal_stamp = logs.get() < 0 ? IndexedFileStamp{}
            : indexed_stamp_at(logs.get(), "log.sqlite-wal");
        if (sqlite_stamp == cache.sqlite && wal_stamp == cache.sqlite_wal) {
            return false;
        }
        const auto had_sqlite = cache.sqlite.state == 1;
        cache.sqlite = sqlite_stamp;
        cache.sqlite_wal = wal_stamp;
        if (sqlite_stamp.state != 1) {
            needs_repartition = had_sqlite;
            return true;
        }
        SqliteCloser sqlite;
        sqlite.db = open_log_sqlite(agent_fd, row.directory_path);
        MoltWindows windows;
        if (!sqlite.db || !read_molt_windows_sqlite(sqlite.db, windows)) {
            needs_repartition = true;
            return true;
        }
        auto current_tools = std::int64_t{0};
        auto last_tools = std::int64_t{0};
        if (!read_tool_call_counts_sqlite(
                sqlite.db, windows, current_tools, last_tools)) {
            needs_repartition = true;
            return true;
        }
        const auto molts = sqlite_event_times_ms(
            sqlite.db, "psyche_molt", kRecentLedgerEntries);
        const auto boundary = [](const std::vector<std::int64_t> &values,
                std::size_t index) {
            return index < values.size() ? values[index] : std::int64_t{0};
        };
        if (boundary(molts, 0) != boundary(cache.agent.molt_times_ms, 0)
            || boundary(molts, 1) != boundary(cache.agent.molt_times_ms, 1)) {
            needs_repartition = true;
            return true;
        }
        cache.agent.current_session.tool_calls = current_tools;
        cache.agent.last_session.tool_calls = last_tools;
        cache.agent.molt_times_ms = molts;
        cache.agent.refresh_times_ms = sqlite_event_times_ms(
            sqlite.db, "refresh_complete", kRecentLedgerEntries);
        return true;
    }

    static void validate_rebuild_sources(
            AgentCache &cache, const RebuildSourceStamps &before) {
        const auto invalidate = [](JsonlCursor &cursor, bool moved) {
            if (!moved) return;
            cursor.compatible = false;
            cursor.repair_required = true;
        };
        invalidate(cache.token, before.token != cache.token.stamp);
        invalidate(cache.events, before.events != cache.events.stamp);
        invalidate(cache.chat, before.chat != cache.chat.stamp);
        invalidate(cache.delegates,
            before.delegates != cache.delegates.stamp);
        cache.rebuild_required = before.sqlite != cache.sqlite
            || before.sqlite_wal != cache.sqlite_wal;
    }

    void capture_agent(const AgentRow &row, KanbanAgent agent,
            const RebuildSourceStamps *before = nullptr) {
        AgentCache cache;
        cache.agent = std::move(agent);
        const auto fd = posix::open_root_directory(row.directory_path);
        if (fd.get() >= 0) {
            cache.token = cursor_at_end(fd.get(), "logs", "token_ledger.jsonl");
            cache.events = cursor_at_end(fd.get(), "logs", "events.jsonl");
            cache.chat = cursor_at_end(fd.get(), "history", "chat_history.jsonl");
            cache.manifest = indexed_stamp_at(fd.get(), ".agent.json");
            cache.init = indexed_stamp_at(fd.get(), "init.json");
            cache.status = indexed_stamp_at(fd.get(), ".status.json");
            cache.daemon_directory = indexed_stamp_at(fd.get(), "daemons");
            const auto logs = posix::open_directory_component(fd.get(), "logs");
            if (logs.get() >= 0) {
                cache.sqlite = indexed_stamp_at(logs.get(), "log.sqlite");
                cache.sqlite_wal = indexed_stamp_at(
                    logs.get(), "log.sqlite-wal");
            }
            const auto system = posix::open_directory_component(fd.get(), "system");
            if (system.get() >= 0) {
                cache.preset = indexed_stamp_at(
                    system.get(), "manifest.resolved.json");
            }
            if (!cache.agent.is_human) {
                cache.edges = read_avatar_edges(
                    fd.get(), row.directory_key.string());
                cache.delegates = cursor_at_end(
                    fd.get(), "delegates", "ledger.jsonl");
                capture_daemons(fd.get(), cache);
            } else {
                cache.delegates = cursor_at_end(
                    fd.get(), "delegates", "ledger.jsonl");
            }
            if (const auto bytes = read_regular_file(
                    fd.get(), ".agent.json", kMaxJsonBytes)) {
                cache.raw = parse_object(*bytes);
            }
            if (const auto bytes = read_regular_file(
                    fd.get(), "init.json", kMaxJsonBytes)) {
                cache.init_object = parse_object(*bytes);
            }
            if (before && !cache.agent.is_human) {
                validate_rebuild_sources(cache, *before);
            }
        }
        agents[row.directory_key.string()] = std::move(cache);
    }

    void rebuild_all(const ProjectAttachment &attachment,
            const AgentSnapshot &snapshot, KanbanRefreshMetrics &metrics) {
        auto before = std::map<std::string, RebuildSourceStamps>();
        for (const auto &row : snapshot.items) {
            if (row.manifest_kind == AgentManifestKind::valid
                    && row.role != AgentRole::human) {
                before.emplace(row.directory_key.string(),
                    capture_rebuild_source_stamps(row));
            }
        }
        board = read_kanban_board(attachment, snapshot);
        agents.clear();
        metrics.full_agent_rebuilds += snapshot.items.size();
        if (!board) return;
        for (const auto &row : snapshot.items) {
            if (row.manifest_kind != AgentManifestKind::valid) continue;
            auto found = std::find_if(board->agents.begin(), board->agents.end(),
                [&](const KanbanAgent &agent) {
                    return agent.directory_key == row.directory_key;
                });
            if (found != board->agents.end()) {
                if (rebuild_read_complete_hook) rebuild_read_complete_hook(row);
                const auto stamps = before.find(row.directory_key.string());
                capture_agent(row, *found,
                    stamps == before.end() ? nullptr : &stamps->second);
            }
        }
    }

    void rebuild_agent(const ProjectAttachment &attachment,
            const AgentRow &row, KanbanRefreshMetrics &metrics) {
        const auto before = capture_rebuild_source_stamps(row);
        AgentSnapshot one;
        one.items.push_back(row);
        auto fresh = read_kanban_board(attachment, one);
        ++metrics.full_agent_rebuilds;
        if (fresh.agents.empty()) {
            agents.erase(row.directory_key.string());
            return;
        }
        if (rebuild_read_complete_hook) rebuild_read_complete_hook(row);
        capture_agent(row, std::move(fresh.agents.front()), &before);
        if (row.role == AgentRole::main && board) {
            if (!fresh.network_created.empty()) {
                board->network_created = std::move(fresh.network_created);
            }
            board->orchestrator_path = std::move(fresh.orchestrator_path);
        }
    }

    void compose(const AgentSnapshot &snapshot) {
        if (!board) board.emplace();
        auto next = KanbanBoard();
        next.network_root = board->network_root;
        next.network_created = board->network_created;
        next.orchestrator_path = board->orchestrator_path;
        std::vector<AvatarEdge> all_edges;
        auto active_agents = 0;
        auto has_idle = false;
        auto has_asleep = false;
        for (const auto &row : snapshot.items) {
            if (row.manifest_kind != AgentManifestKind::valid) continue;
            const auto found = agents.find(row.directory_key.string());
            if (found == agents.end()) continue;
            auto agent = found->second.agent;
            refresh_lifecycle(agent, row);
            found->second.agent = agent;
            all_edges.insert(all_edges.end(), found->second.edges.begin(),
                found->second.edges.end());
            if (agent.is_human) ++next.human_count;
            else {
                ++next.agent_count;
                add_totals(next.network_tokens, agent.tokens);
                next.running_daemons += agent.daemons.running;
                switch (kanban_column_for_state(agent.state)) {
                case KanbanBoardColumn::active: ++next.active; ++active_agents; break;
                case KanbanBoardColumn::idle: ++next.idle; has_idle = true; break;
                case KanbanBoardColumn::stuck: ++next.stuck; has_idle = true; break;
                case KanbanBoardColumn::asleep: ++next.asleep; has_asleep = true; break;
                case KanbanBoardColumn::suspended: ++next.suspended; break;
                }
            }
            next.agents.push_back(std::move(agent));
        }
        next.activity_status = activity_status(
            active_agents, next.running_daemons, has_idle, has_asleep);
        next.tree_lines = render_tree(next.agents, all_edges);
        board = std::move(next);
    }

    [[nodiscard]] bool has_pending_source_change(
            const AgentSnapshot &snapshot) const {
        for (const auto &row : snapshot.items) {
            if (row.manifest_kind != AgentManifestKind::valid) continue;
            const auto found = agents.find(row.directory_key.string());
            if (found == agents.end()) return true;
            const auto &cache = found->second;
            const auto fd = posix::open_root_directory(row.directory_path);
            if (fd.get() < 0) return true;
            if (cache.manifest != indexed_stamp_at(fd.get(), ".agent.json")
                || cache.init != indexed_stamp_at(fd.get(), "init.json")
                || cache.status != indexed_stamp_at(fd.get(), ".status.json")) {
                return true;
            }
            auto preset = IndexedFileStamp();
            const auto system = posix::open_directory_component(fd.get(), "system");
            if (system.get() >= 0) {
                preset = indexed_stamp_at(system.get(), "manifest.resolved.json");
            }
            if (preset != cache.preset) return true;
            if (row.role == AgentRole::human) continue;
            if (cache.rebuild_required || cache.token.repair_required
                || cache.events.repair_required || cache.chat.repair_required
                || cache.delegates.repair_required
                || cache.daemon_directory
                    != indexed_stamp_at(fd.get(), "daemons")) return true;
            const auto logs = posix::open_directory_component(fd.get(), "logs");
            const auto history = posix::open_directory_component(fd.get(), "history");
            const auto delegates = posix::open_directory_component(fd.get(), "delegates");
            if ((logs.get() < 0 ? IndexedFileStamp{}
                    : indexed_stamp_at(logs.get(), "token_ledger.jsonl"))
                    != cache.token.stamp
                || (logs.get() < 0 ? IndexedFileStamp{}
                    : indexed_stamp_at(logs.get(), "events.jsonl"))
                    != cache.events.stamp
                || (history.get() < 0 ? IndexedFileStamp{}
                    : indexed_stamp_at(history.get(), "chat_history.jsonl"))
                    != cache.chat.stamp
                || (delegates.get() < 0 ? IndexedFileStamp{}
                    : indexed_stamp_at(delegates.get(), "ledger.jsonl"))
                    != cache.delegates.stamp
                || (logs.get() < 0 ? IndexedFileStamp{}
                    : indexed_stamp_at(logs.get(), "log.sqlite"))
                    != cache.sqlite
                || (logs.get() < 0 ? IndexedFileStamp{}
                    : indexed_stamp_at(logs.get(), "log.sqlite-wal"))
                    != cache.sqlite_wal) return true;
        }
        return false;
    }
};

KanbanSnapshotIndex::KanbanSnapshotIndex()
    : impl_(std::make_unique<Impl>()) {
}

KanbanSnapshotIndex::~KanbanSnapshotIndex() = default;
KanbanSnapshotIndex::KanbanSnapshotIndex(KanbanSnapshotIndex &&) noexcept = default;
KanbanSnapshotIndex &KanbanSnapshotIndex::operator=(
    KanbanSnapshotIndex &&) noexcept = default;

void KanbanSnapshotIndex::reset() noexcept {
    impl_ = std::make_unique<Impl>();
}

const KanbanBoard *KanbanSnapshotIndex::current() const noexcept {
    return impl_->board ? &*impl_->board : nullptr;
}

void KanbanSnapshotIndex::set_rebuild_read_complete_hook(
        RebuildReadCompleteHook hook) {
    impl_->rebuild_read_complete_hook = std::move(hook);
}

KanbanRefreshResult KanbanSnapshotIndex::refresh(
        const ProjectAttachment &attachment,
        const AgentSnapshot &snapshot,
        bool force) noexcept {
    KanbanRefreshResult result;
    try {
        RefreshMetricsScope metrics_scope(result.metrics);
        const auto root = attachment.root();
        if (!impl_->board || impl_->root != root || force) {
            impl_->root = root;
            impl_->rebuild_all(attachment, snapshot, result.metrics);
            impl_->compose(snapshot);
            result.board = *impl_->board;
            result.changed = true;
            result.follow_up = impl_->has_pending_source_change(snapshot);
            return result;
        }

        auto desired_keys = std::set<std::string>();
        for (const auto &row : snapshot.items) {
            if (row.manifest_kind == AgentManifestKind::valid) {
                desired_keys.insert(row.directory_key.string());
            }
        }
        auto cached_keys = std::set<std::string>();
        for (const auto &[key, value] : impl_->agents) {
            static_cast<void>(value);
            cached_keys.insert(key);
        }
        if (desired_keys != cached_keys) {
            impl_->rebuild_all(attachment, snapshot, result.metrics);
            impl_->compose(snapshot);
            result.board = *impl_->board;
            result.changed = true;
            result.follow_up = impl_->has_pending_source_change(snapshot);
            return result;
        }

        auto changed = false;
        for (const auto &row : snapshot.items) {
            if (row.manifest_kind != AgentManifestKind::valid) continue;
            auto &cache = impl_->agents.at(row.directory_key.string());
            const auto before_state = cache.agent.state;
            const auto before_presence = cache.agent.presence;
            const auto before_context = cache.agent.context;
            refresh_lifecycle(cache.agent, row);
            if (cache.agent.state != before_state
                || cache.agent.presence != before_presence
                || !same_context(cache.agent.context, before_context)) {
                changed = true;
            }
            const auto fd = posix::open_root_directory(row.directory_path);
            if (fd.get() < 0) {
                impl_->rebuild_agent(attachment, row, result.metrics);
                changed = true;
                continue;
            }
            auto preset = IndexedFileStamp();
            const auto system = posix::open_directory_component(fd.get(), "system");
            if (system.get() >= 0) {
                preset = indexed_stamp_at(system.get(), "manifest.resolved.json");
            }
            const auto manifest = indexed_stamp_at(fd.get(), ".agent.json");
            const auto init = indexed_stamp_at(fd.get(), "init.json");
            const auto status = indexed_stamp_at(fd.get(), ".status.json");
            const auto manifest_changed = cache.manifest != manifest;
            const auto init_changed = cache.init != init;
            const auto status_changed = cache.status != status;
            const auto preset_changed = cache.preset != preset;
            if (manifest_changed) {
                cache.manifest = manifest;
                cache.raw = {};
                if (const auto bytes = read_regular_file(
                        fd.get(), ".agent.json", kMaxJsonBytes)) {
                    cache.raw = parse_object(*bytes);
                }
            }
            if (init_changed) {
                cache.init = init;
                cache.init_object = {};
                if (const auto bytes = read_regular_file(
                        fd.get(), "init.json", kMaxJsonBytes)) {
                    cache.init_object = parse_object(*bytes);
                }
            }
            if (status_changed) cache.status = status;
            if (preset_changed) {
                cache.preset = preset;
                cache.agent.presets = read_agent_preset_summary(
                    attachment, row.directory_key);
            }
            if (manifest_changed || init_changed || status_changed
                    || preset_changed) {
                Impl::project_static(cache);
                if (row.role == AgentRole::main && impl_->board) {
                    auto raw = cache.raw;
                    overlay_init_fields(raw, cache.init_object);
                    impl_->board->network_created =
                        nonempty_string(raw, "created_at")
                            .value_or(nonempty_string(raw, "started_at")
                                .value_or(std::string()));
                    impl_->board->orchestrator_path = row.directory_path;
                }
                changed = true;
            }
            if (cache.agent.is_human) continue;

            if (cache.rebuild_required) {
                impl_->rebuild_agent(attachment, row, result.metrics);
                changed = true;
                continue;
            }

            changed = Impl::refresh_daemons(
                fd.get(), cache, result.metrics) || changed;

            auto reset = false;
            auto sqlite_repartition = false;
            changed = Impl::refresh_sqlite_session(
                fd.get(), row, cache, sqlite_repartition) || changed;
            reset = reset || sqlite_repartition;
            const auto sqlite_active = cache.sqlite.state == 1;
            const auto token = consume_appended_jsonl(fd.get(), "logs",
                "token_ledger.jsonl", cache.token, result.metrics,
                [&](std::string_view line) { add_token_append(cache.agent, line); });
            reset = reset || token == AppendDisposition::reset;
            changed = changed || token == AppendDisposition::appended;

            auto saw_molt = false;
            const auto events = consume_appended_jsonl(fd.get(), "logs",
                "events.jsonl", cache.events, result.metrics,
                [&](std::string_view line) {
                    parse_jsonl_line(line, [&](const QJsonObject &object) {
                        if (sqlite_active) return;
                        const auto type = object.value("type").toString();
                        if (type == QStringLiteral("tool_call")) {
                            ++cache.agent.current_session.tool_calls;
                        } else if (type == QStringLiteral("psyche_molt")) {
                            saw_molt = true;
                        } else if (type == QStringLiteral("refresh_complete")) {
                            if (const auto time = event_time_ms(object)) {
                                cache.agent.refresh_times_ms.insert(
                                    cache.agent.refresh_times_ms.begin(), *time);
                                if (cache.agent.refresh_times_ms.size()
                                        > kRecentLedgerEntries) {
                                    cache.agent.refresh_times_ms.resize(
                                        kRecentLedgerEntries);
                                }
                            }
                        }
                    });
                });
            reset = reset || events == AppendDisposition::reset || saw_molt;
            changed = changed || events == AppendDisposition::appended;

            const auto chat = consume_appended_jsonl(fd.get(), "history",
                "chat_history.jsonl", cache.chat, result.metrics,
                [&](std::string_view line) {
                    add_context_line(cache.agent.context_stats, line);
                });
            reset = reset || chat == AppendDisposition::reset;
            changed = changed || chat == AppendDisposition::appended;

            const auto delegates = consume_appended_jsonl(fd.get(), "delegates",
                "ledger.jsonl", cache.delegates, result.metrics,
                [&](std::string_view line) {
                    parse_jsonl_line(line, [&](const QJsonObject &object) {
                        if (object.value("event").toString()
                                != QStringLiteral("avatar")) return;
                        auto working = object.value("working_dir")
                            .toString().toStdString();
                        if (working.empty()) return;
                        const auto slash = working.find_last_of("/\\");
                        auto child = slash == std::string::npos
                            ? working : working.substr(slash + 1);
                        if (child.empty()) return;
                        cache.edges.push_back({row.directory_key.string(),
                            std::move(child),
                            object.value("name").toString().toStdString()});
                    });
                });
            reset = reset || delegates == AppendDisposition::reset;
            changed = changed || delegates == AppendDisposition::appended;
            if (reset) {
                impl_->rebuild_agent(attachment, row, result.metrics);
                changed = true;
            }
        }
        if (changed) impl_->compose(snapshot);
        result.board = *impl_->board;
        result.changed = changed;
        result.follow_up = impl_->has_pending_source_change(snapshot);
    } catch (...) {
        result.current = false;
        if (impl_->board) result.board = *impl_->board;
    }
    return result;
}

} // namespace lingtai::desktop
