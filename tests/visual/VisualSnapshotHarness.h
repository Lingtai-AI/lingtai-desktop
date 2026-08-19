#pragma once

#include "VisualTestUtils.h"

#include "ui/UiTestHarness.h"
#include "ui/UiTestUtils.h"
#include "ui/object_names.h"

#include <QString>

namespace lingtai::desktop::visual_test {

using ui_test::ThemeMode;

[[nodiscard]] inline QString themeDirectory(ThemeMode theme) {
    return theme == ThemeMode::dark
        ? QStringLiteral("dark")
        : QStringLiteral("light");
}

[[nodiscard]] inline QString snapshotFileName(
        const char *snapshot_id,
        std::string_view viewport_name) {
    return QStringLiteral("%1-%2")
        .arg(QString::fromUtf8(snapshot_id),
            QString::fromUtf8(viewport_name.data(),
                static_cast<int>(viewport_name.size())));
}

[[nodiscard]] inline VisualDiffResult assertWidgetSnapshot(
        QWidget &widget,
        const QString &baseline_root,
        const char *test_name,
        const char *snapshot_id,
        std::string_view viewport_name,
        ThemeMode theme = ThemeMode::light,
        const VisualDiffOptions &options = {}) {
    const auto path = baselinePath(
        baseline_root,
        themeDirectory(theme),
        snapshotFileName(snapshot_id, viewport_name));
    return assertMatchesBaseline(
        widget,
        path,
        QStringLiteral("%1/%2-%3")
            .arg(QString::fromUtf8(test_name),
                QString::fromUtf8(snapshot_id),
                QString::fromUtf8(viewport_name.data(),
                    static_cast<int>(viewport_name.size()))),
        options);
}

} // namespace lingtai::desktop::visual_test
