#include "agent_preset_summary.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace lingtai::desktop {
namespace {

namespace posix = posix_internal;

// A fixed Desktop cap, not read from producer config: comfortably covers
// the current kernel-published artifact while refusing (never truncating)
// anything unexpectedly large.
constexpr std::size_t kMaxArtifactBytes = 1024U * 1024U;

enum class ComponentOutcome { absent, unsafe, ok };

ComponentOutcome classify_directory(int parent_fd, const char *name) {
    struct stat leaf {};
    if (::fstatat(parent_fd, name, &leaf, AT_SYMLINK_NOFOLLOW) != 0) {
        return ComponentOutcome::absent;
    }
    if (S_ISLNK(leaf.st_mode) || !S_ISDIR(leaf.st_mode)) {
        return ComponentOutcome::unsafe;
    }
    return ComponentOutcome::ok;
}

ComponentOutcome classify_regular_file(int parent_fd, const char *name) {
    struct stat leaf {};
    if (::fstatat(parent_fd, name, &leaf, AT_SYMLINK_NOFOLLOW) != 0) {
        return ComponentOutcome::absent;
    }
    if (S_ISLNK(leaf.st_mode) || !S_ISREG(leaf.st_mode)) {
        return ComponentOutcome::unsafe;
    }
    return ComponentOutcome::ok;
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

struct ParsedResolved {
    std::string active_ref, default_ref;
    std::vector<std::string> allowed_refs;
    std::string provider, model;
    std::optional<std::int64_t> context_limit;
    std::vector<std::string> capability_names;
    std::string generated_at;
};

// Requires the exact supported v1 envelope and a complete narrow
// projection; any missing/wrong-typed/unsupported field refuses the whole
// artifact rather than returning a partially parsed summary.
[[nodiscard]] std::optional<ParsedResolved> parse_resolved_manifest(
        const std::string &bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<int>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto root = document.object();

    const auto schema = nonempty_string(root, "schema");
    if (!schema || *schema != "lingtai.manifest.resolved/v1") return std::nullopt;
    const auto schema_version = exact_integer(root.value("schema_version"));
    if (!schema_version || *schema_version != 1) return std::nullopt;
    const auto source = nonempty_string(root, "source");
    if (!source || *source != "kernel") return std::nullopt;
    const auto generated_at = nonempty_string(root, "generated_at");
    if (!generated_at) return std::nullopt;

    const auto preset_value = root.value("preset");
    if (!preset_value.isObject()) return std::nullopt;
    const auto preset = preset_value.toObject();
    const auto active_ref = nonempty_string(preset, "active");
    const auto default_ref = nonempty_string(preset, "default");
    if (!active_ref || !default_ref) return std::nullopt;
    const auto allowed_value = preset.value("allowed");
    if (!allowed_value.isArray()) return std::nullopt;
    std::vector<std::string> allowed_refs;
    for (const auto &entry : allowed_value.toArray()) {
        if (!entry.isString()) return std::nullopt;
        auto text = entry.toString().toStdString();
        if (text.empty()) return std::nullopt;
        allowed_refs.push_back(std::move(text));
    }
    if (allowed_refs.empty()) return std::nullopt;

    const auto manifest_value = root.value("manifest");
    if (!manifest_value.isObject()) return std::nullopt;
    const auto manifest = manifest_value.toObject();
    const auto llm_value = manifest.value("llm");
    if (!llm_value.isObject()) return std::nullopt;
    const auto llm = llm_value.toObject();
    const auto provider = nonempty_string(llm, "provider");
    const auto model = nonempty_string(llm, "model");
    if (!provider || !model) return std::nullopt;
    // The kernel only writes effective `manifest.context_limit` when the
    // active preset itself declared one (`context_limit` is an optional
    // preset field); its absence is a normal narrow outcome, not an
    // incomplete artifact. A *present* value that fails to parse as an
    // exact integer is still treated as malformed.
    std::optional<std::int64_t> context_limit;
    if (manifest.contains("context_limit")) {
        context_limit = exact_integer(manifest.value("context_limit"));
        if (!context_limit) return std::nullopt;
    }

    const auto capabilities_value = manifest.value("capabilities");
    if (!capabilities_value.isObject()) return std::nullopt;
    std::vector<std::string> capability_names;
    const auto capabilities = capabilities_value.toObject();
    for (auto it = capabilities.constBegin(); it != capabilities.constEnd();
            ++it) {
        capability_names.push_back(it.key().toStdString());
    }
    std::ranges::sort(capability_names);

    ParsedResolved result;
    result.active_ref = *active_ref;
    result.default_ref = *default_ref;
    result.allowed_refs = std::move(allowed_refs);
    result.provider = *provider;
    result.model = *model;
    result.context_limit = context_limit;
    result.capability_names = std::move(capability_names);
    result.generated_at = *generated_at;
    return result;
}

[[nodiscard]] bool read_bounded_complete(
        int fd, std::size_t max_bytes, std::string &bytes) {
    struct stat opened {};
    if (::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode)) return false;
    const auto size = static_cast<std::uintmax_t>(opened.st_size);
    if (size > static_cast<std::uintmax_t>(max_bytes)) return false;
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::read(fd, bytes.data() + total, bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    return total == size;
}

} // namespace

AgentPresetSummary read_agent_preset_summary(
        const ProjectAttachment &attachment,
        const std::filesystem::path &selected_directory_key) noexcept {
    try {
        if (!posix::safe_leaf(selected_directory_key)) return {};
        const auto root = posix::open_root_directory(attachment.root());
        if (root.get() < 0) return {};
        const auto lingtai =
            posix::open_directory_component(root.get(), ".lingtai");
        if (lingtai.get() < 0) return {};
        const auto agent = posix::open_directory_component(
            lingtai.get(), selected_directory_key);
        if (agent.get() < 0) return {};

        // `init.json` is opened only for a regular-file mtime comparison;
        // its content is never read or parsed, and a missing/unsafe
        // `init.json` simply yields no staleness evidence.
        std::optional<std::time_t> init_mtime;
        if (classify_regular_file(agent.get(), "init.json")
                == ComponentOutcome::ok) {
            const auto init_file =
                posix::open_regular_file_component(agent.get(), "init.json");
            if (init_file.get() >= 0) {
                struct stat init_stat {};
                if (::fstat(init_file.get(), &init_stat) == 0) {
                    init_mtime = init_stat.st_mtime;
                }
            }
        }

        const auto system_outcome = classify_directory(agent.get(), "system");
        if (system_outcome == ComponentOutcome::absent) {
            AgentPresetSummary result;
            result.source = AgentPresetSummarySource::not_yet_published;
            return result;
        }
        if (system_outcome == ComponentOutcome::unsafe) return {};
        const auto system_dir =
            posix::open_directory_component(agent.get(), "system");
        if (system_dir.get() < 0) return {};

        const auto leaf_outcome =
            classify_regular_file(system_dir.get(), "manifest.resolved.json");
        if (leaf_outcome == ComponentOutcome::absent) {
            AgentPresetSummary result;
            result.source = AgentPresetSummarySource::not_yet_published;
            return result;
        }
        if (leaf_outcome == ComponentOutcome::unsafe) return {};
        const auto artifact = posix::open_regular_file_component(
            system_dir.get(), "manifest.resolved.json");
        if (artifact.get() < 0) return {};

        struct stat artifact_stat {};
        if (::fstat(artifact.get(), &artifact_stat) != 0) return {};

        std::string bytes;
        if (!read_bounded_complete(artifact.get(), kMaxArtifactBytes, bytes)) {
            return {};
        }
        auto parsed = parse_resolved_manifest(bytes);
        if (!parsed) return {};

        AgentPresetSummary result;
        result.source =
            (init_mtime && artifact_stat.st_mtime < *init_mtime)
                ? AgentPresetSummarySource::stale
                : AgentPresetSummarySource::resolved;
        result.active_ref = parsed->active_ref;
        result.default_ref = parsed->default_ref;
        result.allowed.reserve(parsed->allowed_refs.size());
        for (auto &ref : parsed->allowed_refs) {
            const auto is_active = ref == parsed->active_ref;
            const auto is_default = ref == parsed->default_ref;
            result.allowed.push_back(
                AgentPresetRef{std::move(ref), is_active, is_default});
        }
        result.effective.provider = parsed->provider;
        result.effective.model = parsed->model;
        result.effective.context_limit = parsed->context_limit;
        result.effective.capability_names = std::move(parsed->capability_names);
        result.generated_at = parsed->generated_at;
        return result;
    } catch (...) {
        // Only string/vector allocation can throw here. Show nothing rather
        // than a partial projection that looks complete.
        return {};
    }
}

} // namespace lingtai::desktop
