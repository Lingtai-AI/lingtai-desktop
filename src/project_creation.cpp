#include "project_creation.h"

#include "posix_descriptor_primitives.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
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
        ProjectCreationFailure kind, std::string detail) {
    return {
        .created = false,
        .failure = kind,
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

bool runtime_python_available(const fs::path &path) {
    if (!path.is_absolute()) return false;
    std::error_code error;
    const auto resolved = fs::canonical(path, error);
    if (error || !fs::is_regular_file(resolved, error) || error) return false;
    return ::access(resolved.c_str(), X_OK) == 0;
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
        && make_directory(mailbox.get(), "archive", 0755);
}

std::atomic_uint64_t next_stage{0};

std::string stage_name() {
    return std::string(kStagingPrefix) + std::to_string(::getpid()) + "-"
        + std::to_string(next_stage.fetch_add(1, std::memory_order_relaxed));
}

void remove_owned_stage(const fs::path &destination,
        const std::string &stage, dev_t device, ino_t inode) {
    struct stat current {};
    if (::lstat((destination / stage).c_str(), &current) != 0
            || !S_ISDIR(current.st_mode) || current.st_dev != device
            || current.st_ino != inode) {
        return;
    }
    const auto marker = destination / stage / kStagingMarker;
    const auto marker_bytes = read_absolute_regular(marker, 512);
    if (!marker_bytes || *marker_bytes != stage + "\n") return;
    std::error_code ignored;
    fs::remove_all(destination / stage, ignored);
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

ProjectCreationResult create_project(
        const ProjectCreationRequest &request) noexcept {
    try {
        if (!plain_absolute_path(request.destination)) {
            return failure(ProjectCreationFailure::invalid_destination,
                "destination must be an existing absolute directory without traversal");
        }
        auto destination = open_absolute_directory(request.destination);
        if (destination.get() < 0) {
            return failure(ProjectCreationFailure::unsafe_path,
                "destination is unavailable or contains a symlink");
        }
        if (!valid_agent_leaf(request.agent_directory)
                || request.agent_name.empty()) {
            return failure(ProjectCreationFailure::invalid_agent_name,
                "Agent name and folder must be nonempty contained names");
        }
        struct stat existing {};
        if (::fstatat(destination.get(), ".lingtai", &existing,
                AT_SYMLINK_NOFOLLOW) == 0) {
            return failure(ProjectCreationFailure::existing_project,
                ".lingtai already exists; existing project state was preserved");
        }
        if (errno != ENOENT) {
            return failure(ProjectCreationFailure::unsafe_path,
                "the destination .lingtai leaf could not be inspected safely");
        }

        const auto selected_preset = load_preset(request.preset_path);
        if (!selected_preset) {
            return failure(ProjectCreationFailure::invalid_preset,
                "selected preset is unreadable, unsafe, oversized, or malformed");
        }
        auto requested_allowed = std::vector<std::string>();
        for (const auto &path : request.allowed_preset_paths) {
            if (!load_preset(path)) {
                return failure(ProjectCreationFailure::invalid_preset,
                    "an allowed preset is unreadable, unsafe, oversized, or malformed");
            }
            const auto text = path.string();
            if (std::ranges::find(requested_allowed, text)
                    == requested_allowed.end()) {
                requested_allowed.push_back(text);
            }
        }
        AgentSetupPresetSelection selection{
            .choice = AgentSetupPresetChoice::select_preset,
            .reference = request.preset_path.string(),
            .manifest = selected_preset->value("manifest").toObject(),
        };
        const auto policy = reconcile_agent_setup_presets(
            {}, selection, requested_allowed);
        if (policy.active.empty() || policy.default_ref.empty()
                || policy.allowed.empty()) {
            return failure(ProjectCreationFailure::invalid_preset,
                "selected preset could not form a valid setup policy");
        }

        if (!runtime_python_available(request.runtime_python)) {
            return failure(ProjectCreationFailure::runtime_unavailable,
                "the configured kernel runtime Python is unavailable");
        }
        if (!read_absolute_regular(request.env_file, kMaximumJsonBytes)) {
            return failure(ProjectCreationFailure::runtime_unavailable,
                "the configured runtime environment file is unavailable");
        }
        if (!request.covenant_file.empty()
                && !read_absolute_regular(
                    request.covenant_file, kMaximumJsonBytes)) {
            return failure(ProjectCreationFailure::runtime_unavailable,
                "the reviewed covenant file is unavailable");
        }

        const auto stage = stage_name();
        if (!make_directory(destination.get(), stage, 0700)) {
            return failure(ProjectCreationFailure::staging_failed,
                "could not create the owned project staging directory");
        }
        auto staging = posix::open_directory_component(destination.get(), stage);
        if (staging.get() < 0) {
            return failure(ProjectCreationFailure::staging_failed,
                "could not open the owned project staging directory");
        }
        struct stat staging_identity {};
        if (::fstat(staging.get(), &staging_identity) != 0
                || !S_ISDIR(staging_identity.st_mode)) {
            return failure(ProjectCreationFailure::staging_failed,
                "could not identify the owned project staging directory");
        }
        const auto cleanup = [&] {
            remove_owned_stage(request.destination, stage,
                staging_identity.st_dev, staging_identity.st_ino);
        };
        if (!write_new_file(staging.get(), kStagingMarker,
                stage + "\n", 0600)) {
            struct stat current {};
            if (::fstatat(destination.get(), stage.c_str(), &current,
                    AT_SYMLINK_NOFOLLOW) == 0
                    && S_ISDIR(current.st_mode)
                    && current.st_dev == staging_identity.st_dev
                    && current.st_ino == staging_identity.st_ino) {
                static_cast<void>(::unlinkat(
                    destination.get(), stage.c_str(), AT_REMOVEDIR));
            }
            return failure(ProjectCreationFailure::staging_failed,
                "could not mark the owned project staging directory");
        }
        if (request.failure_point == ProjectCreationFailurePoint::after_staging) {
            cleanup();
            return failure(ProjectCreationFailure::staging_failed,
                "injected failure after staging");
        }

        auto human = make_and_open_directory(staging.get(), "human");
        auto shared = make_and_open_directory(staging.get(), ".library_shared");
        auto agent = make_and_open_directory(
            staging.get(), fs::path(request.agent_directory));
        if (human.get() < 0 || shared.get() < 0 || agent.get() < 0
                || !make_mailbox(human.get()) || !make_mailbox(agent.get())) {
            cleanup();
            return failure(ProjectCreationFailure::staging_failed,
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
                || !write_new_file(human_mailbox.get(), "contacts.json", "[]")) {
            cleanup();
            return failure(ProjectCreationFailure::staging_failed,
                "could not build the staged human mailbox");
        }

        auto manifest = selected_preset->value("manifest").toObject();
        manifest["agent_name"] = QString::fromStdString(request.agent_name);
        manifest["language"] = QString::fromStdString(request.setup.language);
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
        if (!request.comment.empty()) {
            const auto final_comment = request.destination / ".lingtai"
                / request.agent_directory / "comment.md";
            init["comment_file"] = QString::fromStdString(final_comment.string());
        }
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
                || (!request.comment.empty()
                    && !write_new_file(agent.get(), "comment.md",
                        request.comment))) {
            cleanup();
            return failure(ProjectCreationFailure::staging_failed,
                "could not write the staged Agent configuration");
        }
        if (::unlinkat(staging.get(), kStagingMarker.data(), 0) != 0
                || ::fchmod(staging.get(), 0755) != 0
                || ::fsync(staging.get()) != 0) {
            cleanup();
            return failure(ProjectCreationFailure::staging_failed,
                "could not finalize the staged project");
        }
        if (request.failure_point == ProjectCreationFailurePoint::before_publish) {
            // Restore the marker solely so bounded cleanup can prove ownership.
            static_cast<void>(write_new_file(staging.get(), kStagingMarker,
                stage + "\n", 0600));
            cleanup();
            return failure(ProjectCreationFailure::publish_failed,
                "injected failure before publication");
        }
        if (!publish_no_replace(destination.get(), stage, ".lingtai")) {
            static_cast<void>(write_new_file(staging.get(), kStagingMarker,
                stage + "\n", 0600));
            cleanup();
            return failure(ProjectCreationFailure::publish_failed,
                "project publication was refused; destination state was preserved");
        }
        static_cast<void>(::fsync(destination.get()));
        return {
            .created = true,
            .project_dir = request.destination,
            .agent_key = fs::path(request.agent_directory),
            .failure = ProjectCreationFailure::none,
        };
    } catch (const std::exception &error) {
        return failure(ProjectCreationFailure::local_failure, error.what());
    } catch (...) {
        return failure(ProjectCreationFailure::local_failure,
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
}

bool ProjectCreationRunner::is_pending() const noexcept { return pending_; }

void ProjectCreationRunner::run_catalog(
        const QString &global_dir, PresetDone done) {
    if (pending_) return;
    pending_ = true;
    auto state = delivery_;
    auto context = QPointer<QObject>(&delivery_context_);
    std::thread([this, state, context, global_dir, done = std::move(done)]() mutable {
        auto result = load_preset_catalog(global_dir);
        if (!state->alive.load(std::memory_order_acquire) || !context) return;
        QMetaObject::invokeMethod(context, [this, state, done = std::move(done),
                result = std::move(result)]() mutable {
            if (!state->alive.load(std::memory_order_acquire)) return;
            pending_ = false;
            if (done) done(std::move(result));
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectCreationRunner::run_create(
        ProjectCreationRequest request, CreateDone done) {
    if (pending_) return;
    pending_ = true;
    auto state = delivery_;
    auto context = QPointer<QObject>(&delivery_context_);
    std::thread([this, state, context, request = std::move(request),
            done = std::move(done)]() mutable {
        auto result = create_project(request);
        if (!state->alive.load(std::memory_order_acquire) || !context) return;
        QMetaObject::invokeMethod(context, [this, state, done = std::move(done),
                result = std::move(result)]() mutable {
            if (!state->alive.load(std::memory_order_acquire)) return;
            pending_ = false;
            if (done) done(std::move(result));
        }, Qt::QueuedConnection);
    }).detach();
}

} // namespace lingtai::desktop
