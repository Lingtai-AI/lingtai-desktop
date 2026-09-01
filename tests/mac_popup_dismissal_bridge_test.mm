#include "mac_popup_dismissal_bridge.h"
#include "native_shell.h"

#include "ui/integration.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/rp_window.h"

#include <rpl/rpl.h>

#import <AppKit/AppKit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QFocusEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using lingtai::desktop::MacPopupDismissalBridge;
using lingtai::desktop::NativeWindowIdentity;
using lingtai::desktop::ShouldDismissMacPopup;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool wait_until(Fn<bool()> predicate) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

template <typename Widget>
Widget *required_ui_child(QWidget &root, const char *object_name) {
    auto *object = root.findChild<QObject *>(object_name);
    auto *result = dynamic_cast<Widget *>(object);
    require(result != nullptr, std::string("missing child: ") + object_name);
    return result;
}

class ScopedObjectEventFilter final {
public:
    ScopedObjectEventFilter(QObject &object, QObject &filter)
    : object_(&object)
    , filter_(filter) {
        object.installEventFilter(&filter_);
    }

    ~ScopedObjectEventFilter() {
        if (object_) {
            object_->removeEventFilter(&filter_);
        }
    }

private:
    QPointer<QObject> object_;
    QObject &filter_;
};

class QtEventSpy final : public QObject {
public:
    Ui::PopupMenu *popup = nullptr;
    int mouse_press_count = 0;
    int mouse_release_count = 0;
    int window_activate_count = 0;
    bool popup_hidden_on_press = false;

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        (void)object;
        if (event->type() == QEvent::MouseButtonPress) {
            ++mouse_press_count;
            popup_hidden_on_press = popup && popup->isHidden();
        } else if (event->type() == QEvent::MouseButtonRelease) {
            ++mouse_release_count;
        } else if (event->type() == QEvent::WindowActivate) {
            ++window_activate_count;
        }
        return false;
    }
};

class HideEventSpy final : public QObject {
public:
    int hide_count = 0;

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        (void)object;
        if (event->type() == QEvent::Hide) {
            ++hide_count;
        }
        return false;
    }
};

class ForceHideRequestCounter final {
public:
    ForceHideRequestCounter() {
        Ui::Integration::Instance().forcePopupMenuHideRequests(
        ) | rpl::on_next([this] {
            ++count;
        }, lifetime_);
    }

    int count = 0;

private:
    rpl::lifetime lifetime_;
};

struct ShellContext final {
    ShellContext() {
        shell.show();
        QCoreApplication::processEvents();
        composer = required_ui_child<Ui::InputField>(
            shell.window(), "lingtai_composer_input");
    }

    bool activate() {
        auto &window = shell.window();
        require(window.internalWinId() != 0,
            "shell must already own a Cocoa view");
        auto *view = reinterpret_cast<NSView *>(window.internalWinId());
        auto *native = view.window;
        require(native != nil, "shell must have a Cocoa NSWindow");

        [NSApp activateIgnoringOtherApps:YES];
        [native makeKeyAndOrderFront:nil];
        window.activateWindow();
        return wait_until([&] {
            return NSApp.isActive
                && native.isKeyWindow
                && window.isActiveWindow();
        });
    }

    lingtai::desktop::NativeShell shell;
    Ui::InputField *composer = nullptr;
};

class TestPopupMenu final : public Ui::PopupMenu {
public:
    using Ui::PopupMenu::PopupMenu;

    void deliver_key_press(QKeyEvent &event) {
        keyPressEvent(&event);
    }

    void deliver_focus_out(QFocusEvent &event) {
        focusOutEvent(&event);
    }
};

std::unique_ptr<TestPopupMenu> show_popup(
        ShellContext &context,
        Fn<void()> action = [] {}) {
    auto result = std::make_unique<TestPopupMenu>(context.composer);
    result->deleteOnHide(false);
    result->addAction(QStringLiteral("Composer action"), std::move(action));
    constexpr auto kSetupAttempts = 3;
    for (auto attempt = 0; attempt != kSetupAttempts; ++attempt) {
        if (!context.activate()) {
            continue;
        }
        result->popup(context.composer->mapToGlobal(QPoint(8, 8)));
        QCoreApplication::processEvents();
        if (result->isVisible()) {
            break;
        }
    }
    require(result->isVisible(), "composer popup must be visible");
    require(result->internalWinId() != 0,
        "composer popup must already own a Cocoa view");
    return result;
}

