#pragma once

#include <QtGui/QImage>
#include <QtGui/QPixmap>

#include <QString>

namespace lingtai::desktop::visual_test {

struct VisualDiffOptions {
    int channel_threshold = 10;
    double max_changed_ratio = 0.001;
};

struct VisualDiffResult {
    bool passed = false;
    int changed_pixels = 0;
    double changed_ratio = 0.0;
    QString actual_path;
    QString expected_path;
    QString diff_path;
    QString message;
};

// Milestone 4: full PNG compare + diff image generation.
[[nodiscard]] VisualDiffResult compareSnapshot(
    const QImage &actual,
    const QImage &expected,
    const VisualDiffOptions &options = {});

[[nodiscard]] bool saveWidgetSnapshot(QWidget &widget, const QString &path);

} // namespace lingtai::desktop::visual_test
