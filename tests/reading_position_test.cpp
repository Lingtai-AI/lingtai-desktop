// Real shell routing and real mailbox fixtures; assert content and viewport Y.
#include "native_shell.h"
#include "runtime_options.h"
#include "shell_host.h"
#include "ui/conversation_surface.h"
#include "ui/widgets/rp_window.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QMetaObject>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
namespace lingtai::desktop {
struct ReadingPositionTestAccess {
    static void complete(NativeShell &shell, std::vector<ConversationSessionEntry> events) {
        shell.apply_session_events(shell.session_events_load_generation_, std::move(events));
    }
};
}
namespace fs = std::filesystem;
void require(bool value, const char *message) {
    if (!value) { std::cerr << "reading position: " << message << '\n'; std::exit(1); }
}
void pump(int ms = 100) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < ms) {
        QEventLoop loop; QTimer::singleShot(5, &loop, &QEventLoop::quit); loop.exec();
    }
}
void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path()); std::ofstream out(path); out << text;
    require(out.good(), "fixture write");
}
void append(const fs::path &root, const char *agent, int from, int count) {
    for (int i = from; i < from + count; ++i) {
        const auto number = QStringLiteral("%1").arg(i, 4, 10, QChar('0')).toStdString();
        write(root / ".lingtai/human/mailbox/inbox" / (number + "-" + agent) / "message.json",
            std::string("{\"from\":\"") + agent + "\",\"to\":[\"human\"],\"subject\":\"s\",\"message\":\""
            + agent + " unique body " + number + " with enough text to identify its exact reading line.\","
            "\"type\":\"normal\",\"mode\":\"peer\",\"attachments\":[],\"received_at\":\"2026-09-02T12:00:00Z\"}");
    }
}
void project(const fs::path &root) {
    write(root / ".lingtai/human/.agent.json", "{\"agent_id\":\"human-id\",\"agent_name\":\"Human\",\"address\":\"human\",\"state\":\"active\"}");
    for (const auto agent : {"alpha", "beta", "short", "empty"}) {
        write(root / ".lingtai" / agent / ".agent.json", std::string("{\"admin\":{},\"agent_id\":\"") + agent
            + "-id\",\"agent_name\":\"Same name\",\"address\":\"" + agent + "\",\"state\":\"active\"}");
    }
    append(root, "alpha", 0, 260); append(root, "beta", 0, 110); append(root, "short", 0, 1);
}
int main(int argc, char **argv) {
    require(argc == 2, "fixture root required"); QApplication app(argc, argv);
    const auto root = fs::path(argv[1]); fs::remove_all(root);
    const auto one = root / "one", two = root / "two"; project(one); project(two);
    lingtai::desktop::RuntimeOptions options; options.offscreen_mode = true; options.ui_test_mode = true;
    auto host = std::make_unique<lingtai::desktop::ShellHost>(options, [](qint64) {});
    auto &shell = host->primary(); shell.show_offscreen();
    auto *surface = shell.window().findChild<lingtai::desktop::ConversationSurface *>("lingtai_selected_agent_conversation");
    require(surface, "real surface exists");
    const auto select = [&](const fs::path &p, const char *agent, bool wait = true) {
        static_cast<void>(shell.open_project(p, fs::path(".lingtai") / agent));
        if (wait) pump(180);
    };
    const auto refresh = [&] {
        for (auto *timer : shell.window().findChildren<QTimer *>()) if (timer->interval() == 1000)
            QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        pump(200);
    };
    const auto locate = [&](const QString &text) {
        auto cursor = surface->document()->find(text);
        // Worker completion is asynchronous: wait for content, never guess a
        // machine-speed-dependent delivery delay. A lost anchor still fails.
        QElapsedTimer ready; ready.start();
        while (cursor.isNull() && ready.elapsed() < 3000) {
            pump(10);
            cursor = surface->document()->find(text);
        }
        if (cursor.isNull()) std::cerr << "missing text: " << text.toStdString()
            << " document starts: " << surface->toPlainText().left(200).toStdString() << '\n';
        require(!cursor.isNull(), "reading text is materialized");
        cursor.setPosition(cursor.selectionStart()); return cursor;
    };
    const auto place = [&](const QString &text) {
        auto cursor = locate(text); auto *bar = surface->verticalScrollBar();
        bar->setValue(bar->value() + surface->cursorRect(cursor).y() - 85); pump();
        return surface->cursorRect(locate(text)).y();
    };
    const auto check = [&](const QString &text, int y) {
        pump(); const auto actual = surface->cursorRect(locate(text)).y();
        if (std::abs(actual - y) > 2) std::cerr << "expected Y=" << y << " actual=" << actual << '\n';
        require(std::abs(actual - y) <= 2, "same text must retain viewport Y");
    };
    select(one, "alpha"); pump(500);
    const auto middle = QStringLiteral("alpha unique body 0200"); const auto y = place(middle);
    for (const auto agent : {"beta", "short", "empty"}) {
        select(one, agent); select(one, "alpha"); check(middle, y);
    }
    select(one, "beta"); const auto by = place(QStringLiteral("beta unique body 0050"));
    select(one, "alpha"); check(middle, y); select(one, "beta"); check(QStringLiteral("beta unique body 0050"), by);
    append(one, "alpha", 260, 130); refresh(); select(one, "alpha"); check(middle, y);
    // Reveal history older than the ordinary 100-message tail.
    surface->verticalScrollBar()->setValue(0);
    QKeyEvent reveal(QEvent::KeyPress, Qt::Key_U, Qt::ControlModifier); QApplication::sendEvent(surface, &reveal); pump();
    const auto old = QStringLiteral("alpha unique body 0120"); const auto old_y = place(old);
    select(one, "empty"); append(one, "alpha", 390, 120); refresh(); select(one, "alpha"); check(old, old_y);
    select(one, "alpha"); refresh(); check(old, old_y);
    select(two, "alpha"); const auto two_y = place(middle);
    select(one, "alpha"); check(old, old_y); select(two, "alpha"); check(middle, two_y);
    // Queue multiple route changes before deferred layout restoration drains.
    select(one, "alpha", false); select(one, "beta", false); select(one, "alpha"); check(old, old_y);
    surface->scroll_to_bottom(); pump(); select(one, "short"); select(one, "alpha");
    require(surface->verticalScrollBar()->value() == surface->verticalScrollBar()->maximum(), "A retains bottom-follow intent");
    // Complete details through the real shell callback after restoring an older
    // window. No worker timing or external session log is needed.
    select(one, "alpha");
    surface->cycle_verbose_level(); pump();
    surface->verticalScrollBar()->setValue(0);
    for (int i = 0; i < 15; ++i) QApplication::sendEvent(surface, &reveal);
    pump();
    const auto ancient = QStringLiteral("alpha unique body 0025");
    const auto ancient_y = place(ancient);
    select(one, "beta"); select(one, "alpha"); check(ancient, ancient_y);
    using lingtai::desktop::ConversationSessionEntry;
    std::vector<ConversationSessionEntry> events;
    for (int i = 0; i < 2; ++i) {
        ConversationSessionEntry event;
        event.timestamp = "2026-09-02T12:00:00Z"; event.type = "thinking";
        event.api_call_id = std::to_string(i);
        event.body = "detail-marker-" + std::to_string(i);
        for (int line = 0; line < 30; ++line)
            event.body += "\nread detail " + std::to_string(i) + " line " + std::to_string(line);
        events.push_back(event);
    }
    lingtai::desktop::ReadingPositionTestAccess::complete(shell, events); pump();
    if (!std::getenv("READING_EVENT_ONLY")) {
        require(surface->toPlainText().contains(ancient), "details completion retains revealed history");
        check(ancient, ancient_y);
    }
    // Read inside the SECOND detail frame, with an earlier event above it.
    const auto detail = QStringLiteral("read detail 1 line 3");
    auto detail_cursor = locate(detail);
    surface->verticalScrollBar()->setValue(surface->verticalScrollBar()->value()
        + surface->cursorRect(detail_cursor).y()); pump();
    const auto detail_y = surface->cursorRect(locate(detail)).y();
    select(one, "beta"); select(one, "alpha");
    lingtai::desktop::ReadingPositionTestAccess::complete(shell, events); pump();
    check(detail, detail_y);
    auto inserted = events.front(); inserted.api_call_id = "new";
    inserted.body = "new event above current detail\nmore inserted content";
    events.insert(events.begin(), inserted);
    lingtai::desktop::ReadingPositionTestAccess::complete(shell, events); pump();
    check(detail, detail_y);
    // Thinking -> Extended: A/B share a group, then X changes only B's
    // preceding group decoration. B's body line identity must stay fixed.
    surface->cycle_verbose_level(); pump();
    events.erase(events.begin());
    events[0].api_call_id = "same-group";
    events[1].api_call_id = "same-group";
    lingtai::desktop::ReadingPositionTestAccess::complete(shell, events); pump();
    const auto extended_detail = QStringLiteral("read detail 1 line 15");
    auto extended_cursor = locate(extended_detail);
    surface->verticalScrollBar()->setValue(surface->verticalScrollBar()->value()
        + surface->cursorRect(extended_cursor).y()); pump();
    const auto extended_y = surface->cursorRect(locate(extended_detail)).y();
    require(surface->verticalScrollBar()->value() < surface->verticalScrollBar()->maximum(),
        "Extended regression reads a nonbottom body line");
    auto different_group = events.front();
    different_group.api_call_id = "different-group";
    different_group.body = "inserted X before B\nsecond X line";
    events.insert(events.begin() + 1, different_group);
    lingtai::desktop::ReadingPositionTestAccess::complete(shell, events); pump();
    const auto extended_actual = surface->cursorRect(locate(extended_detail)).y();
    std::cout << "Extended B line15 expected Y=" << extended_y
        << " actual=" << extended_actual << std::endl;
    require(std::abs(extended_actual - extended_y) <= 2,
        "Extended same-group B body line retains viewport Y after different-group X insertion");
    std::cout << "details completion and event anchor regressions passed\n";
    host.reset(); fs::remove_all(root); return 0;
}