NSWindow *native_window(QWidget &widget) {
    require(widget.internalWinId() != 0,
        "widget must already own a Cocoa view");
    auto *view = reinterpret_cast<NSView *>(widget.internalWinId());
    require(view.window != nil, "widget must have a Cocoa NSWindow");
    return view.window;
}

NSPoint location_in_window(QWidget &target, QWidget &window_widget) {
    auto *view = reinterpret_cast<NSView *>(window_widget.internalWinId());
    const auto point = target.mapTo(&window_widget, target.rect().center());
    const auto native_point = NSMakePoint(
        point.x(),
        view.isFlipped ? point.y() : view.bounds.size.height - point.y());
    return [view convertPoint:native_point toView:nil];
}

NSEvent *mouse_event(
        NSEventType type,
        NSWindow *window,
        NSPoint location,
        NSInteger event_number) {
    const auto is_down = type == NSEventTypeLeftMouseDown
        || type == NSEventTypeRightMouseDown
        || type == NSEventTypeOtherMouseDown;
    return [NSEvent mouseEventWithType:type
        location:location
        modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:(window ? window.windowNumber : 0)
        context:nil
        eventNumber:event_number
        clickCount:1
        pressure:(is_down ? 1.0 : 0.0)];
}

void send_qt_window_click(NSWindow *window, NSPoint location) {
    auto *down = mouse_event(
        NSEventTypeLeftMouseDown, window, location, 101);
    auto *up = mouse_event(
        NSEventTypeLeftMouseUp, window, location, 102);
    [NSApp sendEvent:down];
    [NSApp sendEvent:up];
    QCoreApplication::processEvents();
}

MacPopupDismissalBridge &required_bridge(QApplication &application) {
    auto *result = static_cast<MacPopupDismissalBridge *>(nullptr);
    auto count = 0;
    for (auto *child : application.children()) {
        if (auto *candidate = dynamic_cast<MacPopupDismissalBridge *>(child)) {
            result = candidate;
            ++count;
        }
    }
    require(count == 1,
        "QApplication must own exactly one popup dismissal bridge");
    return *result;
}

bool run_filter(
        MacPopupDismissalBridge &bridge,
        NSEvent *event,
        qintptr *result = nullptr) {
    return bridge.nativeEventFilter(
        QByteArrayLiteral("mac_generic_NSEvent"), event, result);
}

void send_direct_qt_click(QWidget &target) {
    const auto local = QPointF(target.rect().center());
    const auto global = QPointF(target.mapToGlobal(target.rect().center()));
    auto down = QMouseEvent(
        QEvent::MouseButtonPress,
        local,
        local,
        global,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
        Qt::MouseEventNotSynthesized);
    auto up = QMouseEvent(
        QEvent::MouseButtonRelease,
        local,
        local,
        global,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier,
        Qt::MouseEventNotSynthesized);
    QApplication::sendEvent(&target, &down);
    QApplication::sendEvent(&target, &up);
    QCoreApplication::processEvents();
}

struct VisibleMenuTree final {
    explicit VisibleMenuTree(ShellContext &context) {
        root = std::make_unique<Ui::PopupMenu>(context.composer);
        root->deleteOnHide(false);
        auto owned_submenu = std::make_unique<Ui::PopupMenu>(context.composer);
        submenu = owned_submenu.get();
        submenu_action = submenu->addAction(
            QStringLiteral("Submenu action"), [] {});
        parent_action = root->addAction(
            QStringLiteral("Submenu"), std::move(owned_submenu));
        root->popup(context.composer->mapToGlobal(QPoint(8, 8)));
        QCoreApplication::processEvents();
        auto *item = root->menu()->itemForAction(parent_action);
        require(item != nullptr, "root submenu action item must exist");
        item->setClicked(Ui::Menu::TriggeredSource::Mouse);
        QCoreApplication::processEvents();
        require(root->isVisible() && submenu->isVisible(),
            "root and submenu must both be visible");
    }

    ~VisibleMenuTree() {
        if (root) {
            root->hideMenu(true);
        }
    }

    std::unique_ptr<Ui::PopupMenu> root;
    Ui::PopupMenu *submenu = nullptr;
    QAction *parent_action = nullptr;
    QAction *submenu_action = nullptr;
};

