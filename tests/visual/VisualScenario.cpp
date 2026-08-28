#include "VisualScenario.h"

#include "ui/UiTestHarness.h"
#include "ui/object_names.h"

#include "ui/widgets/fields/input_field.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QStyleHints>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTreeWidget>

#include <fstream>
#include <stdexcept>

namespace lingtai::desktop::visual_test {
namespace {

using ui_test::findNamed;
using ui_test::requireChild;

void writeFile(const std::filesystem::path &path, std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        throw std::runtime_error("visual scenario: create_directories failed");
    }
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    if (!stream.good()) {
        throw std::runtime_error(
            "visual scenario: write failed: " + path.string());
    }
}

void clickButton(QWidget &window, const char *object_name) {
    auto *button = requireChild<QPushButton>(window, object_name);
    button->click();
    QCoreApplication::processEvents();
}

QWidget *rosterRowsCanvas(QWidget &window) {
    auto *scroll = requireChild<QWidget>(window, ui_test::kAgentRosterScroll);
    return requireChild<QWidget>(*scroll, ui_test::kAgentRosterRows);
}

void clickAgentCanvasRow(QWidget &window, int index) {
    auto *canvas = rosterRowsCanvas(window);
    const auto row_height = canvas->height() > 0 ? canvas->height() / 2 : 24;
    const auto point = QPoint(canvas->width() / 2, row_height * index + row_height / 2);
    auto press = QMouseEvent(
        QEvent::MouseButtonPress,
        point,
        point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    auto release = QMouseEvent(
        QEvent::MouseButtonRelease,
        point,
        point,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    QCoreApplication::processEvents();
}

QPalette lightApplicationPalette() {
    auto palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#111827")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#111827")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#F3F4F6")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#111827")));
    return palette;
}

QPalette darkApplicationPalette() {
    auto palette = QApplication::palette();
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#121820")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#F1F5F9")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#0B1118")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#E2E8F0")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#263241")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#F8FAFC")));
    return palette;
}

} // namespace

ThemeScope::ThemeScope(ui_test::ThemeMode mode)
: original_scheme_(QGuiApplication::styleHints()->colorScheme())
, original_palette_(QApplication::palette()) {
    if (mode == ui_test::ThemeMode::dark) {
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        QApplication::setPalette(darkApplicationPalette());
    } else {
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        QApplication::setPalette(lightApplicationPalette());
    }
    QApplication::processEvents();
}

ThemeScope::~ThemeScope() {
    QGuiApplication::styleHints()->setColorScheme(original_scheme_);
    QApplication::setPalette(original_palette_);
    QApplication::processEvents();
}

SetupSandbox makeSetupSandbox() {
    SetupSandbox sandbox{
        .root = std::filesystem::temp_directory_path()
            / ("lingtai-visual-setup-" + std::to_string(
                static_cast<unsigned long long>(QCoreApplication::applicationPid()))),
        .destination = {},
        .global_dir = {},
    };
    sandbox.destination = sandbox.root / "new-project";
    sandbox.global_dir = sandbox.root / "global";
    std::filesystem::create_directories(sandbox.root);
    return sandbox;
}

void installMockSetupCatalog(const SetupSandbox &sandbox) {
    writeFile(sandbox.global_dir / "presets/saved/beta.json", R"JSON({
      "name":"beta",
      "description":{"summary":"Beta preset","tier":"2"},
      "manifest":{"llm":{"provider":"openrouter","model":"beta"},"capabilities":{}}
    })JSON");
    writeFile(sandbox.global_dir / "presets/templates/alpha.json", R"JSON({
      "name":"alpha",
      "description":{"summary":"Alpha preset","tier":"1"},
      "manifest":{"llm":{"provider":"openrouter","model":"alpha"},"capabilities":{}}
    })JSON");
    qputenv("LINGTAI_TUI_DIR",
        QByteArray::fromStdString(sandbox.global_dir.string()));
}

void waitForVisible(QWidget &widget, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!widget.isVisible() && timer.elapsed() < timeout_ms) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    if (!widget.isVisible()) {
        throw std::runtime_error("visual scenario: widget did not become visible");
    }
}

