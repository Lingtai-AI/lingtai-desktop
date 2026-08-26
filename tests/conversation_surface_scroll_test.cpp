#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"
#include "styles/style_widgets.h"
#include "ui/style/style_core_scale.h"
#include "ui/style/style_core_palette.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lingtai::desktop {
namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void settle() {
    QCoreApplication::sendPostedEvents(nullptr, 0);
    QCoreApplication::processEvents();
}

std::vector<DirectConversationMessage> messages(int count) {
    auto result = std::vector<DirectConversationMessage>();
    result.reserve(static_cast<std::size_t>(count));
    for (auto index = 0; index != count; ++index) {
        result.push_back({
            .id = "scroll-" + std::to_string(index),
            .outgoing = (index % 2) != 0,
            .timestamp = "2026-08-25T12:"
                + std::to_string(10 + index % 40) + ":00Z",
            .subject = std::string(),
            .text = "trackpad regression row " + std::to_string(index)
                + " with enough text to produce stable multi-line overflow "
                  "inside the production conversation surface",
            .attachments = {},
            .mailbox_folder = (index % 2) != 0 ? "sent" : "inbox",
        });
    }
    return result;
}

void append_row(
        std::vector<DirectConversationMessage> &rows,
        const std::string &id,
        const std::string &text) {
    rows.push_back({
        .id = id,
        .outgoing = true,
        .timestamp = "2026-08-26T12:00:00Z",
        .subject = std::string(),
        .text = text,
        .attachments = {},
        .mailbox_folder = "sent",
    });
}

void prepare_surface(
        ConversationSurface &surface,
        const std::vector<DirectConversationMessage> &rows,
        const ConversationPresentationRevision &revision = {}) {
    surface.setAttribute(Qt::WA_DontShowOnScreen);
    surface.resize(420, 180);
    surface.show();
    settle();
    surface.set_conversation(
        QStringLiteral("Agent"), rows, {}, {}, revision);
    settle();
    auto *bar = surface.verticalScrollBar();
    require(bar->maximum() > 0,
        "the conversation fixture must overflow its viewport");
    bar->setValue(bar->maximum());
    settle();
}

struct WheelDelivery {
    bool delivered = false;
    bool accepted = false;
};

WheelDelivery send_wheel(
        ConversationSurface &surface,
        QPoint pixel_delta,
        QPoint angle_delta,
        Qt::ScrollPhase phase,
        bool start_accepted = true) {
    auto *target = surface.viewport();
    const auto local = target->rect().center();
    const auto global = target->mapToGlobal(local);
    QWheelEvent event(
        QPointF(local),
        QPointF(global),
        pixel_delta,
        angle_delta,
        Qt::NoButton,
        Qt::NoModifier,
        phase,
        false,
        Qt::MouseEventNotSynthesized);
    event.setAccepted(start_accepted);
    const auto delivered = QCoreApplication::sendEvent(target, &event);
    return {delivered, event.isAccepted()};
}

void verify_zero_delta_begin_cancels_queued_pin() {
    ConversationSurface surface;
    const auto rows = messages(30);
    prepare_surface(surface, rows);
    auto *bar = surface.verticalScrollBar();

    surface.scroll_to_bottom();
    const auto pinned_at = bar->value();
    const auto begin = send_wheel(
        surface, QPoint(), QPoint(), Qt::ScrollBegin);
    require(begin.delivered,
        "the zero-delta ScrollBegin must be delivered to the viewport");
    require(bar->value() == pinned_at,
        "a zero-delta ScrollBegin must leave the integer value unchanged");

    const auto grown_maximum = bar->maximum() + 47;
    bar->setMaximum(grown_maximum);
    settle();
    require(bar->maximum() == grown_maximum,
        "the deterministic deferred-layout probe must grow the maximum");
    require(bar->value() == pinned_at,
        "ScrollBegin must cancel the queued bottom pin before native handling");

    const auto before_end = bar->value();
    send_wheel(surface, QPoint(), QPoint(), Qt::ScrollEnd);
    settle();
    require(bar->value() == before_end,
        "ScrollEnd must end ownership without moving the scrollbar");
}

void verify_tiny_update_blocks_incremental_follow() {
    ConversationSurface surface;
    auto rows = messages(24);
    const auto initial = ConversationPresentationRevision{
        .history = 100,
        .reactions = 200,
        .session_events = 300,
    };
    prepare_surface(surface, rows, initial);
    auto *bar = surface.verticalScrollBar();
    const auto prior_value = bar->value();
    const auto prior_maximum = bar->maximum();

    const auto begin = send_wheel(
        surface, QPoint(), QPoint(), Qt::ScrollBegin);
    const auto update = send_wheel(
        surface, QPoint(0, -1), QPoint(), Qt::ScrollUpdate);
    require(begin.delivered && update.delivered,
        "native QTextEdit handling must receive the phase-bearing gesture");
    require(bar->value() == prior_value,
        "the 1px update at the bottom must leave the integer value unchanged");

    append_row(rows, "tiny-update-append",
        "an incremental refresh while the trackpad gesture remains active");
    surface.set_conversation(
        QStringLiteral("Agent"), rows, {}, {},
        ConversationPresentationRevision{
            .history = 101,
            .append_from_history = 100,
            .append_from = 24,
            .reactions = 200,
            .session_events = 300,
        });
    settle();
    require(bar->maximum() > prior_maximum,
        "the incremental refresh must grow the scrollbar maximum");
    require(bar->value() == prior_value,
        "an active tiny trackpad update must block incremental bottom-follow");

    const auto before_end = bar->value();
    send_wheel(surface, QPoint(), QPoint(), Qt::ScrollEnd);
    settle();
    require(bar->value() == before_end,
        "ScrollEnd must not jump after an incremental refresh");
}