void verify_identity_predicate() {
    const auto first = reinterpret_cast<NativeWindowIdentity>(0x1000);
    const auto second = reinterpret_cast<NativeWindowIdentity>(0x2000);
    const auto empty = std::array<NativeWindowIdentity, 0>();
    const auto one = std::array{ first };
    const auto duplicated = std::array{ first, first };

    require(!ShouldDismissMacPopup(first, empty),
        "empty popup set must not dismiss");
    require(!ShouldDismissMacPopup(first, one),
        "inside popup recipient must not dismiss");
    require(ShouldDismissMacPopup(second, one),
        "outside recipient must dismiss");
    require(ShouldDismissMacPopup(nullptr, one),
        "nil recipient with a popup must dismiss");
    require(!ShouldDismissMacPopup(first, duplicated),
        "deduplicated-equivalent inside set must not dismiss");
    require(ShouldDismissMacPopup(second, duplicated),
        "deduplicated-equivalent outside set must dismiss once");
}

} // namespace

@interface LingTaiNativeControlProbe : NSObject {
@public
    int actionCount;
    bool popupWasHidden;
    Ui::PopupMenu *popup;
}
- (void)handleControl:(id)sender;
@end

@implementation LingTaiNativeControlProbe
- (void)handleControl:(id)sender {
    (void)sender;
    ++actionCount;
    popupWasHidden = popup->isHidden();
}
@end

namespace {

class ScopedButtonAction final {
public:
    ScopedButtonAction(NSButton *button, id target, SEL action)
    : button_(button)
    , original_target_(button.target)
    , original_action_(button.action) {
        button_.target = target;
        button_.action = action;
    }

