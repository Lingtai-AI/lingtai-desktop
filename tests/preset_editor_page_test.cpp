#include "preset_editor_page.h"

#include "codex_model_catalog.h"
#include "preset_editor_model.h"

#include "base/basic_types.h"
#include "styles/palette.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::CodexModelCatalogFetcher;
using lingtai::desktop::CodexModelOption;
using lingtai::desktop::PresetEditorLoadRequest;
using lingtai::desktop::PresetEditorPage;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

bool wait_until(const std::function<bool()> &predicate, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) return true;
    }
    return predicate();
}

void write_http_response(QTcpSocket *socket, const QByteArray &body,
        const QByteArray &status = "200 OK") {
    socket->write("HTTP/1.1 " + status + "\r\n"
        "Content-Type: application/json\r\nConnection: close\r\n\r\n" + body);
    socket->flush();
    socket->disconnectFromHost();
}

QString write_initial_preset(const QString &global_dir, const QString &name,
        const QString &provider, const QString &model) {
    const auto path = QDir(global_dir).filePath(QStringLiteral("presets/saved/") + name
        + QStringLiteral(".json"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    const auto root = QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), QJsonObject{
            {QStringLiteral("summary"), QStringLiteral("test preset")},
            {QStringLiteral("tier"), QStringLiteral("1")},
        }},
        {QStringLiteral("manifest"), QJsonObject{{QStringLiteral("llm"), QJsonObject{
            {QStringLiteral("provider"), provider},
            {QStringLiteral("model"), model},
            {QStringLiteral("thinking"), QStringLiteral("xhigh")},
        }}}},
    };
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "must write the initial on-disk preset fixture");
    file.write(QJsonDocument(root).toJson());
    return path;
}

QString fixture_global(const fs::path &fixture, const char *subdir) {
    return QString::fromStdString((fixture / subdir / "global").string());
}

// A background catalog refresh that lands while the page is a hidden
// ancestor (see preset_editor_page.cpp's isHidden()/isVisible() fix) must
// never clear an in-progress custom model-id draft, and that same draft
// must still be what Save persists.
void test_custom_draft_preserved_across_hidden_refresh_and_saves_slug(const fs::path &fixture) {
    const auto global = fixture_global(fixture, "custom-draft");
    qputenv("LINGTAI_TUI_DIR", global.toUtf8());
    const auto preset_path = write_initial_preset(global, QStringLiteral("codex-test-preset"),
        QStringLiteral("codex"), QStringLiteral("gpt-6-alpha"));

    QWidget container;
    auto *page = new PresetEditorPage(&container);
    container.hide(); // Explicit: "page is a hidden ancestor", never shown.

    auto *fetcher = page->findChild<CodexModelCatalogFetcher *>();
    require(fetcher != nullptr, "custom-draft: page must own a CodexModelCatalogFetcher child");

    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "custom-draft: server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) connections.push_back(server.nextPendingConnection());
    });
    fetcher->set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort())));

    PresetEditorLoadRequest request;
    request.path = preset_path;
    request.name = QStringLiteral("codex-test-preset");
    request.summary = QStringLiteral("test preset");
    request.source = QStringLiteral("saved");
    page->load(request); // Fires refresh_async() against our injected server.

    auto *model_combo = page->findChild<QComboBox *>("lingtai_setup_edit_preset_model");
    auto *model_edit = page->findChild<QLineEdit *>("lingtai_setup_edit_preset_model_edit");
    auto *name_field = page->findChild<QLineEdit *>("lingtai_setup_edit_preset_name");
    auto *summary_field = page->findChild<QLineEdit *>("lingtai_setup_edit_preset_summary");
    require(model_combo && model_edit && name_field && summary_field,
        "custom-draft: required controls must exist");

    auto sentinel_index = -1;
    for (auto i = 0; i != model_combo->count(); ++i) {
        if (model_combo->itemData(i).toString().isEmpty()) sentinel_index = i;
    }
    require(sentinel_index >= 0, "custom-draft: the explicit Custom… row must exist");
    model_combo->setCurrentIndex(sentinel_index);
    emit model_combo->activated(sentinel_index);
    require(!model_edit->isHidden(),
        "custom-draft: custom entry must be shown after activating Custom…");

    model_edit->setText(QStringLiteral("my-custom-id")); // No editingFinished: still a draft.
    const auto summary_draft = QStringLiteral("still typing this summary");
    summary_field->setText(summary_draft);
    const auto name_before = name_field->text();

    require(wait_until([&] { return !connections.isEmpty(); }, 2000),
        "custom-draft: the page's own fetcher must connect to the injected server");
    write_http_response(connections[0],
        R"({"models": [{"slug": "gpt-new-suggestion", "display_name": "GPT New Suggestion"}]})");
    require(wait_until([&] {
        return model_combo->findData(QStringLiteral("gpt-new-suggestion")) >= 0;
    }, 2000), "custom-draft: the async refresh must eventually land in the combo");

    require(model_edit->text() == QStringLiteral("my-custom-id"),
        "custom-draft: a background refresh while the page is a hidden ancestor "
        "must never clear the in-progress custom draft");
    require(!model_edit->isHidden(), "custom-draft: custom mode must survive the refresh");
    require(summary_field->text() == summary_draft,
        "custom-draft: a background refresh must never touch other in-progress drafts");
    require(name_field->text() == name_before,
        "custom-draft: a background refresh must never touch the name field");

    auto *save_button = page->findChild<QPushButton *>("lingtai_setup_edit_preset_save");
    require(save_button != nullptr, "custom-draft: save button must exist");
    save_button->click();

    QFile saved(preset_path);
    require(saved.open(QIODevice::ReadOnly), "custom-draft: the saved preset file must be readable");
    const auto persisted_model = QJsonDocument::fromJson(saved.readAll()).object()
        .value(QStringLiteral("manifest")).toObject()
        .value(QStringLiteral("llm")).toObject()
        .value(QStringLiteral("model")).toString();
    require(persisted_model == QStringLiteral("my-custom-id"),
        "custom-draft: Save must persist the typed custom id, never a display label");
}