void startSetupWizard(NativeShell &shell, const SetupSandbox &sandbox) {
    installMockSetupCatalog(sandbox);
    auto &window = shell.window();
    auto *wizard = requireChild<QWidget>(window, "lingtai_project_setup_wizard");
    shell.set_open_project_request_handler([&] {
        shell.request_new_project_at(sandbox.destination);
    });
    clickButton(window, ui_test::kStartupChooseProject);
    waitForVisible(*wizard);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        if (auto *catalog = window.findChild<QTreeWidget *>(
                "lingtai_setup_preset_catalog")) {
            if (catalog->topLevelItemCount() > 0) {
                break;
            }
        }
        QThread::msleep(20);
    }
}

void advanceSetupToAgentsPage(NativeShell &shell) {
    auto &window = shell.window();
    auto *chooser = requireChild<QComboBox>(
        window, "lingtai_bootstrap_preset_chooser");
    if (chooser->currentIndex() < 0 && chooser->count() > 0) {
        chooser->setCurrentIndex(0);
    }
    QCoreApplication::processEvents();
    clickButton(window, "lingtai_setup_preset_continue");
    QCoreApplication::processEvents();
    clickButton(window, "lingtai_setup_edit_preset_save");
    QCoreApplication::processEvents();
    waitForVisible(*requireChild<QWidget>(window, "lingtai_setup_agents_page"));
}

void advanceSetupToReviewPage(
        NativeShell &shell,
        const SetupSandbox &sandbox) {
    advanceSetupToAgentsPage(shell);
    auto &window = shell.window();
    clickButton(window, "lingtai_setup_agents_continue");
    QCoreApplication::processEvents();
    waitForVisible(*requireChild<QWidget>(window, "lingtai_setup_review_page"));
    auto *destination = ui_test::findNamed<QObject>(
        window, "lingtai_bootstrap_destination_input");
    if (auto *field = dynamic_cast<Ui::InputField *>(destination)) {
        field->setText(QString::fromStdString(sandbox.destination.string()));
        QCoreApplication::processEvents();
    }
}

void prepareKanbanProject(const std::filesystem::path &project_root) {
    const auto alpha = project_root / ".lingtai/alpha";
    writeFile(project_root / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    writeFile(alpha / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","nickname":"Alpha",)"
        R"("address":"alpha","state":"active","language":"en"})");
    writeFile(alpha / "init.json",
        R"({"model":"claude-opus","provider":"anthropic","mcp":{"fs":{}}})");
    writeFile(alpha / ".status.json",
        R"({"tokens":{"context":{"window_size":200000,"system_tokens":1000,)"
        R"("tools_tokens":500,"history_tokens":2500,"total_tokens":4000,)"
        R"("usage_pct":2.0}}})");
    writeFile(alpha / "logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":100,\"output\":40,"
        "\"thinking\":10,\"cached\":20,\"model\":\"claude-opus\","
        "\"endpoint\":\"https://api.anthropic.com/v1\"}\n");
    writeFile(alpha / "daemons/run-1/daemon.json",
        R"({"state":"running","backend":"claude-p",)"
        R"("cli_tokens":{"input":10,"output":5,"thinking":1,"cached":2,"calls":1}})");
}

void selectFirstAgentRow(QWidget &window) {
    clickAgentCanvasRow(window, 0);
}

QWidget &snapshotTargetForContent(QWidget &window) {
    return *requireChild<QWidget>(window, ui_test::kDesktopContent);
}

QWidget &snapshotTargetForAgentDetail(QWidget &window) {
    return *requireChild<QWidget>(window, ui_test::kAgentDetail);
}

const char *surfaceSnapshotId(std::string_view surface) {
    if (surface == "startup-idle") return "startup-idle";
    if (surface == "setup-preset") return "setup-preset";
    if (surface == "setup-agents") return "setup-agents";
    if (surface == "setup-review") return "setup-review";
    if (surface == "conversation") return "conversation";
    if (surface == "conversation-attachments"
            || surface == "conversation-attachments-narrow") {
        return "conversation-attachments";
    }
    if (surface == "composer-attachments") return "composer-attachments";
    if (surface == "presets") return "presets";
    if (surface == "kanban") return "kanban";
    if (surface == "empty-conversation") return "empty-conversation";
    throw std::runtime_error("visual scenario: unknown surface");
}

} // namespace lingtai::desktop::visual_test