    ~ScopedButtonAction() {
        button_.target = original_target_;
        button_.action = original_action_;
    }

private:
    NSButton *button_;
    id original_target_;
    SEL original_action_;
};

void send_native_button_click(NSButton *button, NSWindow *window) {
    const auto center = NSMakePoint(
        NSMidX(button.frame), NSMidY(button.frame));
    const auto location = [button.superview convertPoint:center toView:nil];
    auto *up = mouse_event(
        NSEventTypeLeftMouseUp, window, location, 202);
    auto *down = mouse_event(
        NSEventTypeLeftMouseDown, window, location, 201);
    [NSApp postEvent:up atStart:YES];
    [NSApp sendEvent:down];
    QCoreApplication::processEvents();
}

void verify_native_traffic_light(
        QApplication &application,
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto popup = show_popup(context);
    auto &host = context.shell.window();
    auto *host_window = native_window(host);
    auto *close_button = [host_window standardWindowButton:NSWindowCloseButton];
    require(close_button != nil && close_button.superview != nil,
        "Desktop shell must expose the native close traffic light");

    auto *probe = [[LingTaiNativeControlProbe alloc] init];
    probe->actionCount = 0;
    probe->popupWasHidden = false;
    probe->popup = popup.get();
    {
        const auto restore_action = ScopedButtonAction(
            close_button, probe, @selector(handleControl:));
        auto qt_spy = QtEventSpy();
        const auto remove_qt_spy = ScopedObjectEventFilter(
            application, qt_spy);
        const auto before = requests.count;
        send_native_button_click(close_button, host_window);

        require(probe->actionCount == 1,
            "native traffic-light target action must run exactly once");
        require(probe->popupWasHidden,
            "popup must be hidden before the traffic-light target action");
        require(qt_spy.mouse_press_count == 0,
            "native traffic-light dispatch must not translate to a Qt mouse press");
        require(requests.count == before + 1,
            "native traffic-light press must fire one force-hide request");
    }
    [probe release];
}

void verify_qt_content_and_native_boundary(
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto &host = context.shell.window();
    auto target = QPushButton(QStringLiteral("Harmless target"), host.body());
    target.setGeometry(320, 240, 160, 40);
    target.show();
    target.raise();
    QCoreApplication::processEvents();

    auto popup = show_popup(context);
    auto target_spy = QtEventSpy();
    target_spy.popup = popup.get();
    const auto remove_target_spy = ScopedObjectEventFilter(target, target_spy);
    const auto before_qt = requests.count;
    send_direct_qt_click(target);
    require(requests.count == before_qt,
        "direct Qt-synthesized outside press must not cross the native filter");
    require(popup->isVisible(),
        "direct Qt-synthesized outside press must leave popup visible");
    require(target_spy.mouse_press_count == 1
            && target_spy.mouse_release_count == 1,
        "direct Qt target must receive one down/up pair");

    target_spy.mouse_press_count = 0;
    target_spy.mouse_release_count = 0;
    target_spy.popup_hidden_on_press = false;
    const auto before_native = requests.count;
    auto *host_window = native_window(host);
    send_qt_window_click(
        host_window, location_in_window(target, host));
    require(requests.count == before_native + 1,
        "same-window Cocoa outside press must fire once");
    require(popup->isHidden() && target_spy.popup_hidden_on_press,
        "same-window target must observe the popup already hidden");
    require(target_spy.mouse_press_count == 1
            && target_spy.mouse_release_count == 1,
        "same-window Qt target must receive one down/up pair");
}

void verify_second_shell(
        QApplication &application,
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto second = lingtai::desktop::NativeShell();
    second.show();
    QCoreApplication::processEvents();
    required_bridge(application);

    auto &host_a = context.shell.window();
    auto &host_b = second.window();
    auto target = QPushButton(QStringLiteral("Window B target"), host_b.body());
    target.setGeometry(300, 220, 160, 40);
    target.show();
    target.raise();
    QCoreApplication::processEvents();
    auto popup = show_popup(context);

    auto target_spy = QtEventSpy();
    target_spy.popup = popup.get();
    const auto remove_target_spy = ScopedObjectEventFilter(target, target_spy);
    auto activation_spy = QtEventSpy();
    const auto remove_activation_spy = ScopedObjectEventFilter(
        host_a, activation_spy);
    const auto activations_before = activation_spy.window_activate_count;
    const auto requests_before = requests.count;
    send_qt_window_click(
        native_window(host_b), location_in_window(target, host_b));

    require(requests.count == requests_before + 1,
        "second-window press must fire one process-wide request");
    require(target_spy.mouse_press_count == 1
            && target_spy.mouse_release_count == 1
            && target_spy.popup_hidden_on_press,
        "window B target must receive once after the popup hides");
    require(activation_spy.window_activate_count == activations_before,
        "outside press in B must not reactivate window A");
    required_bridge(application);
}

void verify_inside_root(
        MacPopupDismissalBridge &bridge,
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto action_count = 0;
    auto popup = show_popup(context, [&] { ++action_count; });
    const auto before = requests.count;
    auto *event = mouse_event(
        NSEventTypeLeftMouseDown,
        native_window(*popup),
        NSMakePoint(4, 4),
        301);
    require(!run_filter(bridge, event),
        "inside-root event must never be consumed");
    require(requests.count == before && popup->isVisible(),
        "inside-root recipient must not force-dismiss");

    auto *item = popup->menu()->itemForAction(popup->actions().front());
    require(item != nullptr, "root action item must exist");
    item->setClicked(Ui::Menu::TriggeredSource::Mouse);
    QCoreApplication::processEvents();
    require(action_count == 1,
        "existing root press/action behavior must remain intact");
    require(requests.count == before,
        "inside root action must not fire the native request");
}

void verify_inside_submenu(
        MacPopupDismissalBridge &bridge,
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto tree = VisibleMenuTree(context);
    auto action_count = 0;
    QObject::connect(tree.submenu_action, &QAction::triggered, [&] {
        ++action_count;
    });
    auto *item = tree.submenu->menu()->itemForAction(tree.submenu_action);
    require(item != nullptr, "submenu action item must exist");
    item->setPreventClose(true);

    const auto before = requests.count;
    auto *event = mouse_event(
        NSEventTypeLeftMouseDown,
        native_window(*tree.submenu),
        NSMakePoint(4, 4),
        302);
    require(!run_filter(bridge, event),
        "inside-submenu event must never be consumed");
    require(requests.count == before,
        "inside-submenu recipient must not force-dismiss");
    item->setClicked(Ui::Menu::TriggeredSource::Mouse);
    QCoreApplication::processEvents();
    require(action_count == 1
            && tree.root->isVisible()
            && tree.submenu->isVisible(),
        "submenu preventClose traversal/action must remain intact");
}

void verify_tree_hide_and_deferred_delete(
        MacPopupDismissalBridge &bridge,
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto *root = new Ui::PopupMenu(context.composer);
    auto owned_submenu = std::make_unique<Ui::PopupMenu>(context.composer);
    auto *submenu = owned_submenu.get();
    submenu->addAction(QStringLiteral("Submenu action"), [] {});
    auto *parent_action = root->addAction(
        QStringLiteral("Submenu"), std::move(owned_submenu)).get();
    root->popup(context.composer->mapToGlobal(QPoint(8, 8)));
    QCoreApplication::processEvents();
    root->menu()->itemForAction(parent_action)->setClicked(
        Ui::Menu::TriggeredSource::Mouse);
    QCoreApplication::processEvents();
    require(root->isVisible() && submenu->isVisible(),
        "deferred-delete tree must begin fully visible");

    auto root_hide = HideEventSpy();
    auto submenu_hide = HideEventSpy();
    const auto remove_root_spy = ScopedObjectEventFilter(*root, root_hide);
    const auto remove_submenu_spy = ScopedObjectEventFilter(
        *submenu, submenu_hide);
    auto root_guard = QPointer<Ui::PopupMenu>(root);
    auto submenu_guard = QPointer<Ui::PopupMenu>(submenu);
    const auto before = requests.count;
    auto *event = mouse_event(
        NSEventTypeLeftMouseDown,
        native_window(context.shell.window()),
        NSMakePoint(5, 5),
        303);
    require(!run_filter(bridge, event),
        "outside tree event must never be consumed");
    require(requests.count == before + 1,
        "root/submenu outside press must fire one request");
    require(root->isHidden() && submenu->isHidden()
            && root_hide.hide_count == 1
            && submenu_hide.hide_count == 1,
        "root and submenu must each hide exactly once");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    require(root_guard.isNull() && submenu_guard.isNull(),
        "deferred deletion must drain without retaining tree pointers");
}

void verify_native_panel_control(
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto *panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(100, 100, 260, 140)
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
        backing:NSBackingStoreBuffered
        defer:NO];
    panel.releasedWhenClosed = NO;
    auto *button = [[NSButton alloc] initWithFrame:NSMakeRect(60, 48, 140, 32)];
    button.title = @"Harmless panel target";
    button.bezelStyle = NSBezelStyleRounded;
    [panel.contentView addSubview:button];
    [panel orderFront:nil];
    QCoreApplication::processEvents();

