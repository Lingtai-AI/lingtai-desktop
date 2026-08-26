#pragma once

#include "preset_catalog.h"

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

// One catalog row per allowed ref, in published order. Reads the JSON when
// the path exists; otherwise the row still lists the file stem so `/presets`
// can show the agent's allow-list without the global library.
[[nodiscard]] std::vector<PresetCatalogRow> build_preset_catalog_rows_from_refs(
    const std::vector<std::string> &refs);

} // namespace lingtai::desktop
