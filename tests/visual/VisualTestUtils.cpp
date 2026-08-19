#include "VisualTestUtils.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>
#include <QtGui/QColor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <cmath>

namespace lingtai::desktop::visual_test {
namespace {

bool env_truthy(const char *name) {
    const auto value = QProcessEnvironment::systemEnvironment().value(
        QString::fromUtf8(name));
    return !value.isEmpty() && value != QStringLiteral("0");
}

} // namespace

QImage normalizeToLogicalPixels(const QImage &device_image, qreal dpr) {
    if (dpr <= 1.0) {
        return device_image;
    }
    QImage logical = device_image.scaled(
        QSize(
            qRound(static_cast<qreal>(device_image.width()) / dpr),
            qRound(static_cast<qreal>(device_image.height()) / dpr)),
        Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation);
    logical.setDevicePixelRatio(1.0);
    return logical;
}

bool updateBaselinesEnabled() {
    return env_truthy("UPDATE_UI_BASELINES");
}

QString artifactsRoot() {
    const auto from_env = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("ARTIFACTS_DIR"));
    if (!from_env.isEmpty()) {
        return from_env;
    }
    return QStringLiteral("artifacts");
}

QString baselinePath(
        const QString &baseline_root,
        const QString &theme,
        const QString &snapshot_name) {
    return QDir(baseline_root)
        .filePath(QStringLiteral("macos/%1/%2.png").arg(theme, snapshot_name));
}

QImage grabWidgetSnapshot(QWidget &widget) {
    widget.repaint();
    QApplication::processEvents();
    return normalizeToLogicalPixels(
        widget.grab().toImage(), widget.devicePixelRatioF());
}

bool saveImage(const QImage &image, const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    return image.save(path, "PNG");
}

bool saveWidgetSnapshot(QWidget &widget, const QString &path) {
    return saveImage(grabWidgetSnapshot(widget), path);
}

QImage makeDiffImage(
        const QImage &actual,
        const QImage &expected,
        int channel_threshold) {
    QImage diff(actual.size(), QImage::Format_RGB32);
    diff.fill(Qt::black);
    if (actual.size() != expected.size()) {
        diff.fill(QColor(255, 0, 255));
        return diff;
    }

    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const auto a = actual.pixelColor(x, y);
            const auto e = expected.pixelColor(x, y);
            const auto dr = std::abs(a.red() - e.red());
            const auto dg = std::abs(a.green() - e.green());
            const auto db = std::abs(a.blue() - e.blue());
            if (dr > channel_threshold || dg > channel_threshold
                    || db > channel_threshold) {
                diff.setPixelColor(x, y, QColor(255, 0, 128));
            } else {
                const auto gray = qGray(e.rgb());
                diff.setPixelColor(x, y, QColor(gray, gray, gray));
            }
        }
    }
    return diff;
}

VisualDiffResult compareSnapshot(
        const QImage &actual,
        const QImage &expected,
        const VisualDiffOptions &options) {
    VisualDiffResult result;
    result.expected_path = QString();
    result.actual_path = QString();
    if (actual.size() != expected.size()) {
        result.message = QStringLiteral("dimension mismatch");
        return result;
    }

    const auto total = actual.width() * actual.height();
    auto changed = 0;
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const auto a = actual.pixelColor(x, y);
            const auto e = expected.pixelColor(x, y);
            const auto dr = std::abs(a.red() - e.red());
            const auto dg = std::abs(a.green() - e.green());
            const auto db = std::abs(a.blue() - e.blue());
            if (dr > options.channel_threshold || dg > options.channel_threshold
                    || db > options.channel_threshold) {
                ++changed;
            }
        }
    }

    result.changed_pixels = changed;
    result.changed_ratio = total > 0
        ? static_cast<double>(changed) / static_cast<double>(total)
        : 0.0;
    result.passed = result.changed_ratio <= options.max_changed_ratio;
    result.message = result.passed
        ? QStringLiteral("within threshold")
        : QStringLiteral("changed ratio %1 exceeds %2")
              .arg(result.changed_ratio, 0, 'f', 4)
              .arg(options.max_changed_ratio, 0, 'f', 4);
    return result;
}

bool writeFailureArtifacts(
        const QString &test_name,
        const QImage &actual,
        const QImage &expected,
        const QImage &diff,
        const VisualDiffResult &result) {
    const auto dir = QDir(artifactsRoot()).filePath(
        QStringLiteral("visual/%1").arg(test_name));
    QDir().mkpath(dir);

    const auto baseline_path = QDir(dir).filePath(QStringLiteral("baseline.png"));
    const auto actual_path = QDir(dir).filePath(QStringLiteral("actual.png"));
    const auto diff_path = QDir(dir).filePath(QStringLiteral("diff.png"));
    const auto metadata_path = QDir(dir).filePath(QStringLiteral("metadata.json"));

    if (!saveImage(expected, baseline_path)
            || !saveImage(actual, actual_path)
            || !saveImage(diff, diff_path)) {
        return false;
    }

    QJsonObject metadata{
        {QStringLiteral("passed"), result.passed},
        {QStringLiteral("changed_pixels"), result.changed_pixels},
        {QStringLiteral("changed_ratio"), result.changed_ratio},
        {QStringLiteral("message"), result.message},
        {QStringLiteral("baseline"), baseline_path},
        {QStringLiteral("actual"), actual_path},
        {QStringLiteral("diff"), diff_path},
    };
    QFile metadata_file(metadata_path);
    if (!metadata_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    metadata_file.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    return true;
}

VisualDiffResult assertMatchesBaseline(
        QWidget &widget,
        const QString &baseline_path,
        const QString &test_name,
        const VisualDiffOptions &options) {
    const auto actual = grabWidgetSnapshot(widget);
    if (updateBaselinesEnabled()) {
        if (!saveImage(actual, baseline_path)) {
            VisualDiffResult result;
            result.message = QStringLiteral("failed to write baseline");
            return result;
        }
        VisualDiffResult result;
        result.passed = true;
        result.message = QStringLiteral("baseline updated");
        result.expected_path = baseline_path;
        result.actual_path = baseline_path;
        return result;
    }

    QImage expected(baseline_path);
    if (expected.isNull()) {
        VisualDiffResult result;
        result.message = QStringLiteral("missing baseline: ") + baseline_path;
        result.expected_path = baseline_path;
        return result;
    }

    auto result = compareSnapshot(actual, expected, options);
    result.expected_path = baseline_path;
    if (!result.passed) {
        const auto diff = makeDiffImage(actual, expected, options.channel_threshold);
        const auto artifact_dir = QDir(artifactsRoot()).filePath(
            QStringLiteral("visual/%1").arg(test_name));
        QDir().mkpath(artifact_dir);
        result.actual_path = QDir(artifact_dir).filePath(QStringLiteral("actual.png"));
        result.diff_path = QDir(artifact_dir).filePath(QStringLiteral("diff.png"));
        static_cast<void>(writeFailureArtifacts(
            test_name, actual, expected, diff, result));
    }
    return result;
}

} // namespace lingtai::desktop::visual_test
