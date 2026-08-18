#pragma once

#include "project_bootstrap.h"

#include <QtCore/QString>

#include <vector>

namespace lingtai::desktop {

struct PresetCatalogRow {
    PresetEntry entry;
    QString summary;
    QString provider;
    QString model;
    QString provider_model;
    bool has_vision = false;
    bool has_tools = false;
    bool is_template = false;
};

// Orders presets like `preset.List()` in the TUI: saved alphabetical,
// then templates in the canonical product order.
[[nodiscard]] std::vector<PresetCatalogRow> build_preset_catalog_rows(
    const std::vector<PresetEntry> &presets);

} // namespace lingtai::desktop
