#include "agent_setup_store.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QString>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

constexpr std::size_t kMaximumJsonBytes = 1024U * 1024U;
constexpr std::size_t kMaximumEnvBytes = 1024U * 1024U;
constexpr std::string_view kSoulFlowKey = "LINGTAI_SOUL_FLOW_ENABLED";

enum class LeafKind {
    read,
    absent,
    symlink,
    not_regular,
    oversized,
    unreadable,
};

struct LeafRead {
    LeafKind kind = LeafKind::unreadable;
    std::string bytes;
    mode_t mode = 0;
};

struct OpenParent {
    posix::FileDescriptor directory{-1};
    fs::path leaf;
};

struct PlannedTarget {
    fs::path absolute_path;
    std::string original;
    std::string replacement;
};

struct StagedTarget {
    posix::FileDescriptor directory;
    fs::path leaf;
    std::string temp_leaf;
    std::string backup_leaf;
    bool published = false;
    bool backup_created = false;
};

[[nodiscard]] AgentSetupLoadResult load_failure(
        AgentSetupFailure failure, std::string detail) {
    return {
        .state = std::nullopt,
        .failure = failure,
        .detail = std::move(detail),
    };
}

[[nodiscard]] AgentSetupSaveResult save_failure(
        AgentSetupFailure failure, std::string detail) {
    return {
        .status = AgentSetupSaveStatus::failed,
        .failure = failure,
        .detail = std::move(detail),
    };
}

[[nodiscard]] bool path_has_parent(const fs::path &path) {
    return std::ranges::any_of(path, [](const auto &part) {
        return part == "..";
    });
}

[[nodiscard]] bool contained_relative_path(const fs::path &path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()
            || path.has_root_directory() || path_has_parent(path)) {
        return false;
    }
    for (const auto &part : path) {
        if (part.empty() || part == ".") continue;
        if (!posix::safe_leaf(part)) return false;
    }
    return true;
}

[[nodiscard]] LeafRead read_leaf(
        int parent_fd, const fs::path &leaf, std::size_t limit) {
    if (!posix::safe_leaf(leaf)) return {LeafKind::unreadable, {}, 0};
    struct stat status {};
    if (::fstatat(parent_fd, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT
            ? LeafRead{LeafKind::absent, {}, 0}
            : LeafRead{LeafKind::unreadable, {}, 0};
    }
    if (S_ISLNK(status.st_mode)) return {LeafKind::symlink, {}, 0};
    if (!S_ISREG(status.st_mode)) return {LeafKind::not_regular, {}, 0};
    if (status.st_size < 0
            || static_cast<std::uintmax_t>(status.st_size) > limit) {
        return {LeafKind::oversized, {}, 0};
    }
    auto file = posix::open_regular_file_component(parent_fd, leaf);
    if (file.get() < 0) return {LeafKind::unreadable, {}, 0};
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)) {
        return {LeafKind::unreadable, {}, 0};
    }

    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(opened.st_size));
    char buffer[8192];
    while (true) {
        const auto count = ::read(file.get(), buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) continue;
            return {LeafKind::unreadable, {}, 0};
        }
        if (count == 0) break;
        if (bytes.size() + static_cast<std::size_t>(count) > limit) {
            return {LeafKind::oversized, {}, 0};
        }
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    return {LeafKind::read, std::move(bytes), opened.st_mode};
}

[[nodiscard]] AgentSetupFailure failure_for_leaf(LeafKind kind) {
    switch (kind) {
    case LeafKind::absent: return AgentSetupFailure::missing_required_file;
    case LeafKind::symlink: return AgentSetupFailure::symlink_rejected;
    case LeafKind::not_regular: return AgentSetupFailure::not_regular;
    case LeafKind::oversized: return AgentSetupFailure::oversized;
    case LeafKind::unreadable: return AgentSetupFailure::local_failure;
    case LeafKind::read: return AgentSetupFailure::none;
    }
    return AgentSetupFailure::local_failure;
}

