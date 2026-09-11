#include "setup_style.h"

#include "base/basic_types.h"
#include "styles/palette.h"
#include "styles/style_widgets.h"
#include "ui/style/style_core_scale.h"
#include "ui/style/style_core_palette.h"

#include <QtCore/QPointer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace lingtai::desktop {
namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QList<QStyle *> owned_fusion_styles(const QWidget &widget) {
    return widget.findChildren<QStyle *>(
        QStringLiteral("lingtai_setup_fusion_style"), Qt::FindDirectChildrenOnly);
}

SetupTokens make_tokens(int shade) {
    SetupTokens tokens{};
    tokens.selected_row = QColor(shade, 0x00, 0x00);
    tokens.border = QColor(0x00, shade, 0x00);
    tokens.value_text = QColor(0x00, 0x00, shade);
    tokens.control_fill = QColor(shade, shade, 0x00);
    return tokens;
}

// Direct QWidget/apply_setup_fusion baseline, with no stylesheet ever
// installed: widget->style() itself must be the owned Fusion instance, and
// repeated calls must not replace or duplicate it.
void verify_fusion_install_is_idempotent_and_owned() {
    QWidget widget;
    apply_setup_fusion(&widget);
    const auto first_pass = owned_fusion_styles(widget);
    require(first_pass.size() == 1,
        "apply_setup_fusion must install exactly one owned Fusion style");
    auto *first = first_pass.first();
    require(widget.style() == first,
        "widget style must be the installed Fusion instance before any stylesheet wraps it");
    require(first->parent() == &widget,
        "the created Fusion style must be parented to the widget for lifetime ownership");

    apply_setup_fusion(&widget);
    apply_setup_fusion(&widget);
    const auto after_repeat = owned_fusion_styles(widget);
    require(after_repeat.size() == 1,
        "repeated apply_setup_fusion calls must not accumulate orphaned Fusion styles");
    require(after_repeat.first() == first,
        "repeated apply_setup_fusion calls on the same widget must reuse the owned style");
}

// Exercises a real fusion-backed helper through the actual production shape:
// apply(tokens) always calls setStyleSheet(), so from the first call onward
// widget->style() is a QStyleSheetStyle proxy, not the raw Fusion instance.
// Alternates two distinct palettes (light/dark-equivalent) three times, i.e.
// the exact repeated apply_chrome() re-entry that caused the retained-heap
// growth, and asserts the owned Fusion child stays singular and stable while
// colors keep tracking the current palette.
void verify_fusion_engine_stays_one_time_across_palettes(
        QWidget &widget,
        const std::function<void(const SetupTokens &)> &apply,
        const std::string &label) {
    const auto tokens_a = make_tokens(0x22);
    const auto tokens_b = make_tokens(0xCC);

    apply(tokens_a);
    const auto installed = owned_fusion_styles(widget);
    require(installed.size() == 1,
        label + ": first call must install exactly one owned Fusion style");
    auto *first = installed.first();
    require(first->parent() == &widget,
        label + ": the installed Fusion style must be owned by the widget");
    require(widget.styleSheet().contains(setup_color_css(tokens_a.border)),
        label + ": styleSheet must reflect the first palette's border color");

    apply(tokens_b);
    require(widget.styleSheet().contains(setup_color_css(tokens_b.border)),
        label + ": styleSheet must adopt the second palette's border color");
    require(!widget.styleSheet().contains(setup_color_css(tokens_a.border)),
        label + ": styleSheet must drop the first palette's stale color");

    apply(tokens_a);
    require(widget.styleSheet().contains(setup_color_css(tokens_a.border)),
        label + ": styleSheet must restore the first palette's border color");

    const auto after_three_calls = owned_fusion_styles(widget);
    require(after_three_calls.size() == 1,
        label + ": alternating palettes must not accumulate orphaned Fusion styles");
    require(after_three_calls.first() == first,
        label + ": alternating palettes must reuse the same owned Fusion style");
}

void verify_line_edit_engine_one_time_across_palettes() {
    QLineEdit field;
    verify_fusion_engine_stays_one_time_across_palettes(
        field,
        [&field](const SetupTokens &tokens) {
            apply_setup_line_edit(&field, tokens);
        },
        "apply_setup_line_edit");
}

void verify_secondary_button_engine_one_time_across_palettes() {
    QPushButton button;
    verify_fusion_engine_stays_one_time_across_palettes(
        button,
        [&button](const SetupTokens &tokens) {
            apply_setup_secondary_button(&button, tokens);
        },
        "apply_setup_secondary_button");
}

// Proves explicit ownership, not just idempotence: the owned Fusion style
// must not survive its widget as a process-lifetime orphan.
void verify_owned_fusion_style_dies_with_widget() {
    QPointer<QStyle> tracked;
    {
        QLineEdit field;
        apply_setup_line_edit(&field, make_tokens(0x22));
        const auto owned = owned_fusion_styles(field);
        require(owned.size() == 1,
            "apply_setup_line_edit must install exactly one owned Fusion style "
            "before the scope-death check");
        tracked = owned.first();
        require(!tracked.isNull(),
            "QPointer must observe the owned Fusion style while its widget is alive");
    }
    require(tracked.isNull(),
        "the owned Fusion style must be destroyed with its widget, not leaked "
        "as a process-lifetime orphan");
}

int run_setup_style_test(int argc, char **argv) {
    try {
        QApplication application(argc, argv);
        style::internal::init_palette(style::kScaleDefault);
        style::internal::init_style_widgets(style::kScaleDefault);

        verify_fusion_install_is_idempotent_and_owned();
        verify_line_edit_engine_one_time_across_palettes();
        verify_secondary_button_engine_one_time_across_palettes();
        verify_owned_fusion_style_dies_with_widget();
        std::cout << "setup style fusion idempotence: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "setup style fusion idempotence: " << error.what() << '\n';
        return 1;
    }
}

} // namespace
} // namespace lingtai::desktop

int main(int argc, char **argv) {
    return lingtai::desktop::run_setup_style_test(argc, argv);
}
