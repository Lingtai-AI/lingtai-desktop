#include "agent_lifecycle.h"

#include "agent_signal.h"
#include "agent_sleep.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLatin1StringView>
#include <QtCore/QTimer>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;
using SteadyTime = std::chrono::steady_clock::time_point;

constexpr auto kSleepObservation = std::chrono::seconds(3);
constexpr auto kSuspendObservation = std::chrono::seconds(10);
constexpr auto kLeaseWait = std::chrono::seconds(60);
constexpr auto kTerminateWait = std::chrono::seconds(2);
constexpr auto kKillWait = std::chrono::seconds(1);
constexpr auto kPostKillLeaseWait = std::chrono::seconds(5);
constexpr auto kLaunchInitialWait = std::chrono::seconds(10);
constexpr auto kLaunchCap = std::chrono::seconds(60);
constexpr auto kLaunchVisibilityGrace = std::chrono::milliseconds(500);
constexpr auto kClearObservation = std::chrono::seconds(30);
constexpr auto kTemporarySuspend = std::chrono::seconds(10);
constexpr auto kHeartbeatAliveSeconds = 5.0;
constexpr auto kMaximumLeafBytes = std::size_t{1} << 20;
constexpr auto kMaximumEventSuffixBytes = std::size_t{64} << 10;

enum class LeaseObservation { free, held, unavailable };

struct EventBaseline {
    bool available = false;
    std::uintmax_t offset = 0;
    bool ends_with_newline = true;
};

struct PresetAuthorization {
    std::string reference;
    bool is_default = false;
};

bool eligible_agent(const AgentRow &row) {
    return row.manifest_kind == AgentManifestKind::valid
        && (row.role == AgentRole::main || row.role == AgentRole::agent);
}

const AgentRow *row_for_key(
        const AgentSnapshot &snapshot, const fs::path &key) {
    const auto found = std::ranges::find_if(snapshot.items,
        [&](const AgentRow &row) { return row.directory_key == key; });
    return found == snapshot.items.end() ? nullptr : &*found;
}

bool contains_ascii_space(std::string_view value) {
    return std::ranges::any_of(value, [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    });
}

bool open_agent_directory(const ProjectAttachment &attachment,
        const fs::path &agent_key, posix::FileDescriptor &agent) {
    if (!posix::safe_leaf(agent_key)) return false;
    const auto root = posix::open_root_directory(attachment.root());
    if (root.get() < 0) return false;
    const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return false;
    agent = posix::open_directory_component(lingtai.get(), agent_key);
    return agent.get() >= 0;
}

std::optional<std::string> read_bounded_file(
        int parent_fd, const fs::path &leaf, std::size_t cap,
        struct stat *opened_out = nullptr) {
    auto file = posix::open_regular_file_component(parent_fd, leaf);
    if (file.get() < 0) return std::nullopt;
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)
        || opened.st_size < 0
        || static_cast<std::uintmax_t>(opened.st_size) > cap) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(opened.st_size), '\0');
    auto total = std::size_t{0};
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
    if (total != bytes.size()) return std::nullopt;
    if (opened_out) *opened_out = opened;
    return bytes;
}

