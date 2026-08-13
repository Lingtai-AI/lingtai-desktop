#include "agent_projection.h"
#include "posix_descriptor_primitives.h"
#include "project_attachment.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <locale>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {
namespace fs = std::filesystem;
namespace posix = posix_internal;

// Discovery owns a 1 MiB source-content limit for each immediate manifest and
// status file. The bounded read, not advisory metadata, authoritatively
// enforces it. Heartbeats are one owned wall-clock number, not an arbitrary
// metadata file.
constexpr std::size_t kMaximumAgentManifestBytes = 1024U * 1024U;
constexpr std::size_t kMaximumAgentStatusBytes = 1024U * 1024U;
constexpr std::size_t kMaximumAgentHeartbeatBytes = 128U;
constexpr double kHeartbeatStaleAfterSeconds = 5.0;

enum class LeafReadOutcome { absent, unsafe_symlink, unreadable, read };
struct LeafRead { LeafReadOutcome outcome; std::string bytes; };

// Reads one immediate, safe-leaf file relative to an already open directory
// descriptor: no-follow at the single leaf, bounded to `limit` on the actual
// read. An absent leaf and an unreadable/oversize/non-regular leaf are kept
// distinct because "no manifest at all" and "a manifest that failed to read"
// are different, real outcomes; nothing beyond that distinction is retained.
[[nodiscard]] LeafRead read_leaf(int parent_fd, const char *name,
        std::size_t limit) {
    struct stat leaf {};
    if (::fstatat(parent_fd, name, &leaf, AT_SYMLINK_NOFOLLOW) != 0) {
        return {LeafReadOutcome::absent, {}};
    }
    if (S_ISLNK(leaf.st_mode)) return {LeafReadOutcome::unsafe_symlink, {}};
    if (!S_ISREG(leaf.st_mode)) return {LeafReadOutcome::unreadable, {}};
    const auto file = posix::open_regular_file_component(parent_fd, name);
    if (file.get() < 0) return {LeafReadOutcome::unreadable, {}};
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || opened.st_size < 0
        || static_cast<std::uintmax_t>(opened.st_size) > limit) {
        return {LeafReadOutcome::unreadable, {}};
    }
    std::string bytes(static_cast<std::size_t>(opened.st_size), '\0');
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::read(file.get(), bytes.data() + total,
            bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return {LeafReadOutcome::unreadable, {}};
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    return {LeafReadOutcome::read, std::move(bytes)};
}

[[nodiscard]] std::optional<std::string> nonempty_string(
        const QJsonObject &object, const char *key) {
    const auto value = object.value(key);
    if (!value.isString()) return std::nullopt;
    auto text = value.toString().toStdString();
    return text.empty() ? std::nullopt
                        : std::optional<std::string>(std::move(text));
}

[[nodiscard]] std::optional<std::int64_t> exact_integer(
        const QJsonValue &value) {
    if (!value.isDouble()) return std::nullopt;
    const auto number = value.toDouble();
    constexpr auto kMaximumExactInteger = 9007199254740991.0;
    if (!std::isfinite(number) || std::trunc(number) != number
        || number < -kMaximumExactInteger || number > kMaximumExactInteger) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
}

[[nodiscard]] std::optional<double> finite_number(const QJsonValue &value) {
    if (!value.isDouble()) return std::nullopt;
    const auto number = value.toDouble();
    return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}

[[nodiscard]] AgentCapabilityFacts parse_capabilities(
        const QJsonObject &object) {
    AgentCapabilityFacts result;
    constexpr std::array intrinsic{"system", "soul", "email", "psyche"};
    for (const auto *name : intrinsic) result.display_names.emplace_back(name);
    const auto value = object.value("capabilities");
    if (!value.isArray()) return result;
    for (const auto &entry : value.toArray()) {
        std::optional<std::string> name;
        if (entry.isString()) {
            auto text = entry.toString().toStdString();
            if (!text.empty()) name = std::move(text);
        } else if (entry.isArray()) {
            const auto tuple = entry.toArray();
            if (!tuple.empty() && tuple.at(0).isString()) {
                auto text = tuple.at(0).toString().toStdString();
                if (!text.empty()) name = std::move(text);
            }
        }
        if (!name) continue;
        if (std::ranges::find(result.display_names, *name)
                == result.display_names.end()) {
            result.display_names.push_back(*name);
        }
    }
    return result;
}

