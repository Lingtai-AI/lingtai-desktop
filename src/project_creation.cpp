#include "project_creation.h"

#include "posix_descriptor_primitives.h"
#include "project_creation_resources.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QMetaObject>
#include <QtCore/QDateTime>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <fcntl.h>
#include <ranges>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

constexpr auto kMaximumJsonBytes = std::size_t{1} << 20;
constexpr std::string_view kStagingPrefix = ".lingtai.create-";
constexpr std::string_view kStagingMarker = ".desktop-create-owner";

ProjectCreationResult failure(
        ProjectCreationFailure kind,
        ProjectCreationStage stage,
        std::string detail) {
    return {
        .created = false,
        .failure = kind,
        .stage = stage,
        .detail = std::move(detail),
    };
}

bool plain_absolute_path(const fs::path &path) {
    if (!path.is_absolute() || path.has_root_name()
            || !path.has_root_directory()) {
        return false;
    }
    return std::ranges::none_of(path.relative_path(), [](const auto &part) {
        return part.empty() || part == "." || part == ".."
            || !posix::safe_leaf(part);
    });
}

posix::FileDescriptor open_absolute_directory(const fs::path &path) {
    if (!plain_absolute_path(path)) return posix::FileDescriptor();
    auto current = posix::open_root_directory(path.root_path());
    if (current.get() < 0) return current;
    for (const auto &part : path.relative_path()) {
        auto next = posix::open_directory_component(current.get(), part);
        if (next.get() < 0) return posix::FileDescriptor();
        current = std::move(next);
    }
    return current;
}

std::optional<std::string> read_absolute_regular(
        const fs::path &path, std::size_t cap, bool require_executable = false) {
    if (!plain_absolute_path(path) || path.filename().empty()) {
        return std::nullopt;
    }
    auto parent = open_absolute_directory(path.parent_path());
    if (parent.get() < 0) return std::nullopt;
    auto file = posix::open_regular_file_component(parent.get(), path.filename());
    if (file.get() < 0) return std::nullopt;
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)
            || opened.st_size < 0
            || static_cast<std::uintmax_t>(opened.st_size) > cap
            || (require_executable && (opened.st_mode & 0111) == 0)) {
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
    return bytes;
}

std::optional<std::string> read_regular_component(
        int parent, const fs::path &leaf, std::size_t cap) {
    auto file = posix::open_regular_file_component(parent, leaf);
    if (file.get() < 0) return std::nullopt;
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || opened.st_size < 0
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
    return bytes;
}

bool optional_absolute_reference(const fs::path &path) {
    return path.empty() || plain_absolute_path(path);
}

bool blank(std::string_view value) {
    return std::ranges::all_of(value, [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    });
}

bool unresolved_placeholder(std::string_view value) {
    return value.find("{{") != std::string_view::npos
        || value.find("}}") != std::string_view::npos;
}

void replace_all(std::string &value, std::string_view token,
        std::string_view replacement) {
    auto offset = std::size_t{0};
    while ((offset = value.find(token, offset)) != std::string::npos) {
        value.replace(offset, token.size(), replacement);
        offset += replacement.size();
    }
}

std::optional<std::string> render_greeting(
        const ProjectCreationResources &resources,
        const ProjectCreationRequest &request) {
    auto result = std::string(resources.greeting_template);
    const auto timestamp = request.guidance_local_time
        ? request.guidance_local_time()
        : QDateTime::currentDateTime()
            .toString(QStringLiteral("yyyy-MM-dd HH:mm")).toStdString();
    auto location = request.guidance_cached_location
        ? request.guidance_cached_location() : std::string{};
    if (blank(location)) location = "unknown";
    const auto soul_delay = request.setup.soul_delay
        ? QString::number(*request.setup.soul_delay, 'g', 15).toStdString()
        : std::string("kernel default");
    for (const auto &[token, replacement] : {
            std::pair<std::string_view, std::string_view>{
                "{{time}}", timestamp},
            {"{{location}}", location},
            {"{{lang}}", resources.language},
            {"{{soul_delay}}", soul_delay},
            {"{{addr}}", "human"},
        }) {
        replace_all(result, token, replacement);
    }
    if (result.size() > kMaximumJsonBytes || blank(result)
            || unresolved_placeholder(result)) {
        return std::nullopt;
    }
    return result;
}