[[nodiscard]] std::optional<QJsonObject> parse_object(
        const std::string &bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

[[nodiscard]] std::optional<std::string> nonempty_string(
        const QJsonObject &object, const char *key) {
    const auto value = object.value(key);
    if (!value.isString()) return std::nullopt;
    auto result = value.toString().toStdString();
    if (result.empty()) return std::nullopt;
    return result;
}

[[nodiscard]] std::int64_t integer_or(
        const QJsonObject &object, const char *key, std::int64_t fallback) {
    const auto value = object.value(key);
    return value.isDouble() ? value.toInteger(fallback) : fallback;
}

[[nodiscard]] std::vector<std::string> string_array(
        const QJsonValue &value) {
    std::vector<std::string> result;
    if (!value.isArray()) return result;
    for (const auto &entry : value.toArray()) {
        if (!entry.isString()) continue;
        auto text = entry.toString().toStdString();
        if (!text.empty()) result.push_back(std::move(text));
    }
    return result;
}

[[nodiscard]] bool is_active_env_line(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    return line.starts_with(kSoulFlowKey)
        && line.size() > kSoulFlowKey.size()
        && line[kSoulFlowKey.size()] == '=';
}

[[nodiscard]] bool env_soul_flow_enabled(const std::string &bytes) {
    std::size_t start = 0;
    while (start <= bytes.size()) {
        const auto end = bytes.find('\n', start);
        const auto stop = end == std::string::npos ? bytes.size() : end;
        auto line = std::string_view(bytes).substr(start, stop - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (is_active_env_line(line)) {
            const auto equals = line.find('=');
            auto value = line.substr(equals + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            std::string normalized(value);
            std::ranges::transform(normalized, normalized.begin(), [](char ch) {
                return ch >= 'A' && ch <= 'Z'
                    ? static_cast<char>(ch - 'A' + 'a') : ch;
            });
            return normalized == "1" || normalized == "true"
                || normalized == "yes" || normalized == "on";
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

[[nodiscard]] std::string merge_soul_flow_env(
        const std::string &bytes, bool enabled) {
    std::string result;
    result.reserve(bytes.size() + kSoulFlowKey.size() + 4U);
    auto found = false;
    auto newline = bytes.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::size_t start = 0;
    while (start < bytes.size()) {
        const auto lf = bytes.find('\n', start);
        const auto stop = lf == std::string::npos ? bytes.size() : lf + 1;
        auto line = std::string_view(bytes).substr(start, stop - start);
        auto content = line;
        if (!content.empty() && content.back() == '\n') content.remove_suffix(1);
        auto ending = std::string_view{};
        if (line.size() != content.size()) ending = "\n";
        if (!content.empty() && content.back() == '\r') {
            content.remove_suffix(1);
            if (!ending.empty()) ending = "\r\n";
        }
        if (is_active_env_line(content)) {
            found = true;
            if (enabled) {
                result.append(kSoulFlowKey);
                result.append("=1");
                result.append(ending);
            }
        } else {
            result.append(line);
        }
        start = stop;
    }
    if (enabled && !found) {
        const auto had_final_newline = bytes.ends_with('\n');
        if (!result.empty() && !result.ends_with('\n')) result.append(newline);
        result.append(kSoulFlowKey);
        result.append("=1");
        if (had_final_newline) result.append(newline);
    }
    return result;
}

[[nodiscard]] bool descriptor_absolute_path(const fs::path &path) {
    if (!path.is_absolute() || path.has_root_name() || !path.has_root_directory()
            || path.filename().empty()) {
        return false;
    }
    for (const auto &part : path.relative_path()) {
        if (part.empty() || part == "." || part == ".."
                || !posix::safe_leaf(part)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<fs::path> configured_env_path(
        const fs::path &project_root, const fs::path &agent_key,
        const std::string &configured) {
    if (configured.empty()) return std::nullopt;
    const auto input = fs::path(configured);
    if (input.is_absolute()) {
        return descriptor_absolute_path(input)
            ? std::optional<fs::path>(input) : std::nullopt;
    }
    if (!contained_relative_path(input)) return std::nullopt;
    const auto candidate = project_root / ".lingtai" / agent_key / input;
    return descriptor_absolute_path(candidate)
        ? std::optional<fs::path>(candidate) : std::nullopt;
}

[[nodiscard]] OpenParent open_absolute_parent(const fs::path &absolute) {
    if (!descriptor_absolute_path(absolute)) return {};
    auto directory = posix::open_root_directory(absolute.root_path());
    if (directory.get() < 0) return {};
    for (const auto &part : absolute.parent_path().relative_path()) {
        auto next = posix::open_directory_component(directory.get(), part);
        if (next.get() < 0) return {};
        directory = std::move(next);
    }
    return {std::move(directory), absolute.filename()};
}

[[nodiscard]] std::string serialize(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Indented).toStdString();
}

[[nodiscard]] bool sentinel_reference(std::string_view reference) {
    if (reference == "keep_current") return true;
    const auto slash = reference.find_last_of("/\\");
    const auto leaf = slash == std::string_view::npos
        ? reference : reference.substr(slash + 1);
    return leaf == "keep_current.json";
}

[[nodiscard]] QJsonArray json_array(const std::vector<std::string> &values) {
    QJsonArray result;
    for (const auto &value : values) {
        result.append(QString::fromStdString(value));
    }
    return result;
}

void patch_admin(QJsonObject &owner, bool karma, bool nirvana) {
    auto admin = owner.value("admin").isObject()
        ? owner.value("admin").toObject() : QJsonObject{};
    admin["karma"] = karma;
    admin["nirvana"] = nirvana;
    owner["admin"] = admin;
}

[[nodiscard]] bool is_orchestrator(const QJsonObject &manifest) {
    const auto admin = manifest.value("admin");
    if (!admin.isObject()) return false;
    const auto flags = admin.toObject();
    return flags.value("karma").toBool(false)
        || flags.value("nirvana").toBool(false);
}

[[nodiscard]] bool valid_draft(const AgentSetupDraft &draft) {
    if (draft.agent_name.empty() || draft.language.empty()
            || draft.context_limit <= 0 || draft.max_rpm < 0
            || draft.max_aed_attempts < 1 || draft.max_aed_attempts > 100
            || (draft.soul_delay
                && (!std::isfinite(*draft.soul_delay) || *draft.soul_delay < 0.0))) {
        return false;
    }
    if (draft.preset.choice == AgentSetupPresetChoice::select_preset) {
        if (draft.preset.reference.empty()
                || sentinel_reference(draft.preset.reference)
                || !draft.preset.manifest.value("llm").isObject()
                || !draft.preset.manifest.value("capabilities").isObject()) {
            return false;
        }
    }
    return std::ranges::none_of(draft.allowed_presets, [](const auto &value) {
        return value.empty() || sentinel_reference(value);
    });
}

[[nodiscard]] std::string transaction_leaf(const char *suffix) {
    static std::atomic<unsigned long long> next{0};
    return ".lingtai-setup-" + std::to_string(::getpid()) + "-"
        + std::to_string(next.fetch_add(1)) + suffix;
}

[[nodiscard]] bool write_all(int fd, const std::string &bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(
            fd, bytes.data() + written, bytes.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

void cleanup_staged(std::vector<StagedTarget> &targets) {
    for (auto &target : targets) {
        if (!target.temp_leaf.empty()) {
            ::unlinkat(target.directory.get(), target.temp_leaf.c_str(), 0);
        }
        if (target.backup_created && !target.backup_leaf.empty()) {
            ::unlinkat(target.directory.get(), target.backup_leaf.c_str(), 0);
        }
    }
}

[[nodiscard]] bool rollback(std::vector<StagedTarget> &targets) {
    auto ok = true;
    for (auto i = targets.rbegin(); i != targets.rend(); ++i) {
        if (i->published) {
            if (::renameat(i->directory.get(), i->backup_leaf.c_str(),
                    i->directory.get(), i->leaf.c_str()) != 0) {
                ok = false;
            } else {
                i->backup_created = false;
            }
        } else if (i->backup_created) {
            if (::unlinkat(i->directory.get(), i->backup_leaf.c_str(), 0) != 0) {
                ok = false;
            } else {
                i->backup_created = false;
            }
        }
        if (!i->temp_leaf.empty()) {
            ::unlinkat(i->directory.get(), i->temp_leaf.c_str(), 0);
        }
    }
    return ok;
}

} // namespace

AgentSetupPresetPolicy reconcile_agent_setup_presets(
        const AgentSetupPresetPolicy &current,
        const AgentSetupPresetSelection &selection,
        const std::vector<std::string> &requested_allowed) {
    AgentSetupPresetPolicy result;
    result.default_ref = selection.choice == AgentSetupPresetChoice::select_preset
        ? selection.reference
        : (!current.default_ref.empty() ? current.default_ref : current.active);

    const auto &seed = requested_allowed.empty()
        ? current.allowed : requested_allowed;
    for (const auto &reference : seed) {
        if (reference.empty()
                || std::ranges::find(result.allowed, reference)
                    != result.allowed.end()) {
            continue;
        }
        result.allowed.push_back(reference);
    }
    if (!result.default_ref.empty()
            && std::ranges::find(result.allowed, result.default_ref)
                == result.allowed.end()) {
        result.allowed.push_back(result.default_ref);
    }
    if (requested_allowed.empty() && !current.active.empty()
            && std::ranges::find(result.allowed, current.active)
                == result.allowed.end()) {
        result.allowed.push_back(current.active);
    }
    result.active = !current.active.empty()
            && std::ranges::find(result.allowed, current.active)
                != result.allowed.end()
        ? current.active : result.default_ref;
    return result;
}

AgentSetupStore::AgentSetupStore(const ProjectAttachment &attachment)
: project_root_(attachment.root()) {
}

AgentSetupLoadResult AgentSetupStore::load(
        const fs::path &agent_key) const noexcept {
    try {
        if (!posix::safe_leaf(agent_key)) {
            return load_failure(
                AgentSetupFailure::unsafe_agent_key, "unsafe Agent directory key");
        }
        auto root = posix::open_root_directory(project_root_);
        if (root.get() < 0) {
            return load_failure(
                AgentSetupFailure::project_unavailable, "project root unavailable");
        }
        auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
        if (lingtai.get() < 0) {
            return load_failure(
                AgentSetupFailure::project_unavailable, ".lingtai unavailable");
        }
        auto agent = posix::open_directory_component(lingtai.get(), agent_key);
        if (agent.get() < 0) {
            return load_failure(
                AgentSetupFailure::unsafe_path, "selected Agent directory unavailable");
        }

        const auto init_read = read_leaf(agent.get(), "init.json", kMaximumJsonBytes);
        if (init_read.kind != LeafKind::read) {
            return load_failure(
                failure_for_leaf(init_read.kind), "cannot safely read init.json");
        }
        const auto agent_read = read_leaf(agent.get(), ".agent.json", kMaximumJsonBytes);
        if (agent_read.kind != LeafKind::read) {
            return load_failure(failure_for_leaf(agent_read.kind),
                "cannot safely read .agent.json");
        }
        const auto init = parse_object(init_read.bytes);
        const auto identity = parse_object(agent_read.bytes);
        if (!init || !identity) {
            return load_failure(
                AgentSetupFailure::malformed_json, "configuration JSON is malformed");
        }
        if (!nonempty_string(*identity, "agent_id")) {
            return load_failure(
                AgentSetupFailure::missing_identity, ".agent.json has no agent_id");
        }
        if (!init->value("manifest").isObject()) {
            return load_failure(
                AgentSetupFailure::invalid_shape, "init.json manifest is not an object");
        }
        const auto manifest = init->value("manifest").toObject();
        if (!manifest.value("preset").isObject()) {
            return load_failure(
                AgentSetupFailure::invalid_shape, "manifest.preset is not an object");
        }
        const auto preset = manifest.value("preset").toObject();
        AgentSetupPresetPolicy current{
            .active = nonempty_string(preset, "active").value_or(""),
            .default_ref = nonempty_string(preset, "default").value_or(""),
            .allowed = string_array(preset.value("allowed")),
        };
        if (current.default_ref.empty()) current.default_ref = current.active;
        if (current.active.empty()) current.active = current.default_ref;
        if (current.default_ref.empty()) {
            return load_failure(
                AgentSetupFailure::invalid_shape, "preset policy has no real default");
        }
        if (current.allowed.empty()) current.allowed.push_back(current.default_ref);
        if (std::ranges::find(current.allowed, current.default_ref)
                == current.allowed.end()) {
            current.allowed.push_back(current.default_ref);
        }
        if (std::ranges::find(current.allowed, current.active)
                == current.allowed.end()) {
            current.allowed.push_back(current.active);
        }

        AgentSetupState state;
        state.agent_key = agent_key;
        state.init_document = *init;
        state.agent_document = *identity;
        state.init_bytes = init_read.bytes;
        state.agent_bytes = agent_read.bytes;
        state.draft.preset.choice = AgentSetupPresetChoice::keep_current;
        state.draft.allowed_presets = current.allowed;
        state.draft.agent_name = nonempty_string(manifest, "agent_name")
            .value_or(nonempty_string(*identity, "agent_name").value_or(""));
        state.draft.language = nonempty_string(manifest, "language").value_or("en");
        state.draft.context_limit = integer_or(manifest, "context_limit", 500000);
        state.draft.max_rpm = integer_or(manifest, "max_rpm", 60);
        state.draft.max_aed_attempts = integer_or(manifest, "max_aed_attempts", 5);
        if (const auto soul = manifest.value("soul"); soul.isObject()) {
            const auto delay = soul.toObject().value("delay");
            if (delay.isDouble() && std::isfinite(delay.toDouble())) {
                state.draft.soul_delay = delay.toDouble();
            }
        }
        const auto admin = manifest.value("admin").toObject();
        state.draft.karma = admin.value("karma").toBool(false);
        state.draft.nirvana = admin.value("nirvana").toBool(false);
        state.draft.covenant_file = nonempty_string(*init, "covenant_file")
            .value_or("");
        state.draft.comment_file = nonempty_string(*init, "comment_file").value_or("");

        if (const auto configured = nonempty_string(*init, "env_file")) {
            const auto env_path = configured_env_path(
                project_root_, agent_key, *configured);
            if (!env_path) {
                return load_failure(
                    AgentSetupFailure::unsafe_path, "env_file path is unsafe");
            }
            auto parent = open_absolute_parent(*env_path);
            if (parent.directory.get() < 0) {
                return load_failure(
                    AgentSetupFailure::unsafe_path, "env_file parent is unsafe");
            }
            const auto env = read_leaf(
                parent.directory.get(), parent.leaf, kMaximumEnvBytes);
            if (env.kind != LeafKind::read) {
                return load_failure(
                    failure_for_leaf(env.kind), "cannot safely read env_file");
            }
            state.env_path = *env_path;
            state.env_bytes = env.bytes;
            state.draft.soul_flow_enabled = env_soul_flow_enabled(env.bytes);
        }

        const auto duplicate = ::dup(lingtai.get());
        if (duplicate < 0) {
            return load_failure(
                AgentSetupFailure::local_failure, "cannot scan peer Agents");
        }
        posix::DirectoryStream entries(::fdopendir(duplicate));
        if (!entries.get()) {
            ::close(duplicate);
            return load_failure(
                AgentSetupFailure::local_failure, "cannot scan peer Agents");
        }
        std::vector<std::string> names;
        errno = 0;
        while (const auto *entry = ::readdir(entries.get())) {
            const auto name = std::string(entry->d_name);
            if (name == "." || name == "..") continue;
            names.push_back(name);
        }
        if (errno != 0) {
            return load_failure(
                AgentSetupFailure::local_failure, "cannot enumerate peer Agents");
        }
        std::ranges::sort(names);
        for (const auto &name : names) {
            const auto key = fs::path(name);
            if (key == agent_key || name == "human") continue;
            struct stat status {};
            if (::fstatat(lingtai.get(), name.c_str(), &status,
                    AT_SYMLINK_NOFOLLOW) != 0) {
                return load_failure(
                    AgentSetupFailure::local_failure, "cannot inspect peer entry");
            }
            if (S_ISLNK(status.st_mode)) {
                return load_failure(
                    AgentSetupFailure::symlink_rejected, "peer directory symlink rejected");
            }
            if (!S_ISDIR(status.st_mode)) continue;
            auto peer = posix::open_directory_component(lingtai.get(), key);
            if (peer.get() < 0) {
                return load_failure(
                    AgentSetupFailure::unsafe_path, "peer directory is unsafe");
            }
            const auto peer_init = read_leaf(
                peer.get(), "init.json", kMaximumJsonBytes);
            if (peer_init.kind == LeafKind::absent) continue;
            if (peer_init.kind != LeafKind::read) {
                return load_failure(failure_for_leaf(peer_init.kind),
                    "cannot safely read peer init.json");
            }
            const auto peer_document = parse_object(peer_init.bytes);
            if (!peer_document) {
                return load_failure(
                    AgentSetupFailure::malformed_json, "peer init.json is malformed");
            }
            const auto peer_manifest = peer_document->value("manifest");
            if (!peer_manifest.isObject()) continue;
            state.peers.push_back({
                .directory_key = key,
                .init_document = *peer_document,
                .init_bytes = peer_init.bytes,
            });
        }
        return {
            .state = std::move(state),
            .failure = AgentSetupFailure::none,
            .detail = {},
        };
    } catch (...) {
        return load_failure(
            AgentSetupFailure::local_failure, "unexpected local setup load failure");
    }
}

AgentSetupSaveResult AgentSetupStore::save(
        const AgentSetupState &state,
        const AgentSetupDraft &draft,
        AgentSetupFailurePoint failure_point) const noexcept {
    std::vector<StagedTarget> staged;
    try {
        if (!posix::safe_leaf(state.agent_key) || !valid_draft(draft)) {
            return save_failure(
                AgentSetupFailure::invalid_draft, "invalid setup draft");
        }
        const auto existing_manifest = state.init_document.value("manifest").toObject();
        const auto existing_preset = existing_manifest.value("preset").toObject();
        const AgentSetupPresetPolicy current{
            .active = nonempty_string(existing_preset, "active").value_or(""),
            .default_ref = nonempty_string(existing_preset, "default")
                .value_or(nonempty_string(existing_preset, "active").value_or("")),
            .allowed = string_array(existing_preset.value("allowed")),
        };
        const auto policy = reconcile_agent_setup_presets(
            current, draft.preset, draft.allowed_presets);
        if (policy.default_ref.empty() || policy.active.empty()
                || policy.allowed.empty()
                || sentinel_reference(policy.default_ref)
                || sentinel_reference(policy.active)) {
            return save_failure(
                AgentSetupFailure::invalid_draft, "invalid reconciled preset policy");
        }

        auto init = state.init_document;
        auto manifest = existing_manifest;
        manifest["agent_name"] = QString::fromStdString(draft.agent_name);
        manifest["language"] = QString::fromStdString(draft.language);
        manifest["context_limit"] = draft.context_limit;
        manifest["max_rpm"] = draft.max_rpm;
        manifest["max_aed_attempts"] = draft.max_aed_attempts;
        patch_admin(manifest, draft.karma, draft.nirvana);
        if (draft.preset.choice == AgentSetupPresetChoice::select_preset) {
            manifest["llm"] = draft.preset.manifest.value("llm");
            manifest["capabilities"] = draft.preset.manifest.value("capabilities");
        }
        auto soul = manifest.value("soul").isObject()
            ? manifest.value("soul").toObject() : QJsonObject{};
        if (draft.soul_delay) {
            soul["delay"] = *draft.soul_delay;
        } else {
            soul.remove("delay");
        }
        if (soul.isEmpty()) manifest.remove("soul");
        else manifest["soul"] = soul;

        auto preset = existing_preset;
        preset["active"] = QString::fromStdString(policy.active);
        preset["default"] = QString::fromStdString(policy.default_ref);
        preset["allowed"] = json_array(policy.allowed);
        manifest["preset"] = preset;
        init["manifest"] = manifest;
        if (!draft.covenant_file.empty()) {
            init["covenant_file"] = QString::fromStdString(draft.covenant_file);
        }
        if (!draft.comment_file.empty()) {
            init["comment_file"] = QString::fromStdString(draft.comment_file);
        }

        auto identity = state.agent_document;
        identity["agent_name"] = QString::fromStdString(draft.agent_name);
        patch_admin(identity, draft.karma, draft.nirvana);

        std::vector<PlannedTarget> plan;
        const auto configured = nonempty_string(state.init_document, "env_file");
        const auto authorized_env = configured
            ? configured_env_path(project_root_, state.agent_key, *configured)
            : std::nullopt;
        if (authorized_env != state.env_path) {
            return save_failure(AgentSetupFailure::invalid_draft,
                "env target does not match the loaded init.json");
        }
        if (state.env_path
                && draft.soul_flow_enabled != state.draft.soul_flow_enabled) {
            const auto replacement = merge_soul_flow_env(
                state.env_bytes, draft.soul_flow_enabled);
            if (replacement != state.env_bytes) {
                plan.push_back({
                    .absolute_path = *state.env_path,
                    .original = state.env_bytes,
                    .replacement = replacement,
                });
            }
        } else if (!state.env_path && draft.soul_flow_enabled) {
            return save_failure(AgentSetupFailure::invalid_draft,
                "soul flow cannot be enabled without a configured env_file");
        }
        if (init != state.init_document) {
            plan.push_back({
                .absolute_path = project_root_ / ".lingtai"
                    / state.agent_key / "init.json",
                .original = state.init_bytes,
                .replacement = serialize(init),
            });
        }
        if (identity != state.agent_document) {
            plan.push_back({
                .absolute_path = project_root_ / ".lingtai"
                    / state.agent_key / ".agent.json",
                .original = state.agent_bytes,
                .replacement = serialize(identity),
            });
        }
        const auto propagate_presets = !draft.allowed_presets.empty();
        const auto propagate_orchestrator = is_orchestrator(manifest);
        for (const auto &peer : state.peers) {
            auto peer_init = peer.init_document;
            auto peer_manifest = peer_init.value("manifest").toObject();
            if (propagate_presets && peer_manifest.value("preset").isObject()) {
                auto peer_preset = peer_manifest.value("preset").toObject();
                const auto peer_active = nonempty_string(peer_preset, "active")
                    .value_or("");
                const auto active = std::ranges::find(policy.allowed, peer_active)
                        != policy.allowed.end()
                    ? peer_active : policy.default_ref;
                peer_preset["active"] = QString::fromStdString(active);
                peer_preset["default"] = QString::fromStdString(policy.default_ref);
                peer_preset["allowed"] = json_array(policy.allowed);
                peer_manifest["preset"] = peer_preset;
            }
            if (propagate_orchestrator) {
                peer_manifest["llm"] = manifest.value("llm");
                peer_manifest["capabilities"] = manifest.value("capabilities");
                peer_manifest["soul"] = manifest.value("soul");
                peer_manifest["context_limit"] = manifest.value("context_limit");
                patch_admin(peer_manifest, false, false);
                peer_init.remove("addons");
                if (const auto env_file = nonempty_string(init, "env_file")) {
                    peer_init["env_file"] = QString::fromStdString(*env_file);
                }
            }
            peer_init["manifest"] = peer_manifest;
            if (peer_init != peer.init_document) {
                plan.push_back({
                    .absolute_path = project_root_ / ".lingtai"
                        / peer.directory_key / "init.json",
                    .original = peer.init_bytes,
                    .replacement = serialize(peer_init),
                });
            }
        }

        if (plan.empty()) {
            return {
                .status = AgentSetupSaveStatus::no_change,
                .failure = AgentSetupFailure::none,
                .detail = {},
            };
        }

        staged.reserve(plan.size());
        for (const auto &target : plan) {
            auto parent = open_absolute_parent(target.absolute_path);
            if (parent.directory.get() < 0) {
                cleanup_staged(staged);
                return save_failure(
                    AgentSetupFailure::source_changed, "target parent changed or is unsafe");
            }
            const auto current = read_leaf(
                parent.directory.get(), parent.leaf,
                std::max(kMaximumJsonBytes, kMaximumEnvBytes));
            if (current.kind != LeafKind::read || current.bytes != target.original) {
                cleanup_staged(staged);
                return save_failure(
                    AgentSetupFailure::source_changed, "a setup source changed after load");
            }
            StagedTarget item{
                .directory = std::move(parent.directory),
                .leaf = parent.leaf,
                .temp_leaf = transaction_leaf(".tmp"),
                .backup_leaf = transaction_leaf(".bak"),
            };
            const auto fd = ::openat(item.directory.get(), item.temp_leaf.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                current.mode & 07777);
            if (fd < 0) {
                cleanup_staged(staged);
                return save_failure(
                    AgentSetupFailure::staging_failed, "cannot create setup stage file");
            }
            posix::FileDescriptor stage(fd);
            if (::fchmod(stage.get(), current.mode & 07777) != 0
                    || !write_all(stage.get(), target.replacement)
                    || ::fsync(stage.get()) != 0) {
                ::unlinkat(item.directory.get(), item.temp_leaf.c_str(), 0);
                cleanup_staged(staged);
                return save_failure(
                    AgentSetupFailure::staging_failed, "cannot fully stage setup file");
            }
            staged.push_back(std::move(item));
            if (failure_point == AgentSetupFailurePoint::staging_after_first
                    && staged.size() == 1U) {
                cleanup_staged(staged);
                return save_failure(
                    AgentSetupFailure::staging_failed, "injected staging failure");
            }
        }

        for (std::size_t index = 0; index < staged.size(); ++index) {
            auto &target = staged[index];
            if (::linkat(target.directory.get(), target.leaf.c_str(),
                    target.directory.get(), target.backup_leaf.c_str(), 0) != 0) {
                const auto restored = rollback(staged);
                return save_failure(restored
                        ? AgentSetupFailure::publish_failed
                        : AgentSetupFailure::rollback_failed,
                    "cannot create setup rollback link");
            }
            target.backup_created = true;
            if (::renameat(target.directory.get(), target.temp_leaf.c_str(),
                    target.directory.get(), target.leaf.c_str()) != 0) {
                const auto restored = rollback(staged);
                return save_failure(restored
                        ? AgentSetupFailure::publish_failed
                        : AgentSetupFailure::rollback_failed,
                    "cannot atomically publish setup file");
            }
            target.temp_leaf.clear();
            target.published = true;
            if (::fsync(target.directory.get()) != 0) {
                const auto restored = rollback(staged);
                return save_failure(restored
                        ? AgentSetupFailure::publish_failed
                        : AgentSetupFailure::rollback_failed,
                    "cannot sync setup publication");
            }
            if (failure_point == AgentSetupFailurePoint::publish_after_first
                    && index == 0U) {
                const auto restored = rollback(staged);
                return save_failure(restored
                        ? AgentSetupFailure::publish_failed
                        : AgentSetupFailure::rollback_failed,
                    "injected publish failure");
            }
        }

        for (auto &target : staged) {
            if (target.backup_created) {
                ::unlinkat(target.directory.get(), target.backup_leaf.c_str(), 0);
                target.backup_created = false;
                ::fsync(target.directory.get());
            }
        }
        return {
            .status = AgentSetupSaveStatus::saved,
            .failure = AgentSetupFailure::none,
            .detail = {},
        };
    } catch (...) {
        const auto restored = rollback(staged);
        return save_failure(
            restored ? AgentSetupFailure::local_failure
                     : AgentSetupFailure::rollback_failed,
            "unexpected local setup save failure");
    }
}

} // namespace lingtai::desktop
