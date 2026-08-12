#include "agent_manifest_discovery.h"
#include "agent_manifest_discovery_test_seam.h"
#include "agent_roster_presence.h"
#include "agent_roster_presence_test_seam.h"
#include "project_attachment.h"

#include <QtCore/QByteArrayView>
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
#include <map>
#include <locale>
#include <sstream>
#include <sys/stat.h>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace lingtai::desktop {
namespace {
namespace fs = std::filesystem;
using testing::AgentManifestDirectoryListing;
using testing::AgentManifestDiscoveryFilesystem;
using testing::AgentManifestFileRead;
using testing::AgentManifestFileReadKind;
using testing::AgentHeartbeatFileRead;
using testing::AgentHeartbeatFileReadKind;
using testing::AgentRosterPresenceClock;
using testing::AgentRosterPresenceFilesystem;
using ListingKind = AgentManifestDirectoryListing::Kind;

// Discovery owns a 1 MiB source-content limit for each immediate manifest.
// The streaming read, not advisory file metadata, authoritatively enforces it.
constexpr std::size_t kMaximumAgentManifestBytes = 1024U * 1024U;
// Heartbeats are one owned wall-clock number, not an arbitrary metadata file.
constexpr std::size_t kMaximumAgentHeartbeatBytes = 128U;

bool is_missing(const std::error_code &error) {
    return error == std::errc::no_such_file_or_directory; }
bool is_unavailable(const std::error_code &error) {
    return error == std::errc::permission_denied
        || error == std::errc::operation_not_permitted; }
std::error_code current_error() { return {errno, std::generic_category()}; }
std::error_code unknown_io_error() {
    return std::make_error_code(std::errc::io_error); }

class FileDescriptor final {
public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    ~FileDescriptor() { reset(); }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept
    : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor &operator=(FileDescriptor &&other) noexcept {
        if (this != &other) reset(std::exchange(other.value_, -1));
        return *this;
    }
    void reset(int value = -1) {
        if (value_ >= 0) ::close(value_);
        value_ = value;
    }
    [[nodiscard]] int get() const { return value_; }
private:
    int value_;
};

class DirectoryStream final {
public:
    explicit DirectoryStream(DIR *value) : value_(value) {}
    ~DirectoryStream() { if (value_) ::closedir(value_); }
    [[nodiscard]] DIR *get() const { return value_; }
private:
    DIR *value_;
};

int read_flags() {
    auto flags = O_RDONLY;
    flags |= O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    return flags;
}

ListingKind listing_error_kind(const std::error_code &error) {
    if (is_missing(error)) return ListingKind::missing;
    if (error == std::errc::not_a_directory) return ListingKind::not_directory;
    if (error == std::errc::too_many_symbolic_link_levels) {
        return ListingKind::unsafe_symlink;
    }
    return is_unavailable(error) ? ListingKind::unreadable : ListingKind::io_error;
}

AgentManifestFileRead candidate_error(std::error_code error) {
    const auto kind = is_missing(error)
        ? AgentManifestFileReadKind::candidate_absent
        : error == std::errc::not_a_directory
            ? AgentManifestFileReadKind::candidate_not_directory
        : error == std::errc::too_many_symbolic_link_levels
            ? AgentManifestFileReadKind::candidate_unsafe_symlink
        : is_unavailable(error)
            ? AgentManifestFileReadKind::candidate_unreadable
            : AgentManifestFileReadKind::candidate_io_error;
    return {kind, {}, error};
}

AgentManifestFileRead manifest_error(std::error_code error) {
    const auto kind = is_missing(error) || error == std::errc::not_a_directory
        ? AgentManifestFileReadKind::manifest_absent
        : error == std::errc::too_many_symbolic_link_levels
            ? AgentManifestFileReadKind::unsafe_symlink
        : is_unavailable(error) ? AgentManifestFileReadKind::unreadable
                                : AgentManifestFileReadKind::io_error;
    return {kind, {}, error};
}

struct FileIdentity {
    dev_t device = 0;
    ino_t inode = 0;
};

bool same_file(const struct stat &value, const FileIdentity &identity) {
    return value.st_dev == identity.device && value.st_ino == identity.inode;
}

class DefaultFilesystem final : public AgentRosterPresenceFilesystem {
public:
    AgentManifestDirectoryListing list_directory(
            const fs::path &path) const override {
        AgentManifestDirectoryListing result;
        struct stat observed_root {};
        if (::fstatat(AT_FDCWD, path.c_str(), &observed_root,
                AT_SYMLINK_NOFOLLOW) != 0) {
            result.error = current_error();
            result.kind = listing_error_kind(result.error);
            return result;
        }
        if (S_ISLNK(observed_root.st_mode))
            return result.kind = ListingKind::unsafe_symlink, result;
        if (!S_ISDIR(observed_root.st_mode))
            return result.kind = ListingKind::not_directory, result;
        root_.reset(::open(path.c_str(), read_flags() | O_DIRECTORY));
        if (root_.get() < 0) {
            result.error = current_error();
            struct stat current_root {};
            if (::fstatat(AT_FDCWD, path.c_str(), &current_root,
                    AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(current_root.st_mode)) {
                result.error.clear();
                result.kind = ListingKind::unsafe_symlink;
                return result;
            }
            result.kind = listing_error_kind(result.error);
            return result;
        }
        root_path_ = path;
        if (::fstat(root_.get(), &opened_root_) != 0) {
            result.error = current_error();
            root_.reset();
            return result;
        }
        const auto duplicate = ::dup(root_.get());
        if (duplicate < 0) {
            result.error = current_error();
            return result;
        }
        DirectoryStream stream(::fdopendir(duplicate));
        if (!stream.get()) {
            ::close(duplicate);
            result.error = current_error();
            return result;
        }
        for (;;) {
            errno = 0;
            const auto entry = ::readdir(stream.get());
            if (!entry) {
                if (errno != 0) result.error = current_error();
                break;
            }
            const auto name = fs::path(entry->d_name);
            if (name != "." && name != "..") result.entries.push_back(name);
        }
        if (result.error) {
            result.entries.clear();
            result.kind = listing_error_kind(result.error);
        } else {
            result.kind = current_root_kind(result.error);
        }
        return result;
    }

    AgentManifestFileRead read_manifest(
            const fs::path &root,
            const fs::path &directory_key) const override {
        if (root_.get() < 0 || root != root_path_)
            return candidate_error(unknown_io_error());
        struct stat observed_directory {};
        if (::fstatat(root_.get(), directory_key.c_str(), &observed_directory,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return candidate_error(current_error());
        }
        if (S_ISLNK(observed_directory.st_mode))
            return {AgentManifestFileReadKind::candidate_unsafe_symlink, {}, {}};
        if (!S_ISDIR(observed_directory.st_mode))
            return {AgentManifestFileReadKind::candidate_not_directory, {}, {}};
        const FileDescriptor directory(::openat(
            root_.get(), directory_key.c_str(), read_flags() | O_DIRECTORY));
        if (directory.get() < 0) {
            const auto error = current_error();
            struct stat current_directory {};
            if (::fstatat(root_.get(), directory_key.c_str(), &current_directory,
                    AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(current_directory.st_mode)) {
                return {AgentManifestFileReadKind::candidate_unsafe_symlink, {}, {}};
            }
            return candidate_error(error);
        }
        struct stat opened_directory {};
        if (::fstat(directory.get(), &opened_directory) != 0)
            return candidate_error(current_error());

        constexpr auto manifest_name = ".agent.json";
        struct stat observed_manifest {};
        if (::fstatat(directory.get(), manifest_name, &observed_manifest,
                AT_SYMLINK_NOFOLLOW) != 0) {
            const auto error = current_error();
            if (is_missing(error) || error == std::errc::not_a_directory)
                return {AgentManifestFileReadKind::manifest_absent, {}, error};
            return candidate_error(error);
        }
        if (S_ISLNK(observed_manifest.st_mode))
            return {AgentManifestFileReadKind::unsafe_symlink, {}, {}};
        if (!S_ISREG(observed_manifest.st_mode))
            return {AgentManifestFileReadKind::not_regular, {}, {}};
        const FileDescriptor manifest(::openat(
            directory.get(), manifest_name, read_flags()));
        if (manifest.get() < 0) {
            const auto error = current_error();
            struct stat current_manifest {};
            if (::fstatat(directory.get(), manifest_name, &current_manifest,
                    AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(current_manifest.st_mode)) {
                return {AgentManifestFileReadKind::unsafe_symlink, {}, {}};
            }
            return manifest_error(error);
        }
        struct stat opened_manifest {};
        if (::fstat(manifest.get(), &opened_manifest) != 0)
            return manifest_error(current_error());
        if (!S_ISREG(opened_manifest.st_mode))
            return {AgentManifestFileReadKind::not_regular, {}, {}};

        std::string bytes;
        std::array<char, 8192> buffer{};
        auto too_large = false;
        for (;;) {
            const auto remaining = kMaximumAgentManifestBytes - bytes.size();
            const auto requested = remaining < buffer.size()
                ? remaining + 1U : buffer.size();
            const auto count = ::read(manifest.get(), buffer.data(), requested);
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                return manifest_error(current_error());
            }
            const auto received = static_cast<std::size_t>(count);
            if (received > remaining) {
                too_large = true;
                break;
            }
            bytes.append(buffer.data(), received);
        }

        struct stat current_directory {};
        if (::fstatat(root_.get(), directory_key.c_str(), &current_directory,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return candidate_error(current_error());
        }
        if (S_ISLNK(current_directory.st_mode))
            return {AgentManifestFileReadKind::candidate_unsafe_symlink, {}, {}};
        if (!S_ISDIR(current_directory.st_mode))
            return {AgentManifestFileReadKind::candidate_not_directory, {}, {}};
        if (opened_directory.st_dev != current_directory.st_dev
            || opened_directory.st_ino != current_directory.st_ino) {
            return {AgentManifestFileReadKind::candidate_absent, {}, {}};
        }
        candidate_identities_.insert_or_assign(directory_key,
            FileIdentity{opened_directory.st_dev, opened_directory.st_ino});
        if (too_large)
            return {AgentManifestFileReadKind::too_large, {}, {}};
        return {AgentManifestFileReadKind::read, std::move(bytes), {}};
    }

    AgentHeartbeatFileRead read_heartbeat(
            const fs::path &root,
            const fs::path &directory_key) const override {
        if (root_.get() < 0 || root != root_path_) {
            return {AgentHeartbeatFileReadKind::container_unavailable, {},
                unknown_io_error()};
        }
        if (const auto failure = current_root_failure()) return *failure;
        const auto expected = candidate_identities_.find(directory_key);
        if (expected == candidate_identities_.end()) {
            return {AgentHeartbeatFileReadKind::container_unavailable, {},
                unknown_io_error()};
        }

        struct stat observed_directory {};
        if (::fstatat(root_.get(), directory_key.c_str(), &observed_directory,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return container_error(current_error());
        }
        if (S_ISLNK(observed_directory.st_mode)) {
            return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, {}};
        }
        if (!S_ISDIR(observed_directory.st_mode)
            || !same_file(observed_directory, expected->second)) {
            return {AgentHeartbeatFileReadKind::container_changed, {}, {}};
        }
        const FileDescriptor directory(::openat(
            root_.get(), directory_key.c_str(), read_flags() | O_DIRECTORY));
        if (directory.get() < 0) {
            const auto error = current_error();
            struct stat current_directory {};
            if (::fstatat(root_.get(), directory_key.c_str(), &current_directory,
                    AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(current_directory.st_mode)) {
                return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, {}};
            }
            return container_error(error);
        }
        struct stat opened_directory {};
        if (::fstat(directory.get(), &opened_directory) != 0) {
            return container_error(current_error());
        }
        if (!S_ISDIR(opened_directory.st_mode)
            || !same_file(opened_directory, expected->second)) {
            return {AgentHeartbeatFileReadKind::container_changed, {}, {}};
        }

        constexpr auto heartbeat_name = ".agent.heartbeat";
        struct stat observed_heartbeat {};
        if (::fstatat(directory.get(), heartbeat_name, &observed_heartbeat,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return heartbeat_error(current_error());
        }
        if (S_ISLNK(observed_heartbeat.st_mode)) {
            return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, {}};
        }
        if (!S_ISREG(observed_heartbeat.st_mode)) {
            return {AgentHeartbeatFileReadKind::not_regular, {}, {}};
        }
        const FileIdentity heartbeat_identity{
            observed_heartbeat.st_dev, observed_heartbeat.st_ino};
        const FileDescriptor heartbeat(::openat(
            directory.get(), heartbeat_name, read_flags()));
        if (heartbeat.get() < 0) {
            const auto error = current_error();
            struct stat current_heartbeat {};
            if (::fstatat(directory.get(), heartbeat_name, &current_heartbeat,
                    AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(current_heartbeat.st_mode)) {
                return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, {}};
            }
            return heartbeat_error(error);
        }
        struct stat opened_heartbeat {};
        if (::fstat(heartbeat.get(), &opened_heartbeat) != 0) {
            return heartbeat_error(current_error());
        }
        if (!S_ISREG(opened_heartbeat.st_mode)) {
            return {AgentHeartbeatFileReadKind::not_regular, {}, {}};
        }
        if (!same_file(opened_heartbeat, heartbeat_identity)) {
            return {AgentHeartbeatFileReadKind::source_changed, {}, {}};
        }

        std::string bytes;
        std::array<char, kMaximumAgentHeartbeatBytes + 1U> buffer{};
        for (;;) {
            const auto remaining = kMaximumAgentHeartbeatBytes - bytes.size();
            const auto requested = remaining < buffer.size()
                ? remaining + 1U : buffer.size();
            const auto count = ::read(heartbeat.get(), buffer.data(), requested);
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                return heartbeat_error(current_error());
            }
            const auto received = static_cast<std::size_t>(count);
            if (received > remaining) {
                return {AgentHeartbeatFileReadKind::too_large, {}, {}};
            }
            bytes.append(buffer.data(), received);
        }

        if (const auto failure = current_root_failure()) return *failure;
        struct stat current_directory {};
        if (::fstatat(root_.get(), directory_key.c_str(), &current_directory,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return container_error(current_error());
        }
        if (S_ISLNK(current_directory.st_mode)) {
            return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, {}};
        }
        if (!S_ISDIR(current_directory.st_mode)
            || !same_file(current_directory, expected->second)) {
            return {AgentHeartbeatFileReadKind::container_changed, {}, {}};
        }
        struct stat current_heartbeat {};
        if (::fstatat(directory.get(), heartbeat_name, &current_heartbeat,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return heartbeat_error(current_error());
        }
        if (S_ISLNK(current_heartbeat.st_mode)) {
            return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, {}};
        }
        if (!S_ISREG(current_heartbeat.st_mode)
            || !same_file(current_heartbeat, heartbeat_identity)) {
            return {AgentHeartbeatFileReadKind::source_changed, {}, {}};
        }
        return {AgentHeartbeatFileReadKind::read, std::move(bytes), {}};
    }

private:
    static AgentHeartbeatFileRead container_error(std::error_code error) {
        if (error == std::errc::too_many_symbolic_link_levels) {
            return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, error};
        }
        return {AgentHeartbeatFileReadKind::container_unavailable, {}, error};
    }

    static AgentHeartbeatFileRead heartbeat_error(std::error_code error) {
        if (is_missing(error) || error == std::errc::not_a_directory) {
            return {AgentHeartbeatFileReadKind::absent, {}, error};
        }
        if (error == std::errc::too_many_symbolic_link_levels) {
            return {AgentHeartbeatFileReadKind::unsafe_symlink, {}, error};
        }
        return {is_unavailable(error)
                ? AgentHeartbeatFileReadKind::unreadable
                : AgentHeartbeatFileReadKind::io_error,
            {}, error};
    }

    std::optional<AgentHeartbeatFileRead> current_root_failure() const {
        struct stat current_root {};
        if (::fstatat(AT_FDCWD, root_path_.c_str(), &current_root,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return container_error(current_error());
        }
        if (S_ISLNK(current_root.st_mode)) {
            return AgentHeartbeatFileRead{
                AgentHeartbeatFileReadKind::unsafe_symlink, {}, {}};
        }
        if (!S_ISDIR(current_root.st_mode)
            || current_root.st_dev != opened_root_.st_dev
            || current_root.st_ino != opened_root_.st_ino) {
            return AgentHeartbeatFileRead{
                AgentHeartbeatFileReadKind::container_changed, {}, {}};
        }
        return std::nullopt;
    }

    ListingKind current_root_kind(std::error_code &error) const {
        struct stat current_root {};
        if (::fstatat(AT_FDCWD, root_path_.c_str(), &current_root,
                AT_SYMLINK_NOFOLLOW) != 0) {
            error = current_error();
            return listing_error_kind(error);
        }
        if (S_ISLNK(current_root.st_mode)) return ListingKind::unsafe_symlink;
        if (!S_ISDIR(current_root.st_mode)) return ListingKind::not_directory;
        if (opened_root_.st_dev != current_root.st_dev
            || opened_root_.st_ino != current_root.st_ino) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
            return ListingKind::missing;
        }
        return ListingKind::complete;
    }

    mutable FileDescriptor root_;
    mutable fs::path root_path_;
    mutable struct stat opened_root_ {};
    mutable std::map<fs::path, FileIdentity> candidate_identities_;
};

AgentManifestScanState scan_state(ListingKind kind) {
    switch (kind) {
    case ListingKind::complete: return AgentManifestScanState::complete;
    case ListingKind::missing: return AgentManifestScanState::root_missing;
    case ListingKind::not_directory:
        return AgentManifestScanState::root_not_directory;
    case ListingKind::unsafe_symlink:
        return AgentManifestScanState::root_unsafe_symlink;
    case ListingKind::unreadable: return AgentManifestScanState::root_unreadable;
    case ListingKind::io_error: return AgentManifestScanState::root_io_error;
    }
    return AgentManifestScanState::root_io_error;
}

bool safe_leaf(const fs::path &path) {
    return !path.empty() && !path.is_absolute() && !path.has_root_name()
        && !path.has_root_directory() && path == path.filename()
        && path != "." && path != ".."
        && path.native().find('\0') == std::string::npos;
}

struct ParsedManifest {
    AgentManifestDiagnosticKind diagnostic =
        AgentManifestDiagnosticKind::invalid_json;
    AgentRole role = AgentRole::unknown;
};

ParsedManifest parse_manifest(const std::string &bytes) {
    QJsonParseError error;
    const auto value = QJsonValue::fromJson(
        QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError) {
        return {AgentManifestDiagnosticKind::invalid_json, AgentRole::unknown};
    }
    if (!value.isObject()) {
        return {AgentManifestDiagnosticKind::not_object, AgentRole::unknown};
    }
    const auto object = value.toObject();
    constexpr auto admin_key = "admin";
    if (!object.contains(admin_key) || object.value(admin_key).isNull()) {
        return {AgentManifestDiagnosticKind::none, AgentRole::human};
    }
    const auto admin = object.value(admin_key);
    if (admin.isObject()) {
        for (const auto &permission : admin.toObject()) {
            if (permission.isBool() && permission.toBool()) {
                return {AgentManifestDiagnosticKind::none, AgentRole::main};
            }
        }
    }
    return {AgentManifestDiagnosticKind::none, AgentRole::agent};
}

void add_scan_diagnostic(AgentManifestDiscoveryReport &report,
        AgentManifestScanDiagnosticKind kind, const fs::path &path,
        std::error_code error = {}) {
    report.scan_diagnostics.push_back({kind, path, error});
}

void add_item(AgentManifestDiscoveryReport &report, const fs::path &key,
        const fs::path &directory, AgentManifestKind kind,
        AgentManifestObservationState observation,
        AgentManifestDiagnosticKind diagnostic, std::error_code error = {},
        AgentRole role = AgentRole::unknown) {
    report.items.push_back({key, directory, kind, role,
        {directory / ".agent.json", observation, diagnostic, error}});
}

AgentManifestDiscoveryReport failed_report(
    const ProjectAttachment &attachment) noexcept;

AgentManifestDiscoveryReport discover_impl(const ProjectAttachment &attachment,
        const AgentManifestDiscoveryFilesystem &filesystem) noexcept {
    AgentManifestDiscoveryReport report;
    AgentManifestDirectoryListing listing;
    try {
        report.scan.path = attachment.root() / ".lingtai";
        listing = filesystem.list_directory(report.scan.path);
        report.scan.state = scan_state(listing.kind);
        report.scan.system_error = listing.error;
    } catch (...) {
        return failed_report(attachment);
    }
    if (listing.kind != ListingKind::complete) return report;

    try {
        std::ranges::sort(listing.entries);
        listing.entries.erase(
            std::ranges::unique(listing.entries).begin(), listing.entries.end());
    } catch (...) {
        return report;
    }
    // Each try-block contains one algorithm-selected, safe-leaf candidate.
    for (const auto &name : listing.entries) try {
        if (!safe_leaf(name)) continue;
        const auto directory = report.scan.path / name;
        const auto manifest = filesystem.read_manifest(report.scan.path, name);
        switch (manifest.kind) {
        case AgentManifestFileReadKind::candidate_absent:
        case AgentManifestFileReadKind::candidate_not_directory:
        case AgentManifestFileReadKind::manifest_absent:
            break;
        case AgentManifestFileReadKind::candidate_unsafe_symlink:
        case AgentManifestFileReadKind::candidate_unreadable:
        case AgentManifestFileReadKind::candidate_io_error:
            add_scan_diagnostic(report,
                manifest.kind == AgentManifestFileReadKind::candidate_unsafe_symlink
                    ? AgentManifestScanDiagnosticKind::child_unsafe_symlink
                : manifest.kind == AgentManifestFileReadKind::candidate_unreadable
                    ? AgentManifestScanDiagnosticKind::child_unreadable
                    : AgentManifestScanDiagnosticKind::child_io_error,
                directory, manifest.error);
            break;
        case AgentManifestFileReadKind::unsafe_symlink:
            add_item(report, name, directory, AgentManifestKind::unsafe,
                AgentManifestObservationState::rejected_unsafe,
                AgentManifestDiagnosticKind::unsafe_symlink, manifest.error);
            break;
        case AgentManifestFileReadKind::not_regular:
        case AgentManifestFileReadKind::unreadable:
        case AgentManifestFileReadKind::io_error:
        case AgentManifestFileReadKind::too_large:
            add_item(report, name, directory, AgentManifestKind::malformed,
                AgentManifestObservationState::observed_unavailable,
                manifest.kind == AgentManifestFileReadKind::not_regular
                    ? AgentManifestDiagnosticKind::not_regular
                : manifest.kind == AgentManifestFileReadKind::unreadable
                    ? AgentManifestDiagnosticKind::unreadable
                : manifest.kind == AgentManifestFileReadKind::too_large
                    ? AgentManifestDiagnosticKind::too_large
                    : AgentManifestDiagnosticKind::io_error,
                manifest.error);
            break;
        case AgentManifestFileReadKind::read: {
            const auto parsed = parse_manifest(manifest.bytes);
            add_item(report, name, directory,
                parsed.diagnostic == AgentManifestDiagnosticKind::none
                    ? AgentManifestKind::valid : AgentManifestKind::malformed,
                AgentManifestObservationState::read_this_scan,
                parsed.diagnostic, {}, parsed.role);
            break;
        }
        }
    } catch (...) {
        try {
            add_item(report, name, report.scan.path / name,
                AgentManifestKind::malformed,
                AgentManifestObservationState::observed_unavailable,
                AgentManifestDiagnosticKind::io_error, unknown_io_error());
        } catch (...) {
            // With no memory to represent this item, retain the honest
            // completed scan and all output already collected.
        }
    }
    return report;
}

AgentManifestDiscoveryReport failed_report(
        const ProjectAttachment &attachment) noexcept {
    AgentManifestDiscoveryReport report;
    report.scan.state = AgentManifestScanState::root_io_error;
    report.scan.system_error = unknown_io_error();
    try { report.scan.path = attachment.root() / ".lingtai"; } catch (...) {}
    return report;
}

bool ascii_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

std::string_view trim_ascii(const std::string &bytes) {
    auto first = std::size_t{0};
    while (first < bytes.size() && ascii_whitespace(bytes[first])) ++first;
    auto last = bytes.size();
    while (last > first && ascii_whitespace(bytes[last - 1U])) --last;
    return std::string_view(bytes).substr(first, last - first);
}

enum class DecimalParseState { valid, invalid, nonfinite };
struct ParsedDecimal { DecimalParseState state; double value = 0.0; };

bool ascii_digit(char value) { return value >= '0' && value <= '9'; }

ParsedDecimal parse_decimal(std::string_view token) {
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

AgentRosterPresenceItem unavailable_presence(const fs::path &path,
        AgentHeartbeatObservationState observation,
        AgentHeartbeatDiagnosticKind diagnostic,
        std::error_code error = {}) {
    return {AgentPresenceKind::unavailable, std::nullopt, std::nullopt,
        {path, observation, diagnostic, error}};
}

AgentRosterPresenceItem classify_heartbeat(const AgentHeartbeatFileRead &read,
        double now, const fs::path &path) {
    switch (read.kind) {
    case AgentHeartbeatFileReadKind::absent:
        return {AgentPresenceKind::missing, std::nullopt, std::nullopt,
            {path, AgentHeartbeatObservationState::absent,
                AgentHeartbeatDiagnosticKind::none, read.error}};
    case AgentHeartbeatFileReadKind::unsafe_symlink:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::rejected_unsafe,
            AgentHeartbeatDiagnosticKind::unsafe_symlink, read.error);
    case AgentHeartbeatFileReadKind::not_regular:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::observed_unavailable,
            AgentHeartbeatDiagnosticKind::not_regular, read.error);
    case AgentHeartbeatFileReadKind::unreadable:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::observed_unavailable,
            AgentHeartbeatDiagnosticKind::unreadable, read.error);
    case AgentHeartbeatFileReadKind::io_error:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::observed_unavailable,
            AgentHeartbeatDiagnosticKind::io_error, read.error);
    case AgentHeartbeatFileReadKind::too_large:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::observed_unavailable,
            AgentHeartbeatDiagnosticKind::too_large, read.error);
    case AgentHeartbeatFileReadKind::container_unavailable:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::observed_unavailable,
            AgentHeartbeatDiagnosticKind::container_unavailable, read.error);
    case AgentHeartbeatFileReadKind::container_changed:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::observed_unavailable,
            AgentHeartbeatDiagnosticKind::container_changed, read.error);
    case AgentHeartbeatFileReadKind::source_changed:
        return unavailable_presence(path,
            AgentHeartbeatObservationState::observed_unavailable,
            AgentHeartbeatDiagnosticKind::source_changed, read.error);
    case AgentHeartbeatFileReadKind::read:
        break;
    }

    const auto token = trim_ascii(read.bytes);
    const auto parsed = parse_decimal(token);
    if (parsed.state == DecimalParseState::invalid) {
        return {AgentPresenceKind::invalid, std::nullopt, std::nullopt,
            {path, AgentHeartbeatObservationState::read_this_scan,
                AgentHeartbeatDiagnosticKind::invalid_number, {}}};
    }
    if (parsed.state == DecimalParseState::nonfinite || !std::isfinite(now)) {
        return {AgentPresenceKind::invalid, std::nullopt, std::nullopt,
            {path, AgentHeartbeatObservationState::read_this_scan,
                AgentHeartbeatDiagnosticKind::nonfinite, {}}};
    }
    const auto timestamp = parsed.value;
    if (timestamp > now) {
        return {AgentPresenceKind::invalid, timestamp, 0.0,
            {path, AgentHeartbeatObservationState::read_this_scan,
                AgentHeartbeatDiagnosticKind::future, {}}};
    }
    const auto age = now - timestamp;
    const auto display_age = std::isfinite(age)
        ? std::optional<double>(age == 0.0 ? 0.0 : age) : std::nullopt;
    return {age < 5.0 ? AgentPresenceKind::alive : AgentPresenceKind::stale,
        timestamp, display_age,
        {path, AgentHeartbeatObservationState::read_this_scan,
            AgentHeartbeatDiagnosticKind::none, {}}};
}