[[nodiscard]] AgentIdentityFacts parse_identity(const QJsonObject &object) {
    AgentIdentityFacts result;
    result.agent_id = nonempty_string(object, "agent_id");
    result.true_name = nonempty_string(object, "agent_name");
    result.nickname = nonempty_string(object, "nickname");
    result.address = nonempty_string(object, "address");
    result.state = nonempty_string(object, "state");
    if (const auto llm = object.value("llm"); llm.isObject()) {
        const auto values = llm.toObject();
        result.llm.provider = nonempty_string(values, "provider");
        result.llm.model = nonempty_string(values, "model");
        result.llm.base_url = nonempty_string(values, "base_url");
        result.llm.api_compat = nonempty_string(values, "api_compat");
        result.llm.context_limit = exact_integer(values.value("context_limit"));
    }
    result.capabilities = parse_capabilities(object);
    return result;
}

struct ParsedManifest {
    AgentManifestDiagnosticKind diagnostic =
        AgentManifestDiagnosticKind::invalid_json;
    AgentRole role = AgentRole::unknown;
    std::optional<AgentIdentityFacts> identity;
};

[[nodiscard]] ParsedManifest parse_manifest(const std::string &bytes) {
    QJsonParseError error;
    const auto value = QJsonValue::fromJson(
        QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
        &error);
    if (error.error != QJsonParseError::NoError) {
        return {AgentManifestDiagnosticKind::invalid_json,
            AgentRole::unknown, std::nullopt};
    }
    if (!value.isObject()) {
        return {AgentManifestDiagnosticKind::not_object,
            AgentRole::unknown, std::nullopt};
    }
    const auto object = value.toObject();
    auto role = AgentRole::agent;
    constexpr auto admin_key = "admin";
    if (!object.contains(admin_key) || object.value(admin_key).isNull()) {
        role = AgentRole::human;
    } else if (const auto admin = object.value(admin_key); admin.isObject()) {
        for (const auto &permission : admin.toObject()) {
            if (permission.isBool() && permission.toBool()) {
                role = AgentRole::main;
                break;
            }
        }
    }
    return {AgentManifestDiagnosticKind::none, role, parse_identity(object)};
}

struct ParsedStatus { bool valid = false; AgentRuntimeFacts facts; };

[[nodiscard]] ParsedStatus parse_status(const std::string &bytes) {
    QJsonParseError error;
    const auto value = QJsonValue::fromJson(
        QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
        &error);
    if (error.error != QJsonParseError::NoError || !value.isObject()) {
        return {};
    }
    const auto object = value.toObject();
    AgentRuntimeFacts facts;
    if (const auto runtime = object.value("runtime"); runtime.isObject()) {
        const auto fields = runtime.toObject();
        facts.state = nonempty_string(fields, "state");
        if (const auto running = fields.value("running"); running.isBool()) {
            facts.running = running.toBool();
        }
        if (const auto pid = exact_integer(fields.value("pid"));
                pid && *pid >= 0) {
            facts.pid = *pid;
        }
        facts.state_changed_at = finite_number(fields.value("state_changed_at"));
        facts.last_progress_at = finite_number(fields.value("last_progress_at"));
        if (const auto seconds = finite_number(fields.value("no_progress_seconds"));
                seconds && *seconds >= 0.0) {
            facts.no_progress_seconds = *seconds;
        }
    }
    if (const auto active_turn = object.value("active_turn");
            active_turn.isObject()) {
        const auto fields = active_turn.toObject();
        AgentActiveTurnFacts active;
        active.kind = nonempty_string(fields, "kind");
        active.id = nonempty_string(fields, "id");
        active.started_at = finite_number(fields.value("started_at"));
        if (const auto seconds = finite_number(fields.value("elapsed_seconds"));
                seconds && *seconds >= 0.0) {
            active.elapsed_seconds = *seconds;
        }
        facts.active_turn = std::move(active);
    }
    if (const auto tokens = object.value("tokens"); tokens.isObject()) {
        if (const auto context = tokens.toObject().value("context");
                context.isObject()) {
            const auto fields = context.toObject();
            if (const auto window = exact_integer(fields.value("window_size"));
                    window && *window > 0) {
                AgentContextFacts projected;
                projected.window_size = *window;
                projected.system_tokens = exact_integer(fields.value("system_tokens"));
                projected.tools_tokens = exact_integer(fields.value("tools_tokens"));
                projected.history_tokens = exact_integer(fields.value("history_tokens"));
                projected.total_tokens = exact_integer(fields.value("total_tokens"));
                projected.usage_percent = finite_number(fields.value("usage_pct"));
                projected.fixed_tokens = exact_integer(fields.value("fixed_tokens"));
                projected.growing_tokens = exact_integer(fields.value("growing_tokens"));
                facts.context = std::move(projected);
            }
        }
    }
    return {true, std::move(facts)};
}

