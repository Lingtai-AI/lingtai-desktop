#include "native_shell.h"

#include "ui/UiTestFonts.h"
#include "ui/UiTestHarness.h"
#include "ui/object_names.h"
#include "visual/VisualScenario.h"
#include "visual/VisualSnapshotHarness.h"
#include "visual/VisualTestUtils.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>
#include <QtGui/QImage>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {

using lingtai::desktop::ui_test::ThemeMode;

void requireVisual(
        const lingtai::desktop::visual_test::VisualDiffResult &result) {
    if (result.passed) {
        return;
    }
    throw std::runtime_error(result.message.toStdString());
}

void captureSurface(
        std::string_view surface,
        ThemeMode theme) {
    const auto fixtures_root = std::filesystem::path(
        LINGTAI_UI_TEST_FIXTURES_DIR);
    const auto viewport = lingtai::desktop::ui_test::kNormalViewport;
    const auto viewport_name = std::string_view("normal");
    const auto snapshot_id =
        lingtai::desktop::visual_test::surfaceSnapshotId(surface);
    const auto theme_label = theme == ThemeMode::dark ? "dark" : "light";

    lingtai::desktop::visual_test::ThemeScope theme_scope(theme);
    lingtai::desktop::NativeShell shell(
        lingtai::desktop::ui_test::defaultUiTestOptions());
    lingtai::desktop::ui_test::showShellAt(shell, viewport);
    auto &window = static_cast<QWidget &>(shell.window());

    QWidget *target = nullptr;
    if (surface == "startup-idle") {
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "setup-preset") {
        const auto sandbox =
            lingtai::desktop::visual_test::makeSetupSandbox();
        lingtai::desktop::visual_test::startSetupWizard(shell, sandbox);
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "setup-agents") {
        const auto sandbox =
            lingtai::desktop::visual_test::makeSetupSandbox();
        lingtai::desktop::visual_test::startSetupWizard(shell, sandbox);
        lingtai::desktop::visual_test::advanceSetupToAgentsPage(shell);
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "setup-review") {
        const auto sandbox =
            lingtai::desktop::visual_test::makeSetupSandbox();
        lingtai::desktop::visual_test::startSetupWizard(shell, sandbox);
        lingtai::desktop::visual_test::advanceSetupToReviewPage(shell, sandbox);
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "conversation") {
        const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
            fixtures_root, "typical");
        const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
            shell, project, std::filesystem::path(".lingtai/alpha"));
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("typical fixture must open");
        }
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else if (surface == "composer-attachments") {
        const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
            fixtures_root, "typical");
        const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
            shell, project, std::filesystem::path(".lingtai/alpha"));
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("typical fixture must open");
        }
        const auto sandbox = std::filesystem::temp_directory_path()
            / ("lingtai-visual-composer-"
                + std::to_string(static_cast<unsigned long long>(
                    QCoreApplication::applicationPid())));
        std::filesystem::create_directories(sandbox);
        const auto report = sandbox / "quarterly-risk-report.txt";
        std::ofstream(report) << "Risk is bounded.\n";
        const auto image = sandbox / "market-map.PNG";
        QImage preview(160, 96, QImage::Format_RGB32);
        preview.fill(QColor(QStringLiteral("#3A8D73")));
        if (!preview.save(QString::fromStdString(image.string()))) {
            throw std::runtime_error("composer preview fixture must save");
        }
        shell.set_attachment_picker([report, image] {
            return std::vector<std::filesystem::path>{report, image};
        });
        auto *button = window.findChild<QPushButton *>(
            "lingtai_composer_attachment_button");
        if (!button) throw std::runtime_error("attachment button is missing");
        button->click();
        QCoreApplication::processEvents();
        auto *report_name = window.findChild<QLabel *>(
            "lingtai_composer_attachment_name_0");
        auto *remove = window.findChild<QPushButton *>(
            "lingtai_composer_attachment_remove_0");
        if (!report_name
                || report_name->text()
                    != QStringLiteral("quarterly-risk-report.txt")) {
            throw std::runtime_error(
                "the ordinary composer filename must remain fully readable");
        }
        if (!remove || remove->size() != QSize(28, 28)
                || remove->focusPolicy() != Qt::StrongFocus) {
            throw std::runtime_error(
                "the composer remove target must remain 28x28 and focusable");
        }
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else if (surface == "presets") {
        const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
            fixtures_root, "typical");
        const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
            shell, project, std::filesystem::path(".lingtai/alpha"));
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("typical fixture must open");
        }
        lingtai::desktop::ui_test::submitSlashCommand(
            window, QStringLiteral("/presets"));
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else if (surface == "kanban") {
        const auto sandbox = std::filesystem::temp_directory_path()
            / ("lingtai-visual-kanban-"
                + std::to_string(static_cast<unsigned long long>(
                    QCoreApplication::applicationPid())));
        const auto project = sandbox / "project";
        std::filesystem::create_directories(project);
        lingtai::desktop::visual_test::prepareKanbanProject(project);
        const auto outcome = shell.open_project(project, std::nullopt);
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("kanban fixture must open");
        }
        lingtai::desktop::visual_test::selectFirstAgentRow(window);
        lingtai::desktop::ui_test::submitSlashCommand(
            window, QStringLiteral("/kanban"));
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else if (surface == "empty-conversation") {
        const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
            fixtures_root, "empty");
        const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
            shell, project, std::filesystem::path(".lingtai/agent"));
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("empty fixture must open");
        }
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else {
        throw std::runtime_error("unknown visual surface");
    }

    requireVisual(lingtai::desktop::visual_test::assertWidgetSnapshot(
        *target,
        QString(LINGTAI_VISUAL_BASELINE_ROOT),
        "native_shell_visual",
        snapshot_id,
        viewport_name,
        theme));

    std::cout << "native_shell_visual@" << snapshot_id << '@' << theme_label
              << ": OK\n";
}