bool valid_agent_leaf(std::string_view name) {
    if (name.empty() || name == "." || name == ".."
            || name.size() > 255U) {
        return false;
    }
    return std::ranges::none_of(name, [](unsigned char ch) {
        return ch == '/' || ch == '\\' || ch == '\0';
    });
}

std::optional<QJsonObject> load_preset(const fs::path &path) {
    const auto bytes = read_absolute_regular(path, kMaximumJsonBytes);
    if (!bytes) return std::nullopt;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes->data(), static_cast<qsizetype>(bytes->size())),
        &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto root = document.object();
    const auto name = root.value("name");
    const auto manifest = root.value("manifest");
    if (!name.isString() || name.toString().trimmed().isEmpty()
            || !manifest.isObject()) {
        return std::nullopt;
    }
    const auto values = manifest.toObject();
    if (!values.value("llm").isObject()
            || !values.value("capabilities").isObject()) {
        return std::nullopt;
    }
    return root;
}

std::optional<QJsonObject> normalize_legacy_capabilities(
        QJsonObject capabilities) {
    if (!capabilities.contains("bash")) return capabilities;
    const auto legacy = capabilities.value("bash");
    if (capabilities.contains("shell")
            && capabilities.value("shell") != legacy) {
        return std::nullopt;
    }
    capabilities["shell"] = legacy;
    capabilities.remove("bash");
    return capabilities;
}

void propagate_provider_api_key_env(
        const QJsonObject &llm, QJsonObject &capabilities) {
    const auto provider = llm.value("provider").toString();
    const auto api_key_env = llm.value("api_key_env").toString();
    if (provider.isEmpty() || api_key_env.isEmpty()) return;
    for (auto capability = capabilities.begin();
            capability != capabilities.end(); ++capability) {
        if (!capability.value().isObject()) continue;
        auto values = capability.value().toObject();
        if (values.value("provider").toString() != provider) continue;
        values["api_key_env"] = api_key_env;
        capability.value() = values;
    }
}