// A refresh that relabels an already-selected suggestion must keep the same
// selection (by slug) and only change what is shown, never what Save writes.
void test_relabel_preserves_selection(const fs::path &fixture) {
    const auto global = fixture_global(fixture, "relabel");
    qputenv("LINGTAI_TUI_DIR", global.toUtf8());
    require(lingtai::desktop::write_codex_model_cache(global, {
        CodexModelOption{QStringLiteral("o9-turbo"), QStringLiteral("GPT-O9 Turbo (old)")},
    }), "relabel: seeding the initial cache must succeed");
    const auto preset_path = write_initial_preset(global, QStringLiteral("relabel-test-preset"),
        QStringLiteral("codex"), QStringLiteral("o9-turbo"));

    QWidget container;
    auto *page = new PresetEditorPage(&container);
    container.hide();
    auto *fetcher = page->findChild<CodexModelCatalogFetcher *>();
    require(fetcher != nullptr, "relabel: page must own a fetcher");

    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "relabel: server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) connections.push_back(server.nextPendingConnection());
    });
    fetcher->set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort())));

    PresetEditorLoadRequest request;
    request.path = preset_path;
    request.name = QStringLiteral("relabel-test-preset");
    request.summary = QStringLiteral("test preset");
    request.source = QStringLiteral("saved");
    page->load(request);

    auto *model_combo = page->findChild<QComboBox *>("lingtai_setup_edit_preset_model");
    require(model_combo != nullptr, "relabel: combo must exist");
    require(model_combo->currentData().toString() == QStringLiteral("o9-turbo"),
        "relabel: initial selection must be the current model's slug");
    require(model_combo->currentText() == QStringLiteral("GPT-O9 Turbo (old)"),
        "relabel: initial label must come from the seeded cache");

    require(wait_until([&] { return !connections.isEmpty(); }, 2000),
        "relabel: the fetcher must connect");
    write_http_response(connections[0],
        R"JSON({"models": [{"slug": "o9-turbo", "display_name": "GPT-O9 Turbo (NEW)"}]})JSON");
    require(wait_until([&] {
        return model_combo->currentText() == QStringLiteral("GPT-O9 Turbo (NEW)");
    }, 2000), "relabel: the refresh must relabel the still-selected row");
    require(model_combo->currentData().toString() == QStringLiteral("o9-turbo"),
        "relabel: the selection (by slug) must be unchanged after a relabel");

    auto *save_button = page->findChild<QPushButton *>("lingtai_setup_edit_preset_save");
    require(save_button != nullptr, "relabel: save button must exist");
    save_button->click();
    QFile saved(preset_path);
    require(saved.open(QIODevice::ReadOnly), "relabel: saved file must be readable");
    const auto persisted_model = QJsonDocument::fromJson(saved.readAll()).object()
        .value(QStringLiteral("manifest")).toObject()
        .value(QStringLiteral("llm")).toObject()
        .value(QStringLiteral("model")).toString();
    require(persisted_model == QStringLiteral("o9-turbo"),
        "relabel: Save must persist the slug, never the (now different) label");
}