[[nodiscard]] bool ascii_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] std::string_view trim_ascii(const std::string &bytes) {
    auto first = std::size_t{0};
    while (first < bytes.size() && ascii_whitespace(bytes[first])) ++first;
    auto last = bytes.size();
    while (last > first && ascii_whitespace(bytes[last - 1U])) --last;
    return std::string_view(bytes).substr(first, last - first);
}

enum class DecimalParseState { valid, invalid, nonfinite };
struct ParsedDecimal { DecimalParseState state; double value = 0.0; };

[[nodiscard]] bool ascii_digit(char value) {
    return value >= '0' && value <= '9';
}

// A minimal, locale-independent decimal/exponent parser: exactly the current
// heartbeat number, not a general locale-aware or exotic-literal reader.
[[nodiscard]] ParsedDecimal parse_decimal(std::string_view token) {
    auto lower = std::string(token);
    std::ranges::transform(lower, lower.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
            ? static_cast<char>(value + ('a' - 'A')) : value;
    });
    if (lower == "nan" || lower == "+nan" || lower == "-nan"
        || lower == "inf" || lower == "+inf" || lower == "-inf"
        || lower == "infinity" || lower == "+infinity"
        || lower == "-infinity") {
        return {DecimalParseState::nonfinite, 0.0};
    }

    auto index = std::size_t{0};
    if (index < token.size() && (token[index] == '+' || token[index] == '-')) {
        ++index;
    }
    auto digits = std::size_t{0};
    while (index < token.size() && ascii_digit(token[index])) ++index, ++digits;
    if (index < token.size() && token[index] == '.') {
        ++index;
        while (index < token.size() && ascii_digit(token[index])) {
            ++index;
            ++digits;
        }
    }
    if (digits == 0) return {DecimalParseState::invalid, 0.0};
    if (index < token.size() && (token[index] == 'e' || token[index] == 'E')) {
        ++index;
        if (index < token.size()
            && (token[index] == '+' || token[index] == '-')) {
            ++index;
        }
        const auto exponent_start = index;
        while (index < token.size() && ascii_digit(token[index])) ++index;
        if (index == exponent_start) {
            return {DecimalParseState::invalid, 0.0};
        }
    }
    if (index != token.size()) return {DecimalParseState::invalid, 0.0};

    auto stream = std::istringstream(std::string(token));
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    auto value = 0.0;
    stream >> value;
    if (stream.fail() || !stream.eof() || !std::isfinite(value)) {
        return {DecimalParseState::invalid, 0.0};
    }
    return {DecimalParseState::valid, value};
}

