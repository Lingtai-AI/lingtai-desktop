// The per-Agent unsent composer draft boundary: the real shell's project-open
// selection route keeps one session-only draft per canonical project root +
// Agent key, never resets ongoing typing on reselection or refresh, never
// resurrects a cleared or sent draft, and preserves a failed send.
#include "agent_detail_view.h"
#include "native_shell.h"
#include "runtime_options.h"
#include "shell_host.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

namespace fs = std::filesystem;
namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "composer draft: " << message << '\n';
        std::exit(1);
    }
}

void pump(int ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < ms) {
        QEventLoop iteration;
        QTimer::singleShot(5, &iteration, &QEventLoop::quit);
        iteration.exec(QEventLoop::AllEvents);
    }
}

void write_file(const fs::path &path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    auto stream = std::ofstream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture file must be written");
}

void write_project(const fs::path &project) {
    fs::remove_all(project);
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"human-id","agent_name":"Human",)"
        R"("address":"human","state":"active"})");
    for (const auto name : {"alpha", "beta"}) {
        write_file(project / ".lingtai" / name / ".agent.json",
            std::string(R"({"admin":{},"agent_id":")") + name
            + R"(-id","agent_name":")" + name + R"(","address":")" + name
            + R"(","state":"active"})");
    }
}

} // namespace

int main(int argc, char **argv) {
    require(argc == 2, "usage: lingtai_composer_draft_test <fixture-root>");
    QApplication app(argc, argv);
    const auto root = fs::path(argv[1]);
    const auto one = root / "one";
    const auto two = root / "two";
    write_project(one);
    write_project(two);
    const auto file = root / "note.txt";
    write_file(file, "attachment");

    lingtai::desktop::RuntimeOptions options;
    options.offscreen_mode = true;
    options.ui_test_mode = true;
    auto host = std::make_unique<lingtai::desktop::ShellHost>(
        options, [](qint64) {});
    auto &shell = host->primary();
    shell.show_offscreen();
    auto *detail = shell.window().findChild<lingtai::desktop::AgentDetailView *>();
    auto *input = static_cast<Ui::InputField *>(
        shell.window().findChild<QObject *>("lingtai_composer_input"));
    require(detail && input, "the real composer must exist");

    const auto select = [&](const fs::path &project, const char *agent) {
        static_cast<void>(shell.open_project(
            project, fs::path(".lingtai") / agent));
        pump(50);
    };
    const auto text = [&] { return input->getTextWithTags().text; };
    const auto type = [&](const QString &value) {
        input->setTextWithTags({value, {}});
    };

    select(one, "alpha");
    type(QStringLiteral("alpha draft"));
    detail->merge_pending_attachments({file});
    require(detail->pending_attachments().size() == 1, "attachment accepted");

    select(one, "beta");
    require(text().isEmpty() && !detail->has_pending_attachments(),
        "Agent B must start from its own empty draft");
    type(QStringLiteral("beta draft"));

    select(one, "alpha");
    require(text() == QStringLiteral("alpha draft"),
        "A→B→A must restore A's text");
    require(detail->pending_attachments().size() == 1,
        "A→B→A must restore A's attachment through preflight");

    select(one, "beta");
    require(text() == QStringLiteral("beta draft"), "B must restore B's text");

    type(QStringLiteral("beta draft more"));
    select(one, "beta");
    for (auto *timer : shell.window().findChildren<QTimer *>()) {
        if (timer->interval() == 1000) {
            QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        }
    }
    pump(100);
    require(text() == QStringLiteral("beta draft more"),
        "same-Agent reopen and ordinary refresh must keep ongoing typing");

    select(two, "alpha");
    require(text().isEmpty() && !detail->has_pending_attachments(),
        "a same-named Agent in another project must not see the draft");
    type(QStringLiteral("two alpha"));
    select(one, "alpha");
    require(text() == QStringLiteral("alpha draft"),
        "project one's alpha keeps its own draft");
    select(two, "alpha");
    require(text() == QStringLiteral("two alpha"),
        "project two's alpha keeps its own draft");

    input->clear();
    select(one, "beta");
    select(two, "alpha");
    require(text().isEmpty(), "a user-cleared draft must not resurrect");

    select(one, "alpha");
    fs::remove(file);
    emit detail->send_message_requested(text());
    pump(50);
    require(text() == QStringLiteral("alpha draft"),
        "a failed send must preserve the draft");
    detail->clear_pending_attachments();
    emit detail->send_message_requested(text());
    pump(50);
    require(text().isEmpty(), "a queued send clears the live composer");
    select(one, "beta");
    require(text() == QStringLiteral("beta draft more"),
        "a send must not discard another Agent's draft");
    select(one, "alpha");
    require(text().isEmpty(), "a sent draft must not resurrect");

    // A cross-project reopen restores through preflight and keeps the
    // rejection notice of an attachment that vanished while stashed visible.
    const auto gone = root / "gone.txt";
    write_file(gone, "attachment");
    type(QStringLiteral("alpha kept"));
    detail->merge_pending_attachments({gone});
    select(two, "alpha");
    fs::remove(gone);
    select(one, "alpha");
    auto *status = shell.window().findChild<QLabel *>("lingtai_composer_status");
    require(text() == QStringLiteral("alpha kept")
            && !detail->has_pending_attachments(),
        "restore keeps the text and drops the vanished attachment");
    require(status && status->text()
            == QStringLiteral("gone.txt is no longer available."),
        "the vanished attachment's rejection notice must stay visible");

    host.reset();
    fs::remove_all(root);
    return 0;
}