    auto popup = show_popup(context);
    auto *probe = [[LingTaiNativeControlProbe alloc] init];
    probe->actionCount = 0;
    probe->popupWasHidden = false;
    probe->popup = popup.get();
    {
        const auto restore_action = ScopedButtonAction(
            button, probe, @selector(handleControl:));
        const auto before = requests.count;
        send_native_button_click(button, panel);
        require(requests.count == before + 1
                && probe->actionCount == 1
                && probe->popupWasHidden,
            "native panel control must receive once after one synchronous hide");
    }

    [panel orderOut:nil];
    [button removeFromSuperview];
    [button release];
    [probe release];
    [panel close];
    [panel release];
}

void verify_filter_event_matrix(
        MacPopupDismissalBridge &bridge,
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    struct Case final {
        NSEventType type;
        bool dismiss;
        const char *name;
    };
    constexpr auto cases = std::array{
        Case{ NSEventTypeLeftMouseDown, true, "left down" },
        Case{ NSEventTypeRightMouseDown, true, "right down" },
        Case{ NSEventTypeOtherMouseDown, true, "other down" },
        Case{ NSEventTypeLeftMouseDragged, false, "drag" },
        Case{ NSEventTypeLeftMouseUp, false, "up" },
        Case{ NSEventTypeMouseMoved, false, "move" },
    };
    auto event_number = NSInteger(400);
    for (const auto &test : cases) {
        auto popup = show_popup(context);
        const auto before = requests.count;
        auto *event = mouse_event(
            test.type,
            native_window(context.shell.window()),
            NSMakePoint(3, 3),
            event_number++);
        require(!run_filter(bridge, event),
            std::string(test.name) + " must never be consumed");
        require(requests.count == before + (test.dismiss ? 1 : 0),
            std::string(test.name) + " request count mismatch");
        require(popup->isHidden() == test.dismiss,
            std::string(test.name) + " visibility mismatch");
        popup->hideMenu(true);
    }

    const auto before_none = requests.count;
    auto *no_popup_event = mouse_event(
        NSEventTypeLeftMouseDown,
        native_window(context.shell.window()),
        NSMakePoint(3, 3),
        event_number++);
    require(!run_filter(bridge, no_popup_event)
            && requests.count == before_none,
        "no-visible-popup press must not fire and must return false");

    auto popup = show_popup(context);
    const auto before_sequence = requests.count;
    auto *down = mouse_event(
        NSEventTypeLeftMouseDown,
        native_window(context.shell.window()),
        NSMakePoint(3, 3),
        event_number++);
    auto *up = mouse_event(
        NSEventTypeLeftMouseUp,
        native_window(context.shell.window()),
        NSMakePoint(3, 3),
        event_number++);
    require(!run_filter(bridge, down)
            && !run_filter(bridge, up)
            && !run_filter(bridge, down)
            && requests.count == before_sequence + 1,
        "release and repeated/late press after hide must not fire again");
}

