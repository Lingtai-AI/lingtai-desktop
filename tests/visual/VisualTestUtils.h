#pragma once

#include <QtGui/QImage>
#include <QtGui/QPixmap>

#include <QString>

class QWidget;

namespace lingtai::desktop::visual_test {

struct VisualDiffOptions {
    int channel_threshold = 10;
    // Headroom for sub-pixel antialiasing across macOS runners after DPR
    // normalization and bundled Open Sans loading.
    double max_changed_ratio = 0.02;
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

[[nodiscard]] bool updateBaselinesEnabled();

[[nodiscard]] QString artifactsRoot();

[[nodiscard]] QString baselinePath(
    const QString &baseline_root,
    const QString &theme,
    const QString &snapshot_name);

[[nodiscard]] QImage normalizeToLogicalPixels(
    const QImage &device_image,
    qreal device_pixel_ratio);

[[nodiscard]] QImage grabWidgetSnapshot(QWidget &widget);

[[nodiscard]] bool saveImage(const QImage &image, const QString &path);

[[nodiscard]] bool saveWidgetSnapshot(QWidget &widget, const QString &path);

[[nodiscard]] QImage makeDiffImage(
    const QImage &actual,
    const QImage &expected,
    int channel_threshold);

[[nodiscard]] VisualDiffResult compareSnapshot(
    const QImage &actual,
    const QImage &expected,
    const VisualDiffOptions &options = {});

[[nodiscard]] bool writeFailureArtifacts(
    const QString &test_name,
    const QImage &actual,
    const QImage &expected,
    const QImage &diff,
    const VisualDiffResult &result);

[[nodiscard]] VisualDiffResult assertMatchesBaseline(
    QWidget &widget,
    const QString &baseline_path,
    const QString &test_name,
    const VisualDiffOptions &options = {});

} // namespace lingtai::desktop::visual_test