AgentRosterPresenceItem no_heartbeat(const AgentManifestDiscoveryItem &item) {
    return {item.role == AgentRole::human
            ? AgentPresenceKind::alive_human : AgentPresenceKind::unknown,
        std::nullopt, std::nullopt,
        {item.directory_path / ".agent.heartbeat",
            AgentHeartbeatObservationState::not_applicable,
            AgentHeartbeatDiagnosticKind::none, {}}};
}

AgentRosterSnapshot roster_impl(const ProjectAttachment &attachment,
        const AgentRosterPresenceFilesystem &filesystem,
        const AgentRosterPresenceClock &clock) noexcept {
    auto discovery = testing::discover_agent_manifests(attachment, filesystem);
    static_assert(std::is_nothrow_move_assignable_v<
        AgentManifestDiscoveryReport>);
    AgentRosterSnapshot result;
    result.discovery = std::move(discovery);

    double now = 0.0;
    try {
        now = clock.wall_now_seconds();
        result.presence_by_item.reserve(result.discovery.items.size());
    } catch (...) {
        return result;
    }
    for (const auto &item : result.discovery.items) {
        try {
            if (item.role == AgentRole::human
                || item.role == AgentRole::unknown) {
                result.presence_by_item.push_back(no_heartbeat(item));
                continue;
            }
            const auto path = item.directory_path / ".agent.heartbeat";
            result.presence_by_item.push_back(classify_heartbeat(
                filesystem.read_heartbeat(
                    result.discovery.scan.path, item.directory_key),
                now, path));
        } catch (...) {
            try {
                result.presence_by_item.push_back(unavailable_presence(
                    item.directory_path / ".agent.heartbeat",
                    AgentHeartbeatObservationState::observed_unavailable,
                    AgentHeartbeatDiagnosticKind::io_error,
                    unknown_io_error()));
            } catch (...) {
                return result;
            }
        }
    }
    result.projection_complete = true;
    return result;
}