void verify_momentum_blocks_rebuild_follow() {
    ConversationSurface surface;
    auto rows = messages(26);
    prepare_surface(surface, rows);
    auto *bar = surface.verticalScrollBar();
    const auto prior_value = bar->value();
    const auto prior_maximum = bar->maximum();

    const auto momentum = send_wheel(
        surface, QPoint(), QPoint(), Qt::ScrollMomentum);
    require(momentum.delivered,
        "native QTextEdit handling must receive ScrollMomentum");
    rows.front().text += " changed to force a complete rebuild";
    append_row(rows, "momentum-rebuild",
        "a full rebuild refresh while momentum owns the viewport");
    surface.set_conversation(QStringLiteral("Agent"), rows);
    settle();
    require(bar->maximum() > prior_maximum,
        "the full rebuild must grow the scrollbar maximum");
    require(bar->value() == prior_value,
        "ScrollMomentum must block full-rebuild bottom-follow until end");

    const auto before_end = bar->value();
    send_wheel(surface, QPoint(), QPoint(), Qt::ScrollEnd);
    settle();
    require(bar->value() == before_end,
        "ScrollEnd after momentum must not move the scrollbar");
}

void verify_normal_bottom_follow() {
    ConversationSurface surface;
    auto rows = messages(24);
    const auto initial = ConversationPresentationRevision{
        .history = 400,
        .reactions = 500,
        .session_events = 600,
    };
    prepare_surface(surface, rows, initial);
    auto *bar = surface.verticalScrollBar();
    const auto prior_maximum = bar->maximum();

    append_row(rows, "ordinary-bottom-append",
        "ordinary append after no gesture must retain bottom follow");
    surface.set_conversation(
        QStringLiteral("Agent"), rows, {}, {},
        ConversationPresentationRevision{
            .history = 401,
            .append_from_history = 400,
            .append_from = 24,
            .reactions = 500,
            .session_events = 600,
        });
    settle();
    require(bar->maximum() > prior_maximum,
        "the ordinary append must grow the scrollbar maximum");
    require(bar->value() == bar->maximum(),
        "without an active gesture, delayed layout stabilization must finish at bottom");
}

void verify_manual_non_bottom_preserved() {
    ConversationSurface surface;
    auto rows = messages(26);
    prepare_surface(surface, rows);
    auto *bar = surface.verticalScrollBar();
    bar->setValue(std::max(bar->minimum(), bar->maximum() / 2));
    const auto prior_value = bar->value();
    const auto prior_maximum = bar->maximum();
    require(prior_value < prior_maximum,
        "the manual-position fixture must be away from bottom");

    append_row(rows, "manual-position-rebuild",
        "ordinary rebuild must preserve a reader who moved away from bottom");
    surface.set_conversation(QStringLiteral("Agent"), rows);
    settle();
    require(bar->maximum() > prior_maximum,
        "the manual-position rebuild must grow the scrollbar maximum");
    require(bar->value() == prior_value,
        "a manual non-bottom position must remain stable across rebuild");
}

void verify_native_wheel_delegation() {
    ConversationSurface surface;
    const auto rows = messages(30);
    prepare_surface(surface, rows);
    auto *bar = surface.verticalScrollBar();
    bar->setValue(bar->maximum() / 2);
    const auto before = bar->value();

    const auto wheel = send_wheel(
        surface, QPoint(), QPoint(0, 120), Qt::NoScrollPhase, false);
    require(wheel.delivered && wheel.accepted,
        "an ordinary wheel event must traverse native QTextEdit handling");
    require(bar->value() < before,
        "native QTextEdit wheel handling must move the viewport; custom gesture "
        "observation must not consume the event");
}

int run_scroll_test(int argc, char **argv) {
    try {
        QApplication application(argc, argv);
        style::internal::init_palette(style::kScaleDefault);
        style::internal::init_style_widgets(style::kScaleDefault);

        verify_zero_delta_begin_cancels_queued_pin();
        verify_tiny_update_blocks_incremental_follow();
        verify_momentum_blocks_rebuild_follow();
        verify_normal_bottom_follow();
        verify_manual_non_bottom_preserved();
        verify_native_wheel_delegation();
        std::cout << "conversation surface gesture scroll: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "conversation surface gesture scroll: " << error.what()
                  << '\n';
        return 1;
    }
}

} // namespace
} // namespace lingtai::desktop

int main(int argc, char **argv) {
    return lingtai::desktop::run_scroll_test(argc, argv);
}
