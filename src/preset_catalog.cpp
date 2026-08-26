#include "preset_catalog.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

#include <algorithm>
#include <filesystem>
#include <map>
#include <system_error>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

constexpr auto kMaximumPresetBytes = qint64(1024 * 1024);

QString path_text(const fs::path &path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

fs::path filesystem_path(const QString &path) {
    return fs::path(path.toStdString());
}

int template_sort_rank(const std::string &name) {
    static const auto ranks = std::map<std::string, int>{
        {"minimax", 0}, {"zhipu", 1}, {"mimo", 2}, {"deepseek", 3},
        {"kimi", 4}, {"grok", 5}, {"nvidia", 6}, {"openrouter", 7},
        {"codex", 8}, {"codex-pool", 9}, {"claude", 10}, {"custom", 11},
    };
    const auto found = ranks.find(name);
    return found == ranks.end() ? 999 : found->second;
}

PresetCatalogLoadResult directory_failure(
        const fs::path &directory, const std::error_code &error) {
    auto detail = std::string("cannot read preset directory: ")
        + directory.string();
    if (error) detail += ": " + error.message();
    return {{}, PresetCatalogLoadFailure::directory_read_failed,
        std::move(detail)};
}

void append_valid_preset(
        std::vector<PresetEntry> &entries,
        const fs::path &path,
        const std::string &source) {
    QFile file(path_text(path));
    const auto info = QFileInfo(file);
    if (!info.exists() || info.isDir() || info.size() < 0
            || info.size() > kMaximumPresetBytes
            || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const auto root = document.object();
    const auto name = root.value(QLatin1String("name")).toString().trimmed();
    if (name.isEmpty()) return;

    auto summary = QString();
    auto tier = QString();
    const auto description = root.value(QLatin1String("description"));
    if (description.isString()) {
        summary = description.toString().trimmed();
        tier = root.value(QLatin1String("tier")).toString().trimmed();
    } else if (description.isObject()) {
        const auto object = description.toObject();
        summary = object.value(QLatin1String("summary")).toString().trimmed();
        tier = object.value(QLatin1String("tier")).toString().trimmed();
    }
    entries.push_back(PresetEntry{
        name.toStdString(), summary.toStdString(), tier.toStdString(), source,
        path_text(path).toStdString(),
    });
}

PresetCatalogLoadResult scan_directory(
        const fs::path &directory,
        const std::string &source,
        std::vector<PresetEntry> &entries) {
    std::error_code error;
    const auto exists = fs::exists(directory, error);
    if (error) return directory_failure(directory, error);
    if (!exists) return {};
    const auto is_directory = fs::is_directory(directory, error);
    if (error || !is_directory) return directory_failure(directory, error);

    fs::directory_iterator iterator(directory, error);
    if (error) return directory_failure(directory, error);
    const auto end = fs::directory_iterator();
    while (iterator != end) {
        const auto path = iterator->path();
        auto entry_error = std::error_code();
        const auto is_directory_entry = iterator->is_directory(entry_error);
        if (entry_error) return directory_failure(directory, entry_error);
        const auto filename = path.filename().string();
        if (!is_directory_entry && filename != "_kernel_meta.json"
                && path.extension() == ".json") {
            append_valid_preset(entries, path, source);
        }
        iterator.increment(error);
        if (error) return directory_failure(directory, error);
    }
    return {};
}

} // namespace

bool preset_catalog_order_less(
        const PresetEntry &left, const PresetEntry &right) {
    const auto left_template = left.source == "template";
    const auto right_template = right.source == "template";
    if (left_template != right_template) return !left_template;
    if (left_template) {
        const auto left_rank = template_sort_rank(left.name);
        const auto right_rank = template_sort_rank(right.name);
        if (left_rank != right_rank) return left_rank < right_rank;
    }
    return left.name < right.name;
}

PresetCatalogLoadResult load_preset_catalog(const QString &global_dir) noexcept {
    try {
        auto entries = std::vector<PresetEntry>();
        const auto root = filesystem_path(global_dir);
        for (const auto &[directory, source] : {
                std::pair{"saved", "saved"},
                std::pair{"templates", "template"}}) {
            const auto result = scan_directory(
                root / "presets" / directory, source, entries);
            if (!result) return result;
        }
        std::stable_sort(
            entries.begin(), entries.end(), preset_catalog_order_less);
        return {std::move(entries), PresetCatalogLoadFailure::none, {}};
    } catch (const std::exception &error) {
        return {{}, PresetCatalogLoadFailure::directory_read_failed,
            error.what()};
    } catch (...) {
        return {{}, PresetCatalogLoadFailure::directory_read_failed,
            "unknown preset catalog read failure"};
    }
}

QString normalize_preset_reference(
        const QString &reference, const QString &global_dir) {
    auto expanded = reference.trimmed();
    const auto standard_prefix = QStringLiteral("~/.lingtai-tui");
    if (expanded == standard_prefix) {
        expanded = global_dir;
    } else if (expanded.startsWith(standard_prefix + QLatin1Char('/'))) {
        expanded = QDir(global_dir).filePath(expanded.mid(standard_prefix.size() + 1));
    } else if (expanded.startsWith(QLatin1String("~/"))) {
        expanded = QDir::homePath() + expanded.mid(1);
    }
    if (expanded.isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(expanded).absoluteFilePath());
}

} // namespace lingtai::desktop
