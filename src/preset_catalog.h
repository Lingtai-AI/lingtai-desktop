#pragma once

#include <QtCore/QString>

#include <string>
#include <vector>

namespace lingtai::desktop {

struct PresetEntry {
    std::string name;
    std::string description;
    std::string tier;
    std::string source;
    std::string path;
};

enum class PresetCatalogLoadFailure {
    none,
    directory_read_failed,
};

struct PresetCatalogLoadResult {
    std::vector<PresetEntry> presets;
    PresetCatalogLoadFailure failure = PresetCatalogLoadFailure::none;
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept {
        return failure == PresetCatalogLoadFailure::none;
    }
};

// Desktop's read-only owner for the global saved/template preset catalog.
// Missing directories are empty; an existing directory that cannot be read is
// a typed failure. The function never creates or updates global preset files.
[[nodiscard]] PresetCatalogLoadResult load_preset_catalog(
    const QString &global_dir) noexcept;

// Resolve a stored preset ref for comparison only. The injected global root is
// treated as the hermetic equivalent of ~/.lingtai-tui, so tests need no live
// HOME access and production absolute/~/ refs compare as the same file.
[[nodiscard]] QString normalize_preset_reference(
    const QString &reference, const QString &global_dir);

[[nodiscard]] bool preset_catalog_order_less(
    const PresetEntry &left, const PresetEntry &right);

} // namespace lingtai::desktop
