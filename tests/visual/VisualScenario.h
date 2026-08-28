#pragma once

#include "native_shell.h"

#include "ui/UiTestUtils.h"

#include <QtWidgets/QWidget>

#include <filesystem>
#include <string_view>

namespace lingtai::desktop::visual_test {

class ThemeScope final {
public:
    explicit ThemeScope(ui_test::ThemeMode mode);
    ~ThemeScope();

    ThemeScope(const ThemeScope &) = delete;
    ThemeScope &operator=(const ThemeScope &) = delete;

private:
    Qt::ColorScheme original_scheme_;
    QPalette original_palette_;
};

struct SetupSandbox {
    std::filesystem::path root;
    std::filesystem::path destination;
    std::filesystem::path global_dir;
};

[[nodiscard]] SetupSandbox makeSetupSandbox();

void installMockSetupCatalog(const SetupSandbox &sandbox);

void startSetupWizard(NativeShell &shell, const SetupSandbox &sandbox);

void waitForVisible(QWidget &widget, int timeout_ms = 3000);

void advanceSetupToAgentsPage(NativeShell &shell);

void advanceSetupToReviewPage(NativeShell &shell, const SetupSandbox &sandbox);

void prepareKanbanProject(const std::filesystem::path &project_root);

void selectFirstAgentRow(QWidget &window);

[[nodiscard]] QWidget &snapshotTargetForContent(QWidget &window);

[[nodiscard]] QWidget &snapshotTargetForAgentDetail(QWidget &window);

[[nodiscard]] const char *surfaceSnapshotId(std::string_view surface);

} // namespace lingtai::desktop::visual_test
