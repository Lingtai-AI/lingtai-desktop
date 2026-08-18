#include "preset_catalog_presentation.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

#include <algorithm>
#include <map>

namespace lingtai::desktop {
namespace {

QString expand_home_path(const std::string &path) {
    const auto text = QString::fromStdString(path);
    if (text.startsWith(QStringLiteral("~/"))) {
        return QDir::homePath() + text.mid(1);
    }
    return text;
}

QString json_string(const QJsonObject &object, QLatin1StringView key) {
    return object.value(key).toString().trimmed();
}

bool manifest_has_vision(const QJsonObject &manifest) {
    const auto capabilities = manifest.value(QLatin1StringView("capabilities"));
    if (!capabilities.isObject()) {
        return false;
    }
    const auto vision = capabilities.toObject().value(QLatin1StringView("vision"));
    if (vision.isNull() || vision.isUndefined()) {
        return false;
    }
    if (vision.isObject() && vision.toObject().isEmpty()) {
        return false;
    }
    return true;
}

void apply_manifest_facts(PresetCatalogRow &row) {
    const auto path = expand_home_path(row.entry.path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        row.summary = QString::fromStdString(row.entry.description);
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        row.summary = QString::fromStdString(row.entry.description);
        return;
    }
    const auto root = document.object();
    const auto description = root.value(QLatin1StringView("description"));
    if (description.isString()) {
        row.summary = description.toString().trimmed();
    } else if (description.isObject()) {
        row.summary = json_string(description.toObject(), QLatin1StringView("summary"));
    }
    if (row.summary.isEmpty()) {
        row.summary = QString::fromStdString(row.entry.description);
    }

    const auto manifest = root.value(QLatin1StringView("manifest")).toObject();
    const auto llm = manifest.value(QLatin1StringView("llm")).toObject();
    row.provider = json_string(llm, QLatin1StringView("provider"));
    row.model = json_string(llm, QLatin1StringView("model"));
    row.has_vision = manifest_has_vision(manifest);
}

QString format_provider_model(const QString &provider, const QString &model) {
    if (provider.isEmpty() && model.isEmpty()) {
        return QString();
    }
    if (provider.isEmpty()) {
        return model;
    }
    if (model.isEmpty()) {
        return provider;
    }
    return provider + QStringLiteral(" · ") + model;
}

int template_sort_rank(const std::string &name) {
    // Keep in sync with preset.List() templateOrder in lingtai-tui.
    static const auto ranks = std::map<std::string, int>{
        {"minimax", 0}, {"zhipu", 1}, {"mimo", 2}, {"deepseek", 3},
        {"kimi", 4}, {"grok", 5}, {"nvidia", 6}, {"openrouter", 7},
        {"codex", 8}, {"codex-pool", 9}, {"claude", 10}, {"custom", 11},
    };
    const auto found = ranks.find(name);
    return found == ranks.end() ? 999 : found->second;
}

} // namespace

std::vector<PresetCatalogRow> build_preset_catalog_rows(
        const std::vector<PresetEntry> &presets) {
    auto rows = std::vector<PresetCatalogRow>();
    rows.reserve(presets.size());
    for (const auto &entry : presets) {
        PresetCatalogRow row{entry, {}, {}, {}, {}, false, false};
        row.is_template = QString::fromStdString(entry.source)
            .compare(QStringLiteral("template"), Qt::CaseInsensitive) == 0;
        apply_manifest_facts(row);
        row.provider_model = format_provider_model(row.provider, row.model);
        if (row.provider_model.isEmpty()) {
            row.provider_model = row.summary;
        }
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(),
        [](const PresetCatalogRow &left, const PresetCatalogRow &right) {
            if (left.is_template != right.is_template) {
                return !left.is_template;
            }
            if (left.is_template) {
                const auto left_rank = template_sort_rank(left.entry.name);
                const auto right_rank = template_sort_rank(right.entry.name);
                if (left_rank != right_rank) {
                    return left_rank < right_rank;
                }
            }
            return left.entry.name < right.entry.name;
        });
    return rows;
}

} // namespace lingtai::desktop