std::optional<QJsonObject> parse_object(std::string_view bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

bool write_all(int fd, std::string_view bytes) {
    auto written = std::size_t{0};
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

bool make_directory(int parent, const fs::path &leaf, mode_t mode) {
    if (!posix::safe_leaf(leaf) || ::mkdirat(parent, leaf.c_str(), mode) != 0) {
        return false;
    }
    auto opened = posix::open_directory_component(parent, leaf);
    return opened.get() >= 0 && ::fchmod(opened.get(), mode) == 0;
}

posix::FileDescriptor make_and_open_directory(
        int parent, const fs::path &leaf, mode_t mode = 0755) {
    if (!make_directory(parent, leaf, mode)) return posix::FileDescriptor();
    return posix::open_directory_component(parent, leaf);
}

bool write_new_file(int parent, const fs::path &leaf,
        std::string_view bytes, mode_t mode = 0644) {
    if (!posix::safe_leaf(leaf)) return false;
    posix::FileDescriptor file(::openat(parent, leaf.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    return file.get() >= 0
        && ::fchmod(file.get(), mode) == 0
        && write_all(file.get(), bytes)
        && ::fsync(file.get()) == 0;
}

std::string json_bytes(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Indented).toStdString();
}

QJsonArray json_array(const std::vector<std::string> &values) {
    QJsonArray result;
    for (const auto &value : values) {
        result.append(QString::fromStdString(value));
    }
    return result;
}

bool make_mailbox(int owner) {
    auto mailbox = make_and_open_directory(owner, "mailbox");
    return mailbox.get() >= 0
        && make_directory(mailbox.get(), "inbox", 0755)
        && make_directory(mailbox.get(), "sent", 0755)
        && make_directory(mailbox.get(), "archive", 0755)
        && ::fsync(mailbox.get()) == 0;
}

std::optional<std::vector<std::string>> directory_children(int directory) {
    const auto scan_fd = ::openat(directory, ".",
        posix::read_flags() | O_DIRECTORY);
    if (scan_fd < 0) return std::nullopt;
    posix::DirectoryStream entries(::fdopendir(scan_fd));
    if (!entries.get()) {
        ::close(scan_fd);
        return std::nullopt;
    }
    auto result = std::vector<std::string>();
    errno = 0;
    while (const auto *entry = ::readdir(entries.get())) {
        const auto leaf = std::string(entry->d_name);
        if (leaf == "." || leaf == "..") continue;
        if (!posix::safe_leaf(fs::path(leaf))) return std::nullopt;
        result.push_back(leaf);
    }
    if (errno != 0) return std::nullopt;
    std::ranges::sort(result);
    return result;
}

bool has_mailbox_shape(int owner) {
    const auto mailbox = posix::open_directory_component(owner, "mailbox");
    if (mailbox.get() < 0) return false;
    for (const auto *leaf : {"archive", "inbox", "sent"}) {
        if (posix::open_directory_component(mailbox.get(), leaf).get() < 0) {
            return false;
        }
    }
    return true;
}

bool has_truthy_admin(const QJsonObject &identity) {
    const auto admin = identity.value("admin");
    if (!admin.isObject()) return false;
    return std::ranges::any_of(admin.toObject(), [](const QJsonValue &value) {
        return value.isBool() && value.toBool();
    });
}

bool json_array_matches(
        const QJsonArray &array, const std::vector<std::string> &expected) {
    if (array.size() != static_cast<qsizetype>(expected.size())) return false;
    for (auto index = std::size_t{0}; index != expected.size(); ++index) {
        if (array.at(static_cast<qsizetype>(index)).toString().toStdString()
                != expected[index]) {
            return false;
        }
    }
    return true;
}

bool validate_staged_project(
        int staging,
        const ProjectCreationRequest &request,
        const AgentSetupPresetPolicy &policy,
        std::string_view expected_language,
        std::string_view expected_greeting,
        std::string_view expected_comment,
        bool default_comment) {
    const auto children = directory_children(staging);
    auto expected = std::vector<std::string>{
        std::string(kStagingMarker), ".library_shared", "human",
        request.agent_directory,
    };
    std::ranges::sort(expected);
    if (!children || *children != expected) return false;

    const auto human = posix::open_directory_component(staging, "human");
    const auto shared = posix::open_directory_component(
        staging, ".library_shared");
    const auto agent = posix::open_directory_component(
        staging, fs::path(request.agent_directory));
    if (human.get() < 0 || shared.get() < 0 || agent.get() < 0
            || !has_mailbox_shape(human.get())
            || !has_mailbox_shape(agent.get())) {
        return false;
    }
    auto expected_agent_children = std::vector<std::string>{
        ".agent.json", ".prompt", "comment.md", "init.json", "mailbox",
    };
    const auto agent_children = directory_children(agent.get());
    std::ranges::sort(expected_agent_children);
    if (!agent_children || *agent_children != expected_agent_children) {
        return false;
    }

    const auto human_bytes = read_regular_component(
        human.get(), ".agent.json", kMaximumJsonBytes);
    const auto agent_bytes = read_regular_component(
        agent.get(), ".agent.json", kMaximumJsonBytes);
    const auto init_bytes = read_regular_component(
        agent.get(), "init.json", kMaximumJsonBytes);
    const auto greeting = read_regular_component(
        agent.get(), ".prompt", kMaximumJsonBytes);
    const auto comment = read_regular_component(
        agent.get(), "comment.md", kMaximumJsonBytes);
    if (!human_bytes || !agent_bytes || !init_bytes || !greeting || !comment
            || *greeting != expected_greeting || *comment != expected_comment
            || blank(*greeting) || blank(*comment)
            || unresolved_placeholder(*greeting)
            || (default_comment && unresolved_placeholder(*comment))) {
        return false;
    }
    const auto human_identity = parse_object(*human_bytes);
    const auto agent_identity = parse_object(*agent_bytes);
    const auto init = parse_object(*init_bytes);
    if (!human_identity || !agent_identity || !init
            || !human_identity->value("admin").isNull()
            || has_truthy_admin(*human_identity)
            || !has_truthy_admin(*agent_identity)
            || agent_identity->value("agent_name").toString().toStdString()
                != request.agent_name
            || agent_identity->value("address").toString().toStdString()
                != request.agent_directory) {
        return false;
    }

    const auto manifest_value = init->value("manifest");
    if (!manifest_value.isObject()) return false;
    const auto manifest = manifest_value.toObject();
    const auto preset_value = manifest.value("preset");
    if (!manifest.value("llm").isObject()
            || !manifest.value("capabilities").isObject()
            || manifest.value("agent_name").toString().toStdString()
                != request.agent_name
            || manifest.value("language").toString().toStdString()
                != expected_language
            || !preset_value.isObject()) {
        return false;
    }
    const auto preset = preset_value.toObject();
    if (preset.value("active").toString().toStdString() != policy.active
            || preset.value("default").toString().toStdString()
                != policy.default_ref
            || !json_array_matches(
                preset.value("allowed").toArray(), policy.allowed)) {
        return false;
    }

    const auto expected_venv =
        request.runtime_python.parent_path().parent_path().string();
    if (init->value("env_file").toString().toStdString()
                != request.env_file.string()
            || init->value("venv_path").toString().toStdString()
                != expected_venv) {
        return false;
    }
    if (!request.covenant_file.empty()
            && init->value("covenant_file").toString().toStdString()
                != request.covenant_file.string()) {
        return false;
    }
    const auto final_comment = request.destination / ".lingtai"
        / request.agent_directory / "comment.md";
    if (init->value("comment_file").toString().toStdString()
            != final_comment.string()) {
        return false;
    }
    return true;
}

std::atomic_uint64_t next_stage{0};

std::string stage_name() {
    return std::string(kStagingPrefix) + std::to_string(::getpid()) + "-"
        + std::to_string(next_stage.fetch_add(1, std::memory_order_relaxed));
}

bool same_directory(const struct stat &value, dev_t device, ino_t inode) {
    return S_ISDIR(value.st_mode) && value.st_dev == device
        && value.st_ino == inode;
}

bool remove_directory_children(int directory) {
    const auto scan_fd = ::openat(directory, ".",
        posix::read_flags() | O_DIRECTORY);
    if (scan_fd < 0) return false;
    posix::DirectoryStream entries(::fdopendir(scan_fd));
    if (!entries.get()) {
        ::close(scan_fd);
        return false;
    }

    auto leaves = std::vector<std::string>();
    errno = 0;
    while (const auto *entry = ::readdir(entries.get())) {
        const auto leaf = std::string(entry->d_name);
        if (leaf == "." || leaf == "..") continue;
        if (!posix::safe_leaf(fs::path(leaf))) return false;
        leaves.push_back(leaf);
    }
    if (errno != 0) return false;

    for (const auto &leaf : leaves) {
        struct stat observed {};
        if (::fstatat(directory, leaf.c_str(), &observed,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return false;
        }
        if (!S_ISDIR(observed.st_mode)) {
            if (::unlinkat(directory, leaf.c_str(), 0) != 0) return false;
            continue;
        }
        auto child = posix::open_directory_component(directory, leaf);
        struct stat opened {};
        if (child.get() < 0 || ::fstat(child.get(), &opened) != 0
                || !same_directory(opened, observed.st_dev, observed.st_ino)
                || !remove_directory_children(child.get())) {
            return false;
        }
        struct stat current {};
        if (::fstatat(directory, leaf.c_str(), &current,
                AT_SYMLINK_NOFOLLOW) != 0
                || !same_directory(current, opened.st_dev, opened.st_ino)
                || ::unlinkat(directory, leaf.c_str(), AT_REMOVEDIR) != 0) {
            return false;
        }
    }
    return true;
}

bool remove_owned_stage(int destination, int staging,
        const std::string &stage, dev_t device, ino_t inode,
        bool require_marker) {
    struct stat opened {};
    if (::fstat(staging, &opened) != 0
            || !same_directory(opened, device, inode)) {
        return false;
    }
    if (require_marker) {
        const auto marker = read_regular_component(
            staging, kStagingMarker, 512);
        if (!marker || *marker != stage + "\n") return false;
    }
    if (!remove_directory_children(staging)) return false;
    struct stat current {};
    if (::fstatat(destination, stage.c_str(), &current,
            AT_SYMLINK_NOFOLLOW) != 0
            || !same_directory(current, device, inode)) {
        return false;
    }
    return ::unlinkat(destination, stage.c_str(), AT_REMOVEDIR) == 0;
}

bool publish_no_replace(int parent, const std::string &from,
        const char *to) {
#if defined(__APPLE__)
    return ::renameatx_np(parent, from.c_str(), parent, to, RENAME_EXCL) == 0;
#elif defined(__linux__) && defined(SYS_renameat2)
    return ::syscall(SYS_renameat2, parent, from.c_str(), parent, to,
        RENAME_NOREPLACE) == 0;
#else
    struct stat existing {};
    if (::fstatat(parent, to, &existing, AT_SYMLINK_NOFOLLOW) == 0
            || errno != ENOENT) {
        return false;
    }
    return ::renameat(parent, from.c_str(), parent, to) == 0;
#endif
}

} // namespace

const char *project_creation_stage_name(ProjectCreationStage stage) noexcept {
    switch (stage) {
    case ProjectCreationStage::none: return "none";
    case ProjectCreationStage::draft_validation: return "draft_validation";
    case ProjectCreationStage::staging: return "staging";
    case ProjectCreationStage::staged_generation: return "staged_generation";
    case ProjectCreationStage::staged_validation: return "staged_validation";
    case ProjectCreationStage::publication: return "publication";
    case ProjectCreationStage::complete: return "complete";
    }
    return "unknown";
}

ProjectCreationResult create_project(
        const ProjectCreationRequest &request) noexcept {
    try {
        if (!plain_absolute_path(request.destination)) {
            return failure(ProjectCreationFailure::invalid_destination,
                ProjectCreationStage::draft_validation,
                "destination must be an existing absolute directory without traversal");
        }
        auto destination = open_absolute_directory(request.destination);
        if (destination.get() < 0) {
            return failure(ProjectCreationFailure::unsafe_path,
                ProjectCreationStage::draft_validation,
                "destination is unavailable or contains a symlink");
        }
        if (!valid_agent_leaf(request.agent_directory)
                || request.agent_name.empty() || blank(request.agent_name)
                || request.agent_name.size() > 255U) {
            return failure(ProjectCreationFailure::invalid_agent_name,
                ProjectCreationStage::draft_validation,
                "Agent name and folder must be nonempty contained names");
        }
        if (!optional_absolute_reference(request.runtime_python)
                || !optional_absolute_reference(request.env_file)
                || !optional_absolute_reference(request.covenant_file)
                || request.comment.size() > kMaximumJsonBytes) {
            return failure(ProjectCreationFailure::runtime_unavailable,
                ProjectCreationStage::draft_validation,
                "runtime, environment, covenant, and comment references must have bounded absolute-path shape");
        }
        struct stat existing {};
        if (::fstatat(destination.get(), ".lingtai", &existing,
                AT_SYMLINK_NOFOLLOW) == 0) {
            return failure(ProjectCreationFailure::existing_project,
                ProjectCreationStage::staging,
                ".lingtai already exists; existing project state was preserved");
        }
        if (errno != ENOENT) {
            return failure(ProjectCreationFailure::unsafe_path,
                ProjectCreationStage::staging,
                "the destination .lingtai leaf could not be inspected safely");
        }

        // Only the reviewed selected preset is needed to begin. Keep its
        // parsed shape in memory across the transaction; allowed dependency
        // reads and policy normalization belong to staged generation below.
        const auto selected_preset = load_preset(request.preset_path);
        if (!selected_preset) {
            return failure(ProjectCreationFailure::invalid_preset,
                ProjectCreationStage::draft_validation,
                "selected preset is unreadable, unsafe, oversized, or malformed");
        }
        const auto &localized = project_creation_resources(
            request.setup.language);

        const auto stage_leaf = stage_name();
        if (!make_directory(destination.get(), stage_leaf, 0700)) {
            return failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staging,
                "could not create the owned project staging directory");
        }
        auto staging = posix::open_directory_component(
            destination.get(), stage_leaf);
        if (staging.get() < 0) {
            return failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staging,
                "could not open the owned project staging directory; it was preserved because its identity could not be verified");
        }
        struct stat staging_identity {};
        if (::fstat(staging.get(), &staging_identity) != 0
                || !S_ISDIR(staging_identity.st_mode)) {
            return failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staging,
                "could not identify the owned project staging directory; it was preserved because its identity could not be verified");
        }
        auto marker_present = false;
        const auto cleanup = [&] {
            return remove_owned_stage(destination.get(), staging.get(),
                stage_leaf, staging_identity.st_dev, staging_identity.st_ino,
                marker_present);
        };
        const auto staged_failure = [&](ProjectCreationFailure kind,
                ProjectCreationStage stage, std::string detail) {
            if (!cleanup()) {
                detail += "; rollback could not safely remove the owned staging directory";
            }
            return failure(kind, stage, std::move(detail));
        };
        if (!write_new_file(staging.get(), kStagingMarker,
                stage_leaf + "\n", 0600)) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staging,
                "could not mark the owned project staging directory");
        }
        marker_present = true;
        if (request.failure_point == ProjectCreationFailurePoint::after_staging) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staging,
                "injected failure after staging");
        }

        auto human = make_and_open_directory(staging.get(), "human");
        auto shared = make_and_open_directory(staging.get(), ".library_shared");
        auto agent = make_and_open_directory(
            staging.get(), fs::path(request.agent_directory));
        if (human.get() < 0 || shared.get() < 0 || agent.get() < 0
                || !make_mailbox(human.get()) || !make_mailbox(agent.get())) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_generation,
                "could not build the staged project directories");
        }
        const QJsonObject human_identity{
            {"agent_name", "human"}, {"address", "human"},
            {"admin", QJsonValue(QJsonValue::Null)},
        };
        auto human_mailbox = posix::open_directory_component(human.get(), "mailbox");
        if (!write_new_file(human.get(), ".agent.json",
                json_bytes(human_identity))
                || human_mailbox.get() < 0
                || !write_new_file(human_mailbox.get(), "contacts.json", "[]")
                || ::fsync(human_mailbox.get()) != 0) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_generation,
                "could not build the staged human mailbox");
        }

        auto requested_allowed = std::vector<std::string>();
        for (const auto &path : request.allowed_preset_paths) {
            if (!load_preset(path)) {
                return staged_failure(ProjectCreationFailure::invalid_preset,
                    ProjectCreationStage::staged_generation,
                    "an allowed preset is unreadable, unsafe, oversized, or malformed");
            }
            const auto text = path.string();
            if (std::ranges::find(requested_allowed, text)
                    == requested_allowed.end()) {
                requested_allowed.push_back(text);
            }
        }
        const auto selected_manifest =
            selected_preset->value("manifest").toObject();
        const auto selected_llm = selected_manifest.value("llm").toObject();
        auto selected_capabilities = normalize_legacy_capabilities(
            selected_manifest.value("capabilities").toObject());
        if (!selected_capabilities) {
            return staged_failure(ProjectCreationFailure::invalid_preset,
                ProjectCreationStage::staged_generation,
                "selected preset has conflicting bash and shell capabilities");
        }
        propagate_provider_api_key_env(
            selected_llm, *selected_capabilities);
        const AgentSetupPresetSelection selection{
            .choice = AgentSetupPresetChoice::select_preset,
            .reference = request.preset_path.string(),
            .manifest = QJsonObject{
                {"llm", selected_llm},
                {"capabilities", *selected_capabilities},
            },
        };
        const auto policy = reconcile_agent_setup_presets(
            {}, selection, requested_allowed);
        if (policy.active.empty() || policy.default_ref.empty()
                || policy.allowed.empty()) {
            return staged_failure(ProjectCreationFailure::invalid_preset,
                ProjectCreationStage::staged_generation,
                "selected preset could not form a valid setup policy");
        }

        const auto greeting = render_greeting(localized, request);
        if (!greeting) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_generation,
                "could not render the localized first-boot greeting safely");
        }
        const auto default_comment = request.comment.empty()
            || blank(request.comment);
        const auto comment = default_comment
            ? std::string(localized.adaptive_playbook)
            : request.comment;
        if (comment.size() > kMaximumJsonBytes || blank(comment)
                || (default_comment && unresolved_placeholder(comment))) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_generation,
                "localized or reviewed Agent comment content is not meaningful and bounded");
        }

        auto manifest = QJsonObject{
            {"llm", selected_llm},
            {"capabilities", *selected_capabilities},
        };
        manifest["agent_name"] = QString::fromStdString(request.agent_name);
        manifest["language"] = QString::fromUtf8(
            localized.language.data(),
            static_cast<qsizetype>(localized.language.size()));
        manifest["context_limit"] = request.setup.context_limit;
        manifest["max_turns"] = 500;
        manifest["max_rpm"] = request.setup.max_rpm;
        manifest["max_aed_attempts"] = request.setup.max_aed_attempts;
        manifest["streaming"] = false;
        manifest["admin"] = QJsonObject{
            {"karma", request.setup.karma},
            {"nirvana", request.setup.nirvana},
        };
        if (request.setup.soul_delay) {
            manifest["soul"] = QJsonObject{{"delay", *request.setup.soul_delay}};
        } else {
            manifest.remove("soul");
        }
        manifest["preset"] = QJsonObject{
            {"active", QString::fromStdString(policy.active)},
            {"default", QString::fromStdString(policy.default_ref)},
            {"allowed", json_array(policy.allowed)},
        };

        QJsonObject init{
            {"manifest", manifest},
            {"env_file", QString::fromStdString(request.env_file.string())},
            {"venv_path", QString::fromStdString(
                request.runtime_python.parent_path().parent_path().string())},
            {"pad", ""},
        };
        if (!request.covenant_file.empty()) {
            init["covenant_file"] = QString::fromStdString(
                request.covenant_file.string());
        }
        const auto final_comment = request.destination / ".lingtai"
            / request.agent_directory / "comment.md";
        init["comment_file"] = QString::fromStdString(final_comment.string());
        const QJsonObject agent_identity{
            {"agent_name", QString::fromStdString(request.agent_name)},
            {"address", QString::fromStdString(request.agent_directory)},
            {"admin", QJsonObject{
                {"karma", request.setup.karma},
                {"nirvana", request.setup.nirvana},
            }},
            {"state", ""},
        };
        if (!write_new_file(agent.get(), "init.json", json_bytes(init))
                || !write_new_file(agent.get(), ".agent.json",
                    json_bytes(agent_identity))
                || !write_new_file(agent.get(), ".prompt", *greeting)
                || !write_new_file(agent.get(), "comment.md", comment)) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_generation,
                "could not write the staged Agent configuration");
        }
        if (request.failure_point
                == ProjectCreationFailurePoint::after_generation) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_generation,
                "injected failure after staged generation");
        }
        if (!validate_staged_project(staging.get(), request, policy,
                localized.language, *greeting, comment,
                default_comment)) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_validation,
                "staged project failed bounded shape, preset, or exactly-one-orchestrator validation");
        }
        if (::fsync(human.get()) != 0 || ::fsync(shared.get()) != 0
                || ::fsync(agent.get()) != 0
                || ::fchmod(staging.get(), 0755) != 0
                || ::fsync(staging.get()) != 0) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::staged_validation,
                "could not finalize the staged project");
        }
        // Everything below the stage is durable before the marker is removed.
        // Publication follows immediately, leaving no ordinary fallible work
        // in the crash-only interval between these two namespace operations.
        if (::unlinkat(staging.get(), kStagingMarker.data(), 0) != 0) {
            return staged_failure(ProjectCreationFailure::staging_failed,
                ProjectCreationStage::publication,
                "could not finalize staged project ownership");
        }
        marker_present = false;
        if (request.failure_point
                == ProjectCreationFailurePoint::after_marker_removal) {
            return staged_failure(ProjectCreationFailure::publish_failed,
                ProjectCreationStage::publication,
                "injected failure after marker removal");
        }
        const auto published = request.failure_point
                == ProjectCreationFailurePoint::publish_refused
            ? false
            : publish_no_replace(destination.get(), stage_leaf, ".lingtai");
        if (!published) {
            return staged_failure(ProjectCreationFailure::publish_failed,
                ProjectCreationStage::publication,
                request.failure_point == ProjectCreationFailurePoint::publish_refused
                    ? "injected project publication refusal"
                    : "project publication was refused; destination state was preserved");
        }
        static_cast<void>(::fsync(staging.get()));
        static_cast<void>(::fsync(destination.get()));
        return {
            .created = true,
            .project_dir = request.destination,
            .agent_key = fs::path(request.agent_directory),
            .failure = ProjectCreationFailure::none,
            .stage = ProjectCreationStage::complete,
        };
    } catch (...) {
        return failure(ProjectCreationFailure::local_failure,
            ProjectCreationStage::none,
            "unexpected local project creation failure");
    }
}

