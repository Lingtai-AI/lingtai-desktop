#include "tui_executable_resolver.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaximumReceiptBytes = 64U * 1024U;
constexpr std::string_view kExecutableName = "lingtai-tui";
constexpr std::string_view kReceiptSchema = "lingtai.tui.install/v1";

[[nodiscard]] bool valid_executable(const fs::path &candidate) {
    std::error_code error;
    const auto status = fs::status(candidate, error);
    return !error
        && fs::is_regular_file(status)
        && ::access(candidate.c_str(), X_OK) == 0;
}

[[nodiscard]] std::optional<std::string> read_receipt(
        const fs::path &receipt) {
    std::error_code error;
    const auto status = fs::symlink_status(receipt, error);
    if (error || !fs::is_regular_file(status)) return std::nullopt;
    const auto size = fs::file_size(receipt, error);
    if (error || size > kMaximumReceiptBytes) return std::nullopt;

    std::ifstream stream(receipt, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string bytes{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    if (stream.bad() || bytes.size() > kMaximumReceiptBytes) {
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] fs::path managed_candidate(const fs::path &home) {
    if (home.empty()) return {};
    const auto bytes = read_receipt(
        home / ".lingtai-tui" / "install.json");
    if (!bytes) return {};

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes->data(), static_cast<qsizetype>(bytes->size())),
        &parse_error);
    if (parse_error.error != QJsonParseError::NoError
            || !document.isObject()) {
        return {};
    }
    const auto object = document.object();
    const auto schema = object.value(QStringLiteral("schema"));
    const auto version = object.value(QStringLiteral("schema_version"));
    const auto bin_dir_value = object.value(QStringLiteral("bin_dir"));
    const auto binaries_value = object.value(QStringLiteral("managed_binaries"));
    if (!schema.isString()
            || schema.toString() != QString::fromUtf8(kReceiptSchema)
            || !version.isDouble() || version.toInteger(-1) != 1
            || version.toDouble() != 1.0
            || !bin_dir_value.isString()
            || bin_dir_value.toString().isEmpty()
            || !binaries_value.isArray()) {
        return {};
    }

    const fs::path bin_dir(bin_dir_value.toString().toStdU16String());
    if (!bin_dir.is_absolute()) return {};
    const auto candidate = (bin_dir / kExecutableName).lexically_normal();
    bool candidate_is_managed = false;
    for (const auto &value : binaries_value.toArray()) {
        if (!value.isString() || value.toString().isEmpty()) return {};
        const fs::path managed(value.toString().toStdU16String());
        if (managed.is_absolute()
                && managed.lexically_normal() == candidate) {
            candidate_is_managed = true;
        }
    }
    return candidate_is_managed ? candidate : fs::path();
}

[[nodiscard]] fs::path from_inherited_path(std::string_view inherited_path) {
    std::size_t begin = 0;
    while (begin <= inherited_path.size()) {
        const auto end = inherited_path.find(':', begin);
        const auto entry = inherited_path.substr(
            begin,
            end == std::string_view::npos
                ? inherited_path.size() - begin
                : end - begin);
        // Empty PATH components normally mean the current directory. Ignore
        // them so resolution never depends on an implicit working directory.
        if (!entry.empty()) {
            const auto candidate = fs::path(entry) / kExecutableName;
            if (valid_executable(candidate)) return candidate;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return {};
}

} // namespace

fs::path resolve_tui_executable(const TuiExecutableSearch &search) noexcept {
    try {
        if (auto candidate = from_inherited_path(search.inherited_path);
                !candidate.empty()) {
            return candidate;
        }
        if (auto candidate = managed_candidate(search.home);
                !candidate.empty() && valid_executable(candidate)) {
            return candidate;
        }
        if (!search.home.empty()) {
            const auto candidate = search.home / ".local" / "bin"
                / kExecutableName;
            if (valid_executable(candidate)) return candidate;
        }
        for (const auto &directory : {
                 search.usr_local_bin, search.opt_homebrew_bin}) {
            if (directory.empty()) continue;
            const auto candidate = directory / kExecutableName;
            if (valid_executable(candidate)) return candidate;
        }
    } catch (...) {
        // Default discovery is best-effort and must fail closed.
    }
    return {};
}

} // namespace lingtai::desktop