[[nodiscard]] std::optional<std::string_view> parseFlagValue(
        std::string_view arg,
        std::string_view prefix) {
    if (!arg.starts_with(prefix)) {
        return std::nullopt;
    }
    return arg.substr(prefix.size());
}

[[nodiscard]] ThemeMode parseTheme(std::string_view value) {
    if (value == "light") {
        return ThemeMode::light;
    }
    if (value == "dark") {
        return ThemeMode::dark;
    }
    throw std::runtime_error("unknown theme: " + std::string(value));
}

[[nodiscard]] bool isKnownSurface(std::string_view surface) {
    for (const auto *known : {
            "startup-idle",
            "setup-preset",
            "setup-agents",
            "setup-review",
            "conversation",
            "composer-attachments",
            "presets",
            "kanban",
            "empty-conversation",
        }) {
        if (surface == known) {
            return true;
        }
    }
    return false;
}

void printUsage() {
    std::cerr
        << "usage: native_shell_visual_test --surface=NAME --theme=light|dark\n"
        << "  surfaces: startup-idle setup-preset setup-agents setup-review\n"
        << "            conversation composer-attachments presets kanban empty-conversation\n";
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_LOGGING_RULES",
        "qt.qpa.fonts.warning=false;qt.qpa.keymapper.warning=false");
    lingtai::desktop::ui_test::applyMacTestPlatformDefaults();
    QApplication app(argc, argv);
    lingtai::desktop::ui_test::applyUiTestFontDefaults();

    std::optional<std::string_view> surface;
    std::optional<ThemeMode> theme;
    for (int index = 1; index < argc; ++index) {
        const auto arg = std::string_view(argv[index]);
        if (const auto value = parseFlagValue(arg, "--surface=")) {
            surface = *value;
            continue;
        }
        if (const auto value = parseFlagValue(arg, "--theme=")) {
            theme = parseTheme(*value);
            continue;
        }
        printUsage();
        return 2;
    }

    if (!surface || !theme) {
        printUsage();
        return 2;
    }
    if (!isKnownSurface(*surface)) {
        std::cerr << "FAIL: unknown surface " << *surface << '\n';
        return 2;
    }

    try {
        captureSurface(*surface, *theme);
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