std::optional<double> parse_decimal(std::string_view bytes) {
    auto first = std::size_t{0};
    while (first < bytes.size()
        && (bytes[first] == ' ' || bytes[first] == '\t'
            || bytes[first] == '\r' || bytes[first] == '\n')) {
        ++first;
    }
    auto last = bytes.size();
    while (last > first
        && (bytes[last - 1] == ' ' || bytes[last - 1] == '\t'
            || bytes[last - 1] == '\r' || bytes[last - 1] == '\n')) {
        --last;
    }
    auto stream = std::istringstream(std::string(bytes.substr(first, last - first)));
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    auto value = 0.0;
    stream >> value;
    if (stream.fail() || !stream.eof() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> heartbeat_value(const ProjectAttachment &attachment,
        const fs::path &agent_key) {
    posix::FileDescriptor agent;
    if (!open_agent_directory(attachment, agent_key, agent)) return std::nullopt;
    const auto bytes = read_bounded_file(
        agent.get(), ".agent.heartbeat", 128);
    return bytes ? parse_decimal(*bytes) : std::nullopt;
}

bool heartbeat_alive(const ProjectAttachment &attachment,
        const fs::path &agent_key, double wall_now) {
    const auto value = heartbeat_value(attachment, agent_key);
    return value && std::isfinite(wall_now) && *value <= wall_now
        && wall_now - *value < kHeartbeatAliveSeconds;
}

std::optional<int> read_molt_count(const ProjectAttachment &attachment,
        const fs::path &agent_key) {
    posix::FileDescriptor agent;
    if (!open_agent_directory(attachment, agent_key, agent)) return std::nullopt;
    const auto bytes = read_bounded_file(agent.get(), ".agent.json", kMaximumLeafBytes);
    if (!bytes) return std::nullopt;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes->data(), static_cast<qsizetype>(bytes->size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto value = document.object().value(QLatin1StringView("molt_count"));
    if (!value.isDouble()) return std::nullopt;
    const auto count = value.toInteger(-1);
    if (count < 0 || count > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(count);
}

EventBaseline capture_event_baseline(const ProjectAttachment &attachment,
        const fs::path &agent_key) {
    posix::FileDescriptor agent;
    if (!open_agent_directory(attachment, agent_key, agent)) return {};
    const auto logs = posix::open_directory_component(agent.get(), "logs");
    if (logs.get() < 0) return {};
    auto file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    if (file.get() < 0) {
        struct stat missing {};
        if (::fstatat(logs.get(), "events.jsonl", &missing,
                AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT) {
            return {.available = true, .offset = 0, .ends_with_newline = true};
        }
        return {};
    }
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || opened.st_size < 0) return {};
    EventBaseline result{
        .available = true,
        .offset = static_cast<std::uintmax_t>(opened.st_size),
        .ends_with_newline = opened.st_size == 0,
    };
    if (opened.st_size > 0) {
        char last = 0;
        if (::pread(file.get(), &last, 1, opened.st_size - 1) != 1) return {};
        result.ends_with_newline = last == '\n';
    }
    return result;
}

bool observe_clear_event(const ProjectAttachment &attachment,
        const fs::path &agent_key, const EventBaseline &baseline) {
    if (!baseline.available) return false;
    posix::FileDescriptor agent;
    if (!open_agent_directory(attachment, agent_key, agent)) return false;
    const auto logs = posix::open_directory_component(agent.get(), "logs");
    if (logs.get() < 0) return false;
    auto file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    if (file.get() < 0) return false;
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || opened.st_size < 0
        || static_cast<std::uintmax_t>(opened.st_size) <= baseline.offset) {
        return false;
    }
    const auto available = static_cast<std::uintmax_t>(opened.st_size)
        - baseline.offset;
    const auto count = static_cast<std::size_t>(
        std::min<std::uintmax_t>(available, kMaximumEventSuffixBytes));
    std::string bytes(count, '\0');
    const auto read = ::pread(file.get(), bytes.data(), bytes.size(),
        static_cast<off_t>(baseline.offset));
    if (read <= 0) return false;
    bytes.resize(static_cast<std::size_t>(read));
    if (!baseline.ends_with_newline) {
        const auto newline = bytes.find('\n');
        bytes.erase(0, newline == std::string::npos ? bytes.size() : newline + 1);
    }
    auto begin = std::size_t{0};
    for (;;) {
        const auto newline = bytes.find('\n', begin);
        if (newline == std::string::npos) break;
        const auto line = std::string_view(bytes).substr(begin, newline - begin);
        begin = newline + 1;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(
            QByteArray(line.data(), static_cast<qsizetype>(line.size())), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const auto object = document.object();
        const auto type = object.value(QLatin1StringView("type")).toString();
        const auto source = object.value(QLatin1StringView("source")).toString();
        if (source == QLatin1StringView("desktop")
            && (type == QLatin1StringView("clear_received")
                || type == QLatin1StringView("psyche_molt"))) {
            return true;
        }
    }
    return false;
}

LeaseObservation observe_lease(const ProjectAttachment &attachment,
        const fs::path &agent_key) {
    posix::FileDescriptor agent;
    if (!open_agent_directory(attachment, agent_key, agent)) {
        return LeaseObservation::unavailable;
    }
    struct stat named {};
    if (::fstatat(agent.get(), ".agent.lock", &named, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? LeaseObservation::free
                              : LeaseObservation::unavailable;
    }
    if (!S_ISREG(named.st_mode)) return LeaseObservation::unavailable;
    posix::FileDescriptor lock(::openat(agent.get(), ".agent.lock",
        O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (lock.get() < 0) return LeaseObservation::unavailable;
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        return errno == EWOULDBLOCK || errno == EAGAIN
            ? LeaseObservation::held : LeaseObservation::unavailable;
    }
    static_cast<void>(::flock(lock.get(), LOCK_UN));
    return LeaseObservation::free;
}

bool remove_stale_lock_if_free(const ProjectAttachment &attachment,
        const fs::path &agent_key) {
    posix::FileDescriptor agent;
    if (!open_agent_directory(attachment, agent_key, agent)) return false;
    struct stat named {};
    if (::fstatat(agent.get(), ".agent.lock", &named, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISREG(named.st_mode)) return false;
    posix::FileDescriptor lock(::openat(agent.get(), ".agent.lock",
        O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (lock.get() < 0 || ::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        return false;
    }
    struct stat opened {};
    struct stat current {};
    const auto same = ::fstat(lock.get(), &opened) == 0
        && ::fstatat(agent.get(), ".agent.lock", &current,
            AT_SYMLINK_NOFOLLOW) == 0
        && opened.st_dev == current.st_dev && opened.st_ino == current.st_ino;
    const auto removed = same
        && ::unlinkat(agent.get(), ".agent.lock", 0) == 0;
    static_cast<void>(::flock(lock.get(), LOCK_UN));
    return removed;
}

std::optional<QJsonObject> read_init_object(const ProjectAttachment &attachment,
        const fs::path &agent_key, struct stat *opened_out = nullptr,
        posix::FileDescriptor *agent_out = nullptr) {
    posix::FileDescriptor agent;
    if (!open_agent_directory(attachment, agent_key, agent)) return std::nullopt;
    struct stat opened {};
    const auto bytes = read_bounded_file(
        agent.get(), "init.json", kMaximumLeafBytes, &opened);
    if (!bytes) return std::nullopt;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes->data(), static_cast<qsizetype>(bytes->size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    if (opened_out) *opened_out = opened;
    if (agent_out) *agent_out = std::move(agent);
    return document.object();
}

std::vector<std::string> allowed_presets(const QJsonObject &root) {
    const auto manifest = root.value(QLatin1StringView("manifest"));
    if (!manifest.isObject()) return {};
    const auto preset = manifest.toObject().value(QLatin1StringView("preset"));
    if (!preset.isObject()) return {};
    const auto allowed = preset.toObject().value(QLatin1StringView("allowed"));
    if (!allowed.isArray()) return {};
    std::vector<std::string> result;
    for (const auto &value : allowed.toArray()) {
        if (value.isString() && !value.toString().isEmpty()) {
            result.push_back(value.toString().toStdString());
        }
    }
    return result;
}

std::optional<PresetAuthorization> authorize_preset(
        const ProjectAttachment &attachment, const fs::path &agent_key,
        const std::string &query) {
    const auto root = read_init_object(attachment, agent_key);
    if (!root) return std::nullopt;
    const auto manifest = root->value(QLatin1StringView("manifest"));
    if (!manifest.isObject()) return std::nullopt;
    const auto preset_value =
        manifest.toObject().value(QLatin1StringView("preset"));
    if (!preset_value.isObject()) return std::nullopt;
    const auto preset = preset_value.toObject();
    const auto allowed = allowed_presets(*root);
    if (allowed.empty()) return std::nullopt;
    if (query.empty()) {
        const auto value = preset.value(QLatin1StringView("default"));
        if (!value.isString() || value.toString().isEmpty()) return std::nullopt;
        const auto reference = value.toString().toStdString();
        if (std::ranges::find(allowed, reference) == allowed.end()) {
            return std::nullopt;
        }
        return PresetAuthorization{reference, true};
    }
    if (const auto exact = std::ranges::find(allowed, query);
            exact != allowed.end()) {
        return PresetAuthorization{*exact, false};
    }
    std::vector<std::string> matches;
    for (const auto &reference : allowed) {
        if (fs::path(reference).stem().string() == query) {
            matches.push_back(reference);
        }
    }
    if (matches.size() != 1) return std::nullopt;
    return PresetAuthorization{matches.front(), false};
}

bool write_all(int fd, std::string_view bytes) {
    auto offset = std::size_t{0};
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset,
            bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool same_file_version(const struct stat &opened, const struct stat &current) {
    if (!S_ISREG(current.st_mode) || opened.st_dev != current.st_dev
        || opened.st_ino != current.st_ino || opened.st_size != current.st_size) {
        return false;
    }
#if defined(__APPLE__)
    return opened.st_mtimespec.tv_sec == current.st_mtimespec.tv_sec
        && opened.st_mtimespec.tv_nsec == current.st_mtimespec.tv_nsec
        && opened.st_ctimespec.tv_sec == current.st_ctimespec.tv_sec
        && opened.st_ctimespec.tv_nsec == current.st_ctimespec.tv_nsec;
#else
    return opened.st_mtim.tv_sec == current.st_mtim.tv_sec
        && opened.st_mtim.tv_nsec == current.st_mtim.tv_nsec
        && opened.st_ctim.tv_sec == current.st_ctim.tv_sec
        && opened.st_ctim.tv_nsec == current.st_ctim.tv_nsec;
#endif
}

bool apply_preset(const ProjectAttachment &attachment,
        const fs::path &agent_key, const PresetAuthorization &authorization) {
    struct stat original {};
    posix::FileDescriptor agent;
    auto root = read_init_object(attachment, agent_key, &original, &agent);
    if (!root || agent.get() < 0) return false;
    const auto authorized_now = authorize_preset(
        attachment, agent_key,
        authorization.is_default ? std::string() : authorization.reference);
    if (!authorized_now || authorized_now->reference != authorization.reference) {
        return false;
    }
    auto manifest = root->value(QLatin1StringView("manifest")).toObject();
    auto preset = manifest.value(QLatin1StringView("preset")).toObject();
    preset.insert(QLatin1StringView("active"),
        QString::fromStdString(authorization.reference));
    manifest.insert(QLatin1StringView("preset"), preset);
    root->insert(QLatin1StringView("manifest"), manifest);
    const auto rendered = QJsonDocument(*root).toJson(QJsonDocument::Indented);

    static std::atomic_uint64_t counter{0};
    const auto temp = std::string(".init.json.desktop.tmp.")
        + std::to_string(::getpid()) + "."
        + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    const auto temp_fd = ::openat(agent.get(), temp.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        original.st_mode & 0777);
    if (temp_fd < 0) return false;
    const auto wrote = write_all(temp_fd,
        std::string_view(rendered.constData(), static_cast<std::size_t>(rendered.size())));
    const auto synced = wrote && ::fsync(temp_fd) == 0;
    const auto closed = ::close(temp_fd) == 0;
    if (!synced || !closed) {
        static_cast<void>(::unlinkat(agent.get(), temp.c_str(), 0));
        return false;
    }
    struct stat current {};
    const auto unchanged = ::fstatat(agent.get(), "init.json", &current,
        AT_SYMLINK_NOFOLLOW) == 0
        && same_file_version(original, current);
    if (!unchanged || ::renameat(agent.get(), temp.c_str(),
            agent.get(), "init.json") != 0) {
        static_cast<void>(::unlinkat(agent.get(), temp.c_str(), 0));
        return false;
    }
    static_cast<void>(::fsync(agent.get()));
    return true;
}

bool clean_stale_signals(const ProjectAttachment &attachment,
        const fs::path &agent_key) {
    for (const auto kind : {
             AgentSignalKind::sleep,
             AgentSignalKind::suspend,
             AgentSignalKind::interrupt,
             AgentSignalKind::refresh,
             AgentSignalKind::refresh_taken,
         }) {
        if (remove_agent_signal(attachment, agent_key, kind)
            == AgentSignalRemoveResult::refused) {
            return false;
        }
    }
    return true;
}

std::string command_name(AgentLifecycleCommand command) {
    switch (command) {
    case AgentLifecycleCommand::sleep: return "Sleep";
    case AgentLifecycleCommand::suspend: return "Suspend";
    case AgentLifecycleCommand::cpr: return "CPR";
    case AgentLifecycleCommand::clear: return "Clear";
    case AgentLifecycleCommand::refresh: return "Refresh";
    }
    return "Lifecycle";
}

bool successful(AgentLifecycleOutcomeKind outcome) {
    return outcome == AgentLifecycleOutcomeKind::requested
        || outcome == AgentLifecycleOutcomeKind::applied
        || outcome == AgentLifecycleOutcomeKind::already_online
        || outcome == AgentLifecycleOutcomeKind::skipped;
}

} // namespace

std::vector<fs::path> resolve_lifecycle_targets(
        const AgentSnapshot &snapshot,
        const std::optional<fs::path> &selected_agent_key,
        bool all, bool live_only) noexcept {
    try {
        std::vector<fs::path> result;
        if (all) {
            for (const auto &row : snapshot.items) {
                if (!eligible_agent(row)) continue;
                if (live_only && row.presence != AgentPresenceKind::alive) continue;
                result.push_back(row.directory_key);
            }
            return result;
        }
        if (selected_agent_key) {
            if (const auto *row = row_for_key(snapshot, *selected_agent_key);
                    row && eligible_agent(*row)) {
                result.push_back(row->directory_key);
                return result;
            }
        }
        const auto main = std::ranges::find_if(snapshot.items,
            [](const AgentRow &row) {
                return eligible_agent(row) && row.role == AgentRole::main;
            });
        if (main != snapshot.items.end()) result.push_back(main->directory_key);
        return result;
    } catch (...) {
        return {};
    }
}

const char *agent_lifecycle_phase_name(AgentLifecyclePhase phase) noexcept {
    switch (phase) {
    case AgentLifecyclePhase::validation: return "validation";
    case AgentLifecyclePhase::signal_write: return "signal_write";
    case AgentLifecyclePhase::sleep_observation: return "sleep_observation";
    case AgentLifecyclePhase::suspend_observation: return "suspend_observation";
    case AgentLifecyclePhase::lease_wait: return "lease_wait";
    case AgentLifecyclePhase::process_scan: return "process_scan";
    case AgentLifecyclePhase::terminate_wait: return "terminate_wait";
    case AgentLifecyclePhase::kill_wait: return "kill_wait";
    case AgentLifecyclePhase::stale_cleanup: return "stale_cleanup";
    case AgentLifecyclePhase::preset_update: return "preset_update";
    case AgentLifecyclePhase::launch: return "launch";
    case AgentLifecyclePhase::heartbeat_wait: return "heartbeat_wait";
    case AgentLifecyclePhase::clear_observation: return "clear_observation";
    case AgentLifecyclePhase::temporary_suspend: return "temporary_suspend";
    case AgentLifecyclePhase::complete: return "complete";
    }
    return "unknown";
}

std::string agent_lifecycle_result_text(const AgentLifecycleResult &result) {
    if (result.targets.empty()) return command_name(result.command) + " had no targets.";
    if (!result.all && result.targets.size() == 1) {
        return result.targets.front().detail;
    }
    const auto ok = std::ranges::count_if(result.targets,
        [](const auto &target) { return successful(target.outcome); });
    std::ostringstream text;
    text << command_name(result.command) << " finished for " << ok << "/"
         << result.targets.size() << " Agents";
    const auto failed = result.targets.size() - static_cast<std::size_t>(ok);
    if (failed != 0) {
        text << "; " << failed << " failed: ";
        auto first = true;
        for (const auto &target : result.targets) {
            if (successful(target.outcome)) continue;
            if (!first) text << ", ";
            first = false;
            text << target.agent_key.string() << " ["
                 << agent_lifecycle_phase_name(target.phase) << "]: "
                 << target.detail;
        }
    } else {
        text << ".";
    }
    return text.str();
}

AgentLifecycleDependencies production_agent_lifecycle_dependencies() {
    return {
        .processes = {
            .observe = observe_exact_agent_processes,
            .signal = signal_exact_agent_process,
        },
        .launcher = {.launch = launch_agent},
        .clock = {
            .monotonic_now = [] { return std::chrono::steady_clock::now(); },
            .wall_seconds = [] {
                return std::chrono::duration<double>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            },
        },
    };
}

class AgentLifecycleController::Impl final {
public:
    explicit Impl(AgentLifecycleDependencies dependencies)
    : dependencies_(std::move(dependencies)) {
        const auto production = production_agent_lifecycle_dependencies();
        if (!dependencies_.processes.observe) {
            dependencies_.processes.observe = production.processes.observe;
        }
        if (!dependencies_.processes.signal) {
            dependencies_.processes.signal = production.processes.signal;
        }
        if (!dependencies_.launcher.launch) {
            dependencies_.launcher.launch = production.launcher.launch;
        }
        if (!dependencies_.clock.monotonic_now) {
            dependencies_.clock.monotonic_now = production.clock.monotonic_now;
        }
        if (!dependencies_.clock.wall_seconds) {
            dependencies_.clock.wall_seconds = production.clock.wall_seconds;
        }
        timer_.setInterval(std::max(1,
            static_cast<int>(dependencies_.poll_interval.count())));
        QObject::connect(&timer_, &QTimer::timeout, [this] { tick(); });
    }

    enum class Stage {
        begin,
        wait_sleep,
        wait_suspend,
        wait_lease,
        wait_refresh_lease,
        wait_refresh_final_lease,
        wait_term,
        wait_kill,
        wait_heartbeat,
        wait_clear,
        wait_temporary_suspend,
    };

    enum class LeasePurpose { cpr, clear };

    struct Target {
        fs::path key;
        const AgentRow *initial_row = nullptr;
        Stage stage = Stage::begin;
        LeasePurpose lease_purpose = LeasePurpose::cpr;
        SteadyTime deadline{};
        SteadyTime launch_started{};
        SteadyTime launch_cap{};
        std::optional<double> heartbeat_before;
        AgentSleepEventBaseline sleep_baseline;
        std::optional<int> molt_before;
        EventBaseline event_baseline;
        std::optional<PresetAuthorization> preset;
        std::string preflight_error;
        bool clear_completed = false;
        bool clear_timed_out = false;
        bool post_launch_cleanup_failed = false;
        AgentLaunchOutcome launch;
    };

    struct Operation {
        AgentLifecycleRequest request;
        bool all = false;
        std::vector<Target> targets;
        std::size_t index = 0;
        AgentLifecycleResult result;
        Done done;
    };

    bool pending() const noexcept { return operation_.has_value(); }

    AgentLifecycleStartResult run(AgentLifecycleRequest request, Done done) {
        if (operation_) return AgentLifecycleStartResult::busy;
        const auto all = request.argument == "all";
        const auto has_argument = !request.argument.empty();
        const auto valid = [&] {
            switch (request.command) {
            case AgentLifecycleCommand::sleep:
            case AgentLifecycleCommand::suspend:
            case AgentLifecycleCommand::cpr:
                return !has_argument || all;
            case AgentLifecycleCommand::clear:
                return !has_argument;
            case AgentLifecycleCommand::refresh:
                return !has_argument || all
                    || !contains_ascii_space(request.argument);
            }
            return false;
        }();
        if (!valid) return AgentLifecycleStartResult::invalid_argument;

        const auto live_only = all
            && (request.command == AgentLifecycleCommand::sleep
                || request.command == AgentLifecycleCommand::suspend);
        const auto keys = resolve_lifecycle_targets(request.snapshot,
            request.selected_agent_key, all, live_only);
        if (keys.empty()) return AgentLifecycleStartResult::no_target;

        const auto command = request.command;
        const auto bound_root = request.attachment.root();
        const auto generation = request.generation;
        Operation operation{
            .request = std::move(request),
            .all = all,
            .result = {
                .command = command,
                .all = all,
                .bound_project_root = bound_root,
                .bound_generation = generation,
            },
            .done = std::move(done),
        };
        operation.targets.reserve(keys.size());
        for (const auto &key : keys) {
            Target target;
            target.key = key;
            target.initial_row = row_for_key(operation.request.snapshot, key);
            if (operation.request.command == AgentLifecycleCommand::refresh) {
                const auto query = all ? std::string() : operation.request.argument;
                target.preset = authorize_preset(
                    operation.request.attachment, key, query);
                if (!target.preset) {
                    target.preflight_error = query.empty()
                        ? "Refresh refused: default preset is missing or not allowed."
                        : "Refresh refused: requested preset is not uniquely allowed.";
                }
            }
            operation.targets.push_back(std::move(target));
        }
        operation_ = std::move(operation);
        if (dependencies_.automatic_poll) timer_.start();
        drive();
        return AgentLifecycleStartResult::started;
    }

    void tick() { if (operation_) drive(); }

    void cancel() noexcept {
        timer_.stop();
        operation_.reset();
    }

private:
    SteadyTime now() const { return dependencies_.clock.monotonic_now(); }
    double wall_now() const { return dependencies_.clock.wall_seconds(); }

    fs::path agent_dir(const Target &target) const {
        return operation_->request.attachment.root()
            / ".lingtai" / target.key;
    }

    AgentProcessObservation processes(const Target &target) const {
        return dependencies_.processes.observe(agent_dir(target));
    }

    bool heartbeat_is_alive(const Target &target) const {
        return heartbeat_alive(operation_->request.attachment,
            target.key, wall_now());
    }

    void finish_target(Target &target, AgentLifecycleOutcomeKind outcome,
            AgentLifecyclePhase phase, std::string detail) {
        operation_->result.targets.push_back({
            .agent_key = target.key,
            .outcome = outcome,
            .phase = phase,
            .detail = std::move(detail),
        });
        ++operation_->index;
    }

    void finish_operation() {
        timer_.stop();
        auto result = std::move(operation_->result);
        auto done = std::move(operation_->done);
        operation_.reset();
        if (done) done(std::move(result));
    }

    bool write_signal(Target &target, AgentSignalKind signal,
            std::string failure) {
        if (write_agent_signal(operation_->request.attachment,
                target.key, signal) == AgentSignalWriteResult::written) {
            return true;
        }
        finish_target(target, AgentLifecycleOutcomeKind::failed,
            AgentLifecyclePhase::signal_write, std::move(failure));
        return false;
    }

    void begin(Target &target) {
        if (!target.preflight_error.empty()) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::validation,
                std::move(target.preflight_error));
            return;
        }
        switch (operation_->request.command) {
        case AgentLifecycleCommand::sleep: begin_sleep(target); break;
        case AgentLifecycleCommand::suspend: begin_suspend(target); break;
        case AgentLifecycleCommand::cpr: begin_cpr(target, LeasePurpose::cpr); break;
        case AgentLifecycleCommand::clear: begin_clear(target); break;
        case AgentLifecycleCommand::refresh: begin_refresh(target); break;
        }
    }

    void begin_sleep(Target &target) {
        const auto eligible = target.initial_row
            && target.initial_row->presence == AgentPresenceKind::alive
            && target.initial_row->identity && target.initial_row->identity->state
            && *target.initial_row->identity->state != "asleep"
            && *target.initial_row->identity->state != "suspended";
        if (!eligible) {
            finish_target(target, AgentLifecycleOutcomeKind::skipped,
                AgentLifecyclePhase::validation,
                "Sleep skipped: Agent is not live and awake.");
            return;
        }
        target.sleep_baseline = capture_agent_sleep_event_baseline(
            operation_->request.attachment, target.key);
        if (!write_signal(target, AgentSignalKind::sleep,
                "Sleep request was not written.")) return;
        target.deadline = now() + kSleepObservation;
        target.stage = Stage::wait_sleep;
    }

    void begin_suspend(Target &target) {
        const auto observed = processes(target);
        if (!heartbeat_is_alive(target) && observed.available
            && observed.pids.empty()) {
            finish_target(target, AgentLifecycleOutcomeKind::applied,
                AgentLifecyclePhase::complete,
                "Agent is already suspended/offline.");
            return;
        }
        if (!write_signal(target, AgentSignalKind::suspend,
                "Suspend request was not written.")) return;
        target.deadline = now() + kSuspendObservation;
        target.stage = Stage::wait_suspend;
    }

    void begin_cpr(Target &target, LeasePurpose purpose) {
        const auto observed = processes(target);
        if (heartbeat_is_alive(target)
            || (observed.available && !observed.pids.empty())) {
            if (purpose == LeasePurpose::clear) {
                if (write_signal(target, AgentSignalKind::clear,
                        "Clear request was not written.")) {
                    finish_target(target, AgentLifecycleOutcomeKind::requested,
                        AgentLifecyclePhase::signal_write,
                        "Clear requested from the live Agent.");
                }
            } else {
                finish_target(target, AgentLifecycleOutcomeKind::already_online,
                    AgentLifecyclePhase::complete,
                    "Agent is already online.");
            }
            return;
        }
        if (!observed.available) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::process_scan,
                "Process table unavailable; duplicate-safe launch refused.");
            return;
        }
        target.lease_purpose = purpose;
        target.deadline = now() + kLeaseWait;
        target.stage = Stage::wait_lease;
    }

    void begin_clear(Target &target) {
        begin_cpr(target, LeasePurpose::clear);
    }

    void begin_refresh(Target &target) {
        if (!write_signal(target, AgentSignalKind::suspend,
                "Refresh failed before suspend: signal was not written.")) return;
        target.deadline = now() + kLeaseWait;
        target.stage = Stage::wait_refresh_lease;
    }

    void poll_sleep(Target &target) {
        const auto marker = observe_agent_signal(
            operation_->request.attachment, target.key,
            AgentSignalKind::sleep);
        if (marker == AgentSignalObservation::refused) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::sleep_observation,
                "Sleep marker became unsafe; application was not claimed.");
            return;
        }
        const auto applied = observe_agent_sleep_received(
                operation_->request.attachment, target.key,
                target.sleep_baseline)
            || marker == AgentSignalObservation::absent;
        if (applied) {
            finish_target(target, AgentLifecycleOutcomeKind::applied,
                AgentLifecyclePhase::sleep_observation,
                "Sleep request applied.");
        } else if (now() >= target.deadline) {
            finish_target(target, AgentLifecycleOutcomeKind::timed_out,
                AgentLifecyclePhase::sleep_observation,
                "Sleep requested; application was not observed before timeout.");
        }
    }

    void poll_suspend(Target &target, bool temporary) {
        const auto observed = processes(target);
        const auto stopped = observed.available && observed.pids.empty()
            && !heartbeat_is_alive(target);
        if (stopped) {
            if (!temporary) {
                finish_target(target, AgentLifecycleOutcomeKind::applied,
                    AgentLifecyclePhase::suspend_observation,
                    "Suspend applied; Agent process and heartbeat stopped.");
            } else if (target.clear_completed) {
                finish_target(target, AgentLifecycleOutcomeKind::applied,
                    AgentLifecyclePhase::temporary_suspend,
                    "Clear completed and the temporary Agent suspended.");
            } else {
                finish_target(target, AgentLifecycleOutcomeKind::timed_out,
                    AgentLifecyclePhase::clear_observation,
                    target.clear_timed_out
                        ? "Clear completion timed out; temporary Agent suspended."
                        : "Clear failed; temporary Agent suspended.");
            }
            return;
        }
        if (now() < target.deadline) return;
        if (!temporary) {
            finish_target(target, AgentLifecycleOutcomeKind::timed_out,
                AgentLifecyclePhase::suspend_observation,
                observed.available
                    ? "Suspend requested but process/heartbeat did not stop."
                    : "Suspend requested but process death could not be verified.");
        } else if (target.clear_completed) {
            finish_target(target, AgentLifecycleOutcomeKind::partial,
                AgentLifecyclePhase::temporary_suspend,
                "Clear completed, but the temporary Agent did not suspend.");
        } else {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::temporary_suspend,
                "Clear did not complete and the temporary Agent did not suspend.");
        }
    }

    void poll_lease(Target &target) {
        const auto lease = observe_lease(
            operation_->request.attachment, target.key);
        if (lease == LeaseObservation::held) {
            if (now() >= target.deadline) {
                finish_target(target, AgentLifecycleOutcomeKind::timed_out,
                    AgentLifecyclePhase::lease_wait,
                    "Agent lease remained held before launch timeout.");
            }
            return;
        }
        if (lease == LeaseObservation::unavailable) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::lease_wait,
                "Agent lease could not be inspected safely.");
            return;
        }
        const auto observed = processes(target);
        if (!observed.available) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::process_scan,
                "Process table unavailable; duplicate-safe launch refused.");
            return;
        }
        if (!observed.pids.empty()) {
            if (target.lease_purpose == LeasePurpose::clear) {
                if (write_signal(target, AgentSignalKind::clear,
                        "Clear request was not written.")) {
                    finish_target(target, AgentLifecycleOutcomeKind::requested,
                        AgentLifecyclePhase::signal_write,
                        "Clear requested from the live Agent.");
                }
            } else {
                finish_target(target, AgentLifecycleOutcomeKind::already_online,
                    AgentLifecyclePhase::complete,
                    "Agent is already online.");
            }
            return;
        }
        if (!clean_stale_signals(operation_->request.attachment, target.key)) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::stale_cleanup,
                "Unsafe stale signal leaf refused before launch.");
            return;
        }
        launch(target);
    }

    void poll_refresh_lease(Target &target, bool final_wait) {
        const auto lease = observe_lease(
            operation_->request.attachment, target.key);
        if (lease == LeaseObservation::unavailable) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::lease_wait,
                "Refresh could not inspect the Agent lease safely.");
            return;
        }
        const auto observed = processes(target);
        if (!observed.available) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::process_scan,
                "Refresh process table scan was unavailable.");
            return;
        }
        if (lease == LeaseObservation::free && observed.pids.empty()) {
            refresh_cleanup_and_launch(target);
            return;
        }
        if (final_wait) {
            if (now() >= target.deadline) {
                finish_target(target, AgentLifecycleOutcomeKind::timed_out,
                    AgentLifecyclePhase::lease_wait,
                    "Refresh process stopped but the Agent lease stayed held.");
            }
            return;
        }
        if (lease == LeaseObservation::free && !observed.pids.empty()) {
            send_termination(target, observed, AgentTerminationSignal::terminate);
            target.deadline = now() + kTerminateWait;
            target.stage = Stage::wait_term;
            return;
        }
        if (now() < target.deadline) return;
        if (!observed.pids.empty()) {
            send_termination(target, observed, AgentTerminationSignal::terminate);
            target.deadline = now() + kTerminateWait;
            target.stage = Stage::wait_term;
            return;
        }
        finish_target(target, AgentLifecycleOutcomeKind::timed_out,
            AgentLifecyclePhase::lease_wait,
            "Refresh timed out waiting for the Agent lease to release.");
    }

    void send_termination(Target &target,
            const AgentProcessObservation &observed,
            AgentTerminationSignal signal) {
        for (const auto pid : observed.pids) {
            static_cast<void>(dependencies_.processes.signal(
                agent_dir(target), pid, signal));
        }
    }

    void poll_termination(Target &target, bool kill_phase) {
        const auto observed = processes(target);
        if (!observed.available) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::process_scan,
                "Refresh could not verify process termination.");
            return;
        }
        if (observed.pids.empty()) {
            target.deadline = now() + kPostKillLeaseWait;
            target.stage = Stage::wait_refresh_final_lease;
            return;
        }
        if (now() < target.deadline) return;
        if (!kill_phase) {
            send_termination(target, observed, AgentTerminationSignal::kill);
            target.deadline = now() + kKillWait;
            target.stage = Stage::wait_kill;
            return;
        }
        finish_target(target, AgentLifecycleOutcomeKind::failed,
            AgentLifecyclePhase::kill_wait,
            "Exact Agent process remained after SIGKILL.");
    }

    void refresh_cleanup_and_launch(Target &target) {
        if (!clean_stale_signals(operation_->request.attachment, target.key)
            || !remove_stale_lock_if_free(
                operation_->request.attachment, target.key)) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::stale_cleanup,
                "Refresh refused unsafe or held stale lifecycle leaves.");
            return;
        }
        if (!target.preset || !apply_preset(
                operation_->request.attachment, target.key, *target.preset)) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::preset_update,
                "Refresh stopped the Agent but could not atomically apply the authorized preset.");
            return;
        }
        launch(target);
        if (target.stage != Stage::wait_heartbeat) return;
        target.post_launch_cleanup_failed = remove_agent_signal(
            operation_->request.attachment, target.key,
            AgentSignalKind::suspend) == AgentSignalRemoveResult::refused;
    }

    void launch(Target &target) {
        target.heartbeat_before = heartbeat_value(
            operation_->request.attachment, target.key);
        target.launch = dependencies_.launcher.launch(
            operation_->request.attachment, target.key,
            operation_->request.fallback_python);
        if (target.launch.result != AgentLaunchResult::started) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::launch,
                "Agent launch was refused; see logs/agent.log.");
            return;
        }
        target.launch_started = now();
        target.deadline = target.launch_started + kLaunchInitialWait;
        target.launch_cap = target.launch_started + kLaunchCap;
        target.stage = Stage::wait_heartbeat;
    }

    bool fresh_heartbeat(const Target &target) const {
        const auto current = heartbeat_value(
            operation_->request.attachment, target.key);
        if (!current || !heartbeat_is_alive(target)) return false;
        return !target.heartbeat_before || *current > *target.heartbeat_before;
    }

    void poll_heartbeat(Target &target) {
        if (fresh_heartbeat(target)) {
            if (operation_->request.command == AgentLifecycleCommand::clear) {
                target.molt_before = read_molt_count(
                    operation_->request.attachment, target.key);
                target.event_baseline = capture_event_baseline(
                    operation_->request.attachment, target.key);
                if (write_agent_signal(operation_->request.attachment,
                        target.key, AgentSignalKind::clear)
                        != AgentSignalWriteResult::written) {
                    start_temporary_suspend(target, false, false);
                    return;
                }
                target.deadline = now() + kClearObservation;
                target.stage = Stage::wait_clear;
                return;
            }
            const auto refresh = operation_->request.command
                == AgentLifecycleCommand::refresh;
            finish_target(target,
                target.post_launch_cleanup_failed
                    ? AgentLifecycleOutcomeKind::partial
                    : AgentLifecycleOutcomeKind::applied,
                target.post_launch_cleanup_failed
                    ? AgentLifecyclePhase::stale_cleanup
                    : AgentLifecyclePhase::heartbeat_wait,
                refresh
                    ? (target.post_launch_cleanup_failed
                        ? "Refresh launched with a fresh heartbeat, but raced suspend cleanup failed."
                        : "Refresh applied; fresh heartbeat observed.")
                    : "Agent is online with a fresh heartbeat.");
            return;
        }
        const auto observed = processes(target);
        if (!observed.available) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::process_scan,
                "Launch process could not be observed safely.");
            return;
        }
        if (observed.pids.empty()
            && now() >= target.launch_started + kLaunchVisibilityGrace) {
            finish_target(target, AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::heartbeat_wait,
                "Agent process exited before a fresh heartbeat; see logs/agent.log.");
            return;
        }
        if (now() < target.deadline) return;
        if (!observed.pids.empty() && now() < target.launch_cap) {
            // Slow but still exact process: keep waiting to the documented cap.
            target.deadline = target.launch_cap;
            return;
        }
        if (operation_->request.command == AgentLifecycleCommand::clear) {
            start_temporary_suspend(target, false, false);
            return;
        }
        finish_target(target, AgentLifecycleOutcomeKind::timed_out,
            AgentLifecyclePhase::heartbeat_wait,
            "Agent process stayed alive but wrote no fresh heartbeat before the startup cap; see logs/agent.log.");
    }

    void poll_clear(Target &target) {
        const auto molt = read_molt_count(
            operation_->request.attachment, target.key);
        const auto completed = (target.molt_before && molt
                && *molt > *target.molt_before)
            || observe_clear_event(operation_->request.attachment,
                target.key, target.event_baseline);
        if (completed) {
            start_temporary_suspend(target, true, false);
        } else if (now() >= target.deadline) {
            start_temporary_suspend(target, false, true);
        }
    }

    void start_temporary_suspend(
            Target &target, bool clear_completed, bool clear_timed_out) {
        target.clear_completed = clear_completed;
        target.clear_timed_out = clear_timed_out;
        if (write_agent_signal(operation_->request.attachment,
                target.key, AgentSignalKind::suspend)
            != AgentSignalWriteResult::written) {
            finish_target(target,
                clear_completed ? AgentLifecycleOutcomeKind::partial
                                : AgentLifecycleOutcomeKind::failed,
                AgentLifecyclePhase::temporary_suspend,
                clear_completed
                    ? "Clear completed, but temporary suspend was not written."
                    : "Clear did not complete and temporary suspend was not written.");
            return;
        }
        target.deadline = now() + kTemporarySuspend;
        target.stage = Stage::wait_temporary_suspend;
    }

    void drive() {
        while (operation_ && operation_->index < operation_->targets.size()) {
            auto &target = operation_->targets[operation_->index];
            const auto before = operation_->index;
            switch (target.stage) {
            case Stage::begin: begin(target); break;
            case Stage::wait_sleep: poll_sleep(target); break;
            case Stage::wait_suspend: poll_suspend(target, false); break;
            case Stage::wait_lease: poll_lease(target); break;
            case Stage::wait_refresh_lease:
                poll_refresh_lease(target, false);
                break;
            case Stage::wait_refresh_final_lease:
                poll_refresh_lease(target, true);
                break;
            case Stage::wait_term: poll_termination(target, false); break;
            case Stage::wait_kill: poll_termination(target, true); break;
            case Stage::wait_heartbeat: poll_heartbeat(target); break;
            case Stage::wait_clear: poll_clear(target); break;
            case Stage::wait_temporary_suspend:
                poll_suspend(target, true);
                break;
            }
            if (!operation_) return;
            if (operation_->index == before) return; // waiting phase
        }
        if (operation_ && operation_->index == operation_->targets.size()) {
            finish_operation();
        }
    }

    AgentLifecycleDependencies dependencies_;
    QTimer timer_;
    std::optional<Operation> operation_;
};

AgentLifecycleController::AgentLifecycleController(
    AgentLifecycleDependencies dependencies)
: impl_(std::make_unique<Impl>(std::move(dependencies))) {}

AgentLifecycleController::~AgentLifecycleController() = default;

bool AgentLifecycleController::is_pending() const noexcept {
    return impl_->pending();
}

AgentLifecycleStartResult AgentLifecycleController::run(
        AgentLifecycleRequest request, Done done) {
    return impl_->run(std::move(request), std::move(done));
}

void AgentLifecycleController::tick() { impl_->tick(); }

void AgentLifecycleController::cancel() noexcept { impl_->cancel(); }

} // namespace lingtai::desktop
