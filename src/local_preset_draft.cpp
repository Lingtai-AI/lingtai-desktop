#include "local_preset_draft.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QString>

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace posix = posix_internal;

// A generous bound, not the 64 KiB document bound itself: worst-case JSON
// string escaping (control/astral characters as `\uXXXX`) can expand a
// maximum-size document well past its own encoded byte count, and the
// envelope adds a small fixed amount of its own fields.
constexpr std::size_t kMaxEnvelopeBytes = 512U * 1024U;

// The one app-owned subdirectory beneath the injected application-data
// root; created (owner-restrictive) only when a save actually persists a
// first draft, never on a mere load.
constexpr auto kDraftsDirectoryName = "local-preset-drafts";

QByteArray path_bytes(const std::filesystem::path &path) {
    const auto encoded = path.u8string();
    return QByteArray(
        reinterpret_cast<const char *>(encoded.data()),
        static_cast<qsizetype>(encoded.size()));
}

// A deterministic digest filename: only a lookup convenience, never the
// identity check itself -- the envelope's own recorded fields are always
// re-verified against the exact requested key on load.
std::string digest_file_name(
        const std::filesystem::path &canonical_project_root,
        const std::string &agent_id) {
    auto input = path_bytes(canonical_project_root);
    input.append('\n');
    input.append(agent_id.data(), static_cast<qsizetype>(agent_id.size()));
    const auto digest =
        QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex();
    return std::string(digest.constData(), static_cast<std::size_t>(digest.size()))
        + ".json";
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

// Write-temp-then-rename inside the already-open drafts directory,
// mirroring the mail publisher's own atomic-replace shape. The temp leaf is
// removed on any failure; pre-existing saved content at the real name is
// never touched until the rename itself succeeds.
[[nodiscard]] bool write_envelope_atomic(
        int drafts_fd, const std::string &leaf, const std::string &bytes) {
    const auto temp_leaf = leaf + ".tmp";
    const auto raw_fd = ::openat(drafts_fd, temp_leaf.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (raw_fd < 0) return false;
    auto ok = true;
    std::size_t total = 0;
    while (ok && total < bytes.size()) {
        const auto count = ::write(raw_fd, bytes.data() + total,
            bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        total += static_cast<std::size_t>(count);
    }
    // Explicit close-and-check before the rename: a write that looked
    // successful can still fail to land at close.
    if (::close(raw_fd) != 0) ok = false;
    if (!ok) {
        ::unlinkat(drafts_fd, temp_leaf.c_str(), 0);
        return false;
    }
    if (::renameat(drafts_fd, temp_leaf.c_str(), drafts_fd, leaf.c_str()) != 0) {
        ::unlinkat(drafts_fd, temp_leaf.c_str(), 0);
        return false;
    }
    return true;
}

} // namespace

LocalPresetDraftLoad load_local_preset_draft(
        const std::filesystem::path &app_data_root,
        const std::filesystem::path &canonical_project_root,
        const std::string &agent_id) noexcept {
    try {
        if (agent_id.empty()) return {};
        const auto root = posix::open_root_directory(app_data_root);
        if (root.get() < 0) return {};
        const auto drafts = posix::open_directory_component(
            root.get(), kDraftsDirectoryName, /*create=*/false);
        if (drafts.get() < 0) return {}; // never saved yet: absent, not an error

        const auto leaf = digest_file_name(canonical_project_root, agent_id);
        const auto file = posix::open_regular_file_component(drafts.get(), leaf);
        if (file.get() < 0) return {};

        std::string bytes;
        if (!read_bounded_complete(file.get(), kMaxEnvelopeBytes, bytes)) {
            return {};
        }

        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(
            QByteArray(bytes.data(), static_cast<int>(bytes.size())), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return {};
        }
        const auto root_object = document.object();
        const auto schema = root_object.value("schema");
        if (!schema.isString()
            || schema.toString() != QStringLiteral("lingtai.desktop.preset-draft/v1")) {
            return {};
        }
        const auto project_value = root_object.value("project");
        if (!project_value.isString()
            || project_value.toString().toUtf8() != path_bytes(canonical_project_root)) {
            return {};
        }
        const auto agent_id_value = root_object.value("agent_id");
        if (!agent_id_value.isString()
            || agent_id_value.toString().toStdString() != agent_id) {
            return {};
        }
        const auto document_value = root_object.value("document");
        if (!document_value.isString()) return {};

        LocalPresetDraftLoad result;
        result.found = true;
        result.document = document_value.toString().toStdString();
        return result;
    } catch (...) {
        // Only string/JSON allocation can throw here. Treat as not found
        // rather than propagating -- a load never has a write side effect
        // to unwind.
        return {};
    }
}

LocalPresetDraftSaveResult save_local_preset_draft(
        const std::filesystem::path &app_data_root,
        const std::filesystem::path &canonical_project_root,
        const std::string &agent_id,
        const std::string &document) noexcept {
    try {
        if (agent_id.empty()) return LocalPresetDraftSaveResult::failed;
        if (document.size() > kLocalPresetDraftMaxBytes) {
            return LocalPresetDraftSaveResult::failed;
        }

        QJsonObject envelope;
        envelope["schema"] = QStringLiteral("lingtai.desktop.preset-draft/v1");
        envelope["project"] = QString::fromUtf8(path_bytes(canonical_project_root));
        envelope["agent_id"] = QString::fromStdString(agent_id);
        envelope["document"] = QString::fromStdString(document);
        const auto bytes = QJsonDocument(envelope)
            .toJson(QJsonDocument::Compact).toStdString();

        const auto root = posix::open_root_directory(app_data_root);
        if (root.get() < 0) return LocalPresetDraftSaveResult::failed;
        const auto drafts = posix::open_directory_component(
            root.get(), kDraftsDirectoryName, /*create=*/true);
        if (drafts.get() < 0) return LocalPresetDraftSaveResult::failed;

        const auto leaf = digest_file_name(canonical_project_root, agent_id);
        return write_envelope_atomic(drafts.get(), leaf, bytes)
            ? LocalPresetDraftSaveResult::saved
            : LocalPresetDraftSaveResult::failed;
    } catch (...) {
        // Only string/JSON allocation can throw here. Fail closed rather
        // than leaving an ambiguous partial write.
        return LocalPresetDraftSaveResult::failed;
    }
}

} // namespace lingtai::desktop