void verify_nil_window(
        MacPopupDismissalBridge &bridge,
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto popup = show_popup(context);
    const auto before = requests.count;
    auto *event = mouse_event(
        NSEventTypeLeftMouseDown, nil, NSMakePoint(0, 0), 501);
    auto result = qintptr(0x1234);
    require(event.window == nil,
        "nil-window fixture must expose no recipient NSWindow");
    require(!run_filter(bridge, event, &result),
        "nil-window event must return false");
    require(requests.count == before + 1 && popup->isHidden(),
        "nil-window event with popup must fire once");
}

void verify_existing_escape_and_deactivation(
        ShellContext &context,
        ForceHideRequestCounter &requests) {
    auto popup = show_popup(context);
    const auto before_escape = requests.count;
    auto phase = Ui::PopupMenu::AnimatePhase::Shown;
    auto phase_lifetime = rpl::lifetime();
    popup->animatePhaseValue(
    ) | rpl::on_next([&](Ui::PopupMenu::AnimatePhase value) {
        phase = value;
    }, phase_lifetime);
    auto escape = QKeyEvent(
        QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    popup->deliver_key_press(escape);
    QCoreApplication::processEvents();
    require(phase == Ui::PopupMenu::AnimatePhase::StartHide,
        "Escape must enter lib_ui's popup hide path");
    popup->hideMenu(true);
    require(popup->isHidden(),
        "Escape-owned popup must complete cleanup");
    require(requests.count == before_escape,
        "Escape must not use the native force stream");

    popup = show_popup(context);
    const auto before_deactivate = requests.count;
    context.composer->setFocus();
    auto deactivate = QEvent(QEvent::WindowDeactivate);
    QApplication::sendEvent(popup.get(), &deactivate);
    auto focus_out = QFocusEvent(
        QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    popup->deliver_focus_out(focus_out);
    QCoreApplication::processEvents();
    require(wait_until([&] { return popup->isHidden(); })
            && requests.count == before_deactivate,
        "deactivation/focus path must retain lib_ui ownership without force stream activity");
}

void verify_two_shell_lifetime(QApplication &application) {
    required_bridge(application);
    {
        auto first = std::make_unique<lingtai::desktop::NativeShell>();
        auto second = std::make_unique<lingtai::desktop::NativeShell>();
        first->show_offscreen();
        second->show_offscreen();
        QCoreApplication::processEvents();
        required_bridge(application);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    required_bridge(application);
}

} // namespace

int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            qputenv("QT_LOGGING_RULES",
                "qt.qpa.fonts.warning=false;qt.qpa.keymapper.warning=false");
            QApplication application(argc, argv);
            verify_identity_predicate();
            auto context = ShellContext();
            auto requests = ForceHideRequestCounter();
            auto &bridge = required_bridge(application);

            verify_native_traffic_light(application, context, requests);
            verify_qt_content_and_native_boundary(context, requests);
            verify_second_shell(application, context, requests);
            verify_inside_root(bridge, context, requests);
            verify_inside_submenu(bridge, context, requests);
            verify_tree_hide_and_deferred_delete(bridge, context, requests);
            verify_native_panel_control(context, requests);
            verify_filter_event_matrix(bridge, context, requests);
            verify_nil_window(bridge, context, requests);
            verify_existing_escape_and_deactivation(context, requests);
            verify_two_shell_lifetime(application);

            std::cout
                << "mac_popup_dismissal_matrix=passed\n"
                << "force_hide_requests_observed=" << requests.count << '\n'
                << "bridge_instances=1\n"
                << "mac popup dismissal: OK\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "mac popup dismissal: " << error.what() << '\n';
            return 1;
        }
    }
}