struct ProjectCreationRunner::DeliveryState {
    std::atomic<bool> alive{true};
};

ProjectCreationRunner::ProjectCreationRunner()
: delivery_(std::make_shared<DeliveryState>()) {
}

ProjectCreationRunner::~ProjectCreationRunner() {
    delivery_->alive.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

bool ProjectCreationRunner::is_pending() const noexcept { return pending_; }

void ProjectCreationRunner::run_catalog(
        const QString &global_dir, PresetDone done) {
    if (pending_) return;
    if (worker_.joinable()) worker_.join();
    pending_ = true;
    auto state = delivery_;
    auto *context = &delivery_context_;
    worker_ = std::thread(
            [this, state, context, global_dir, done = std::move(done)]() mutable {
        auto result = load_preset_catalog(global_dir);
        if (!state->alive.load(std::memory_order_acquire)) return;
        QMetaObject::invokeMethod(context, [this, state, done = std::move(done),
                result = std::move(result)]() mutable {
            if (!state->alive.load(std::memory_order_acquire)) return;
            pending_ = false;
            if (done) done(std::move(result));
        }, Qt::QueuedConnection);
    });
}

void ProjectCreationRunner::run_create(
        ProjectCreationRequest request, CreateDone done) {
    if (pending_) return;
    if (worker_.joinable()) worker_.join();
    pending_ = true;
    auto state = delivery_;
    auto *context = &delivery_context_;
    worker_ = std::thread([this, state, context, request = std::move(request),
            done = std::move(done)]() mutable {
        auto result = create_project(request);
        if (!state->alive.load(std::memory_order_acquire)) return;
        QMetaObject::invokeMethod(context, [this, state, done = std::move(done),
                result = std::move(result)]() mutable {
            if (!state->alive.load(std::memory_order_acquire)) return;
            pending_ = false;
            if (done) done(std::move(result));
        }, Qt::QueuedConnection);
    });
}

} // namespace lingtai::desktop
