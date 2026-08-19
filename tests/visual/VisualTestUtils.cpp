#include "VisualTestUtils.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace lingtai::desktop::visual_test {

VisualDiffResult compareSnapshot(
    const QImage &actual,
    const QImage &expected,
    const VisualDiffOptions &) {
    VisualDiffResult result;
    if (actual.size() != expected.size()) {
        result.message = QStringLiteral("dimension mismatch");
        return result;
    }
    // Stub until Milestone 4 wires pixel-compare and diff PNG output.
    result.passed = (actual == expected);
    result.message = result.passed
        ? QStringLiteral("exact match")
        : QStringLiteral("images differ (stub compare)");
    return result;
}

bool saveWidgetSnapshot(QWidget &widget, const QString &path) {
    widget.repaint();
    QApplication::processEvents();
    return widget.grab().save(path, "PNG");
}

} // namespace lingtai::desktop::visual_test