class SystemClock final : public AgentRosterPresenceClock {
public:
    double wall_now_seconds() const override {
        return std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};
} // namespace

AgentManifestDiscoveryReport discover_agent_manifests(
        const ProjectAttachment &attachment) noexcept {
    const DefaultFilesystem filesystem;
    return testing::discover_agent_manifests(attachment, filesystem);
}

AgentRosterSnapshot project_agent_roster(
        const ProjectAttachment &attachment) noexcept {
    const DefaultFilesystem filesystem;
    const SystemClock clock;
    return roster_impl(attachment, filesystem, clock);
}

namespace testing {
AgentManifestDiscoveryReport discover_agent_manifests(
        const ProjectAttachment &attachment,
        const AgentManifestDiscoveryFilesystem &filesystem) noexcept {
    return discover_impl(attachment, filesystem);
}

AgentRosterSnapshot project_agent_roster(
        const ProjectAttachment &attachment,
        const AgentRosterPresenceClock &clock) noexcept {
    const DefaultFilesystem filesystem;
    return roster_impl(attachment, filesystem, clock);
}

AgentRosterSnapshot project_agent_roster(
        const ProjectAttachment &attachment,
        const AgentRosterPresenceFilesystem &filesystem,
        const AgentRosterPresenceClock &clock) noexcept {
    return roster_impl(attachment, filesystem, clock);
}
} // namespace testing
} // namespace lingtai::desktop
