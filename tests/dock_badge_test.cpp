// The Dock badge boundary: ShellHost publishes its real unread aggregate to an
// injected sink (never the OS Dock) with initial/teardown zero, no repeats,
// and numeric saturation instead of the status item's 99+ cap.
#include "native_shell.h"
#include "runtime_options.h"
#include "shell_host.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "dock badge: " << message << '\n';
        std::exit(1);
    }
}

template <typename Predicate>
bool wait_for_event_loop(Predicate predicate, int timeout_ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout_ms) {
        if (predicate()) return true;
        QEventLoop iteration;
        QTimer::singleShot(5, &iteration, &QEventLoop::quit);
        iteration.exec(QEventLoop::AllEvents);
    }
    return predicate();
}

void write_file(const fs::path &path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    auto stream = std::ofstream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture file must be written");
}

std::string inbound(std::string_view message, std::string_view timestamp) {
    return std::string(R"({"from":"alpha","to":["human"],"subject":"s",)")
        + R"("message":")" + std::string(message)
        + R"(","type":"normal","mode":"peer","attachments":[],)"
        + R"("received_at":")" + std::string(timestamp) + R"("})";
}

} // namespace

int main(int argc, char **argv) {
    require(argc == 2, "usage: lingtai_dock_badge_test <fixture-root>");
    QApplication app(argc, argv);
    using lingtai::desktop::dock_badge_number;
    require(dock_badge_number(100) == 100 && dock_badge_number(101) == 101
            && dock_badge_number(std::numeric_limits<std::size_t>::max())
                == std::numeric_limits<qint64>::max(),
        "the badge must keep >99 counts numeric and saturate safely");

    const auto project = fs::path(argv[1]) / "project";
    fs::remove_all(project);
    const auto inbox = project / ".lingtai/human/mailbox/inbox";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"human-id","agent_name":"Human",)"
        R"("address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"alpha-id","agent_name":"alpha",)"
        R"("address":"alpha","state":"active"})");
    write_file(inbox / "20260902T120000-alpha-seed/message.json",
        inbound("alpha seed", "2026-09-02T12:00:00Z"));

    auto badges = std::vector<qint64>();
    lingtai::desktop::RuntimeOptions options;
    options.offscreen_mode = true;
    options.ui_test_mode = true;
    auto host = std::make_unique<lingtai::desktop::ShellHost>(
        options, [&badges](qint64 number) { badges.push_back(number); });
    require(badges == std::vector<qint64>{0},
        "construction must publish exactly one initial zero");

    auto &shell = host->primary();
    shell.show_offscreen();
    static_cast<void>(shell.open_project(project, fs::path(".lingtai/alpha")));
    auto *surface = shell.window().findChild<QTextEdit *>(
        "lingtai_selected_agent_conversation");
    require(surface && wait_for_event_loop([&] {
        return surface->toPlainText().contains("alpha seed");
    }, 4000), "the seed history must establish the session baseline");
    require(badges == std::vector<qint64>{0},
        "the zero baseline must publish no redundant badge");

    write_file(inbox / "20260902T120100-alpha-one/message.json",
        inbound("alpha one", "2026-09-02T12:01:00Z"));
    write_file(inbox / "20260902T120101-alpha-two/message.json",
        inbound("alpha two", "2026-09-02T12:01:01Z"));
    for (auto *timer : shell.window().findChildren<QTimer *>()) {
        if (timer->interval() == 1000) {
            QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
        }
    }
    require(wait_for_event_loop([&] { return host->unread_total() == 2; }, 5000),
        "two inbound rows in an inactive window must aggregate to two");
    require(badges.back() == 2
            && std::ranges::adjacent_find(badges) == badges.end(),
        "the sink must receive the real aggregate without repeats");

    host.reset();
    require(badges.size() >= 3 && badges.back() == 0
            && badges[badges.size() - 2] == 2,
        "host teardown must force the badge back to zero");
    fs::remove_all(project);
    return 0;
}