// A Codex catalog refresh must never touch a non-Codex provider's model list.
void test_non_codex_provider_unaffected_by_refresh(const fs::path &fixture) {
    const auto global = fixture_global(fixture, "other-provider");
    qputenv("LINGTAI_TUI_DIR", global.toUtf8());
    const auto preset_path = write_initial_preset(global, QStringLiteral("minimax-test-preset"),
        QStringLiteral("minimax"), QStringLiteral("MiniMax-M3"));

    QWidget container;
    auto *page = new PresetEditorPage(&container);
    container.hide();
    auto *fetcher = page->findChild<CodexModelCatalogFetcher *>();
    require(fetcher != nullptr, "other-provider: page must own a fetcher");

    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "other-provider: server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) connections.push_back(server.nextPendingConnection());
    });
    fetcher->set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort())));

    PresetEditorLoadRequest request;
    request.path = preset_path;
    request.name = QStringLiteral("minimax-test-preset");
    request.summary = QStringLiteral("test preset");
    request.source = QStringLiteral("saved");
    page->load(request);

    auto *model_combo = page->findChild<QComboBox *>("lingtai_setup_edit_preset_model");
    require(model_combo != nullptr, "other-provider: combo must exist");
    auto before_items = QStringList();
    auto before_data = QStringList();
    for (auto i = 0; i != model_combo->count(); ++i) {
        before_items << model_combo->itemText(i);
        before_data << model_combo->itemData(i).toString();
    }

    require(wait_until([&] { return !connections.isEmpty(); }, 2000),
        "other-provider: the fetcher must still connect (it always refreshes on load)");
    write_http_response(connections[0],
        R"({"models": [{"slug": "gpt-irrelevant", "display_name": "GPT Irrelevant"}]})");
    require(wait_until([&] { return !lingtai::desktop::read_codex_model_cache(global).isEmpty(); }, 2000),
        "other-provider: the fetch itself must still complete and update the cache");

    auto after_items = QStringList();
    auto after_data = QStringList();
    for (auto i = 0; i != model_combo->count(); ++i) {
        after_items << model_combo->itemText(i);
        after_data << model_combo->itemData(i).toString();
    }
    require(before_items == after_items && before_data == after_data,
        "other-provider: a Codex catalog refresh must never change a "
        "non-Codex provider's model list");
    for (const auto &slug : after_data) {
        require(!slug.isEmpty(),
            "other-provider: non-Codex rows keep label == slug, never the empty sentinel");
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: preset_editor_page_test FIXTURE_ROOT\n";
        return 2;
    }
    const auto fixture = fs::path(argv[1]);
    std::error_code cleanup_error;
    fs::remove_all(fixture, cleanup_error);
    if (cleanup_error) {
        std::cerr << "preset_editor_page_test FAILED: fixture root must start clean\n";
        return 1;
    }
    const auto fixture_home = QString::fromStdString((fixture / "home").string());
    qputenv("HOME", fixture_home.toUtf8());
    qputenv("LINGTAI_TUI_DIR", fixture_home.toUtf8());

    QApplication app(argc, argv);
    style::internal::init_palette(style::kScaleDefault);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    if (!lingtai::desktop::lingtai_global_dir().startsWith(fixture_home)) {
        std::cerr << "preset_editor_page_test FAILED: refusing to run unless "
                     "lingtai_global_dir() resolves under the disposable fixture root\n";
        return 1;
    }

    try {
        test_custom_draft_preserved_across_hidden_refresh_and_saves_slug(fixture);
        test_relabel_preserves_selection(fixture);
        test_non_codex_provider_unaffected_by_refresh(fixture);

        fs::remove_all(fixture, cleanup_error);
        std::cout << "preset_editor_page_test: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "preset_editor_page_test FAILED: " << error.what() << "\n";
        return 1;
    }
}
