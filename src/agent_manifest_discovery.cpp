#include "agent_manifest_discovery.h"
#include "agent_manifest_discovery_test_seam.h"
#include "project_attachment.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace lingtai::desktop {
namespace {
namespace fs = std::filesystem;
using testing::AgentManifestDirectoryListing;
using testing::AgentManifestDiscoveryFilesystem;
using testing::AgentManifestFileRead;
using testing::AgentManifestFileReadKind;
using ListingKind = AgentManifestDirectoryListing::Kind;

// Discovery owns a 1 MiB source-content limit for each immediate manifest.
// The streaming read, not advisory file metadata, authoritatively enforces it.
constexpr std::size_t kMaximumAgentManifestBytes = 1024U * 1024U;

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

class DefaultFilesystem final : public AgentManifestDiscoveryFilesystem {
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
        if (too_large)
            return {AgentManifestFileReadKind::too_large, {}, {}};
        return {AgentManifestFileReadKind::read, std::move(bytes), {}};
    }

private:
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

AgentManifestDiagnosticKind parse_manifest(const std::string &bytes) {
    QJsonParseError error;
    const auto value = QJsonValue::fromJson(
        QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError) {
        return AgentManifestDiagnosticKind::invalid_json;
    }
    return value.isObject() ? AgentManifestDiagnosticKind::none
                            : AgentManifestDiagnosticKind::not_object;
}

void add_scan_diagnostic(AgentManifestDiscoveryReport &report,
        AgentManifestScanDiagnosticKind kind, const fs::path &path,
        std::error_code error = {}) {
    report.scan_diagnostics.push_back({kind, path, error});
}

void add_item(AgentManifestDiscoveryReport &report, const fs::path &key,
        const fs::path &directory, AgentManifestKind kind,
        AgentManifestObservationState observation,
        AgentManifestDiagnosticKind diagnostic, std::error_code error = {}) {
    report.items.push_back({key, directory, kind,
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
            const auto diagnostic = parse_manifest(manifest.bytes);
            add_item(report, name, directory,
                diagnostic == AgentManifestDiagnosticKind::none
                    ? AgentManifestKind::valid : AgentManifestKind::malformed,
                AgentManifestObservationState::read_this_scan, diagnostic);
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
} // namespace

AgentManifestDiscoveryReport discover_agent_manifests(
        const ProjectAttachment &attachment) noexcept {
    const DefaultFilesystem filesystem;
    return testing::discover_agent_manifests(attachment, filesystem);
}

namespace testing {
AgentManifestDiscoveryReport discover_agent_manifests(
        const ProjectAttachment &attachment,
        const AgentManifestDiscoveryFilesystem &filesystem) noexcept {
    return discover_impl(attachment, filesystem);
}
} // namespace testing
} // namespace lingtai::desktop