[[nodiscard]] AgentPresenceKind classify_heartbeat(const LeafRead &read,
        double now, std::optional<double> &age_out) {
    if (read.outcome == LeafReadOutcome::absent) {
        return AgentPresenceKind::missing;
    }
    if (read.outcome != LeafReadOutcome::read) {
        return AgentPresenceKind::unavailable;
    }
    const auto parsed = parse_decimal(trim_ascii(read.bytes));
    if (parsed.state != DecimalParseState::valid || !std::isfinite(now)
        || parsed.value > now) {
        return AgentPresenceKind::invalid;
    }
    const auto age = now - parsed.value;
    if (!std::isfinite(age)) return AgentPresenceKind::invalid;
    age_out = age;
    return age < kHeartbeatStaleAfterSeconds
        ? AgentPresenceKind::alive : AgentPresenceKind::stale;
}

[[nodiscard]] AgentSnapshot scan(
        const ProjectAttachment &attachment, double now) {
    AgentSnapshot snapshot;
    const auto root = posix::open_root_directory(attachment.root());
    if (root.get() < 0) return snapshot;
    const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return snapshot;

    auto names = std::vector<fs::path>();
    const auto duplicate = ::dup(lingtai.get());
    if (duplicate < 0) return snapshot;
    posix::DirectoryStream stream(::fdopendir(duplicate));
    if (!stream.get()) {
        ::close(duplicate);
        return snapshot;
    }
    for (;;) {
        errno = 0;
        const auto entry = ::readdir(stream.get());
        if (!entry) break;
        const auto name = fs::path(entry->d_name);
        if (name != "." && name != "..") names.push_back(name);
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    const auto lingtai_path = attachment.root() / ".lingtai";
    for (const auto &name : names) {
        if (!posix::safe_leaf(name)) continue;
        // One bad sibling -- an unsafe or unreadable candidate directory --
        // is invisible and never hides a later healthy one.
        const auto child = posix::open_directory_component(lingtai.get(), name);
        if (child.get() < 0) continue;
        const auto manifest = read_leaf(
            child.get(), ".agent.json", kMaximumAgentManifestBytes);
        if (manifest.outcome == LeafReadOutcome::absent) continue;

        AgentRow row;
        row.directory_key = name;
        row.directory_path = lingtai_path / name;
        if (manifest.outcome == LeafReadOutcome::unsafe_symlink) {
            row.manifest_kind = AgentManifestKind::unsafe;
            row.manifest_diagnostic = AgentManifestDiagnosticKind::unsafe_symlink;
        } else if (manifest.outcome == LeafReadOutcome::unreadable) {
            row.manifest_kind = AgentManifestKind::malformed;
            row.manifest_diagnostic = AgentManifestDiagnosticKind::unreadable;
        } else {
            auto parsed = parse_manifest(manifest.bytes);
            row.manifest_kind = parsed.diagnostic == AgentManifestDiagnosticKind::none
                ? AgentManifestKind::valid : AgentManifestKind::malformed;
            row.manifest_diagnostic = parsed.diagnostic;
            row.role = parsed.role;
            row.identity = std::move(parsed.identity);
        }

        if (row.role == AgentRole::human || row.role == AgentRole::unknown) {
            row.presence = row.role == AgentRole::human
                ? AgentPresenceKind::alive_human : AgentPresenceKind::unknown;
        } else {
            const auto heartbeat = read_leaf(
                child.get(), ".agent.heartbeat", kMaximumAgentHeartbeatBytes);
            row.presence = classify_heartbeat(
                heartbeat, now, row.heartbeat_age_seconds);
        }

        const auto status = read_leaf(
            child.get(), ".status.json", kMaximumAgentStatusBytes);
        if (status.outcome == LeafReadOutcome::read) {
            if (auto parsed = parse_status(status.bytes); parsed.valid) {
                row.status = std::move(parsed.facts);
            }
        }
        snapshot.items.push_back(std::move(row));
    }
    snapshot.scan = AgentScanState::complete;
    return snapshot;
}

} // namespace

AgentSnapshot project_agents(const ProjectAttachment &attachment) noexcept {
    try {
        const auto now = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return scan(attachment, now);
    } catch (...) {
        // Only row/string allocation can throw here. An interrupted scan
        // shows no partial roster rather than one that looks complete.
        return {};
    }
}

} // namespace lingtai::desktop
