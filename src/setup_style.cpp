#include "setup_style.h"

#include "base/basic_types.h"
#include "styles/palette.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyleFactory>

namespace lingtai::desktop {

bool setup_is_dark(const QPalette &palette) {
    // Prefer the OS color scheme so setup chips track System Settings light/dark
    // even if a stale st::windowBg token briefly disagrees.
    if (const auto *hints = QGuiApplication::styleHints()) {
        const auto scheme = hints->colorScheme();
        if (scheme == Qt::ColorScheme::Dark) {
            return true;
        }
        if (scheme == Qt::ColorScheme::Light) {
            return false;
        }
    }
    if (st::windowBg->c.lightness() < 128) {
        return true;
    }
    return palette.color(QPalette::Window).lightness() < 128;
}

SetupTokens setup_tokens(const QPalette &palette) {
    if (setup_is_dark(palette)) {
        // Match Telegram night shell blues on both setup and chat canvases.
        return {
            QColor(QStringLiteral("#17212B")),
            QColor(QStringLiteral("#202B36")),
            QColor(QStringLiteral("#232E3C")),
            QColor(QStringLiteral("#7F91A4")),
            QColor(255, 255, 255, 38),
            QColor(QStringLiteral("#1E2F40")),
            st::windowBgActive->c,
            QColor(255, 255, 255, 38),
            QColor(255, 255, 255, 12),
            QColor(QStringLiteral("#7F91A4")),
            QColor(QStringLiteral("#F87171")),
            QColor(QStringLiteral("#17212B")),
            QColor(QStringLiteral("#E4ECF2")),
            QColor(QStringLiteral("#202B36")),
        };
    }
    // Match the light shell neutrals — not a leftover jade-green setup island.
    return {
        QColor(QStringLiteral("#FFFFFF")),
        QColor(QStringLiteral("#F7F7F7")),
        QColor(QStringLiteral("#EEF1F4")),
        QColor(QStringLiteral("#5B6B7A")),
        QColor(0, 0, 0, 20),
        QColor(QStringLiteral("#E8F1F8")),
        st::windowBgActive->c,
        QColor(QStringLiteral("#D8DEE6")),
        QColor(0, 0, 0, 8),
        QColor(QStringLiteral("#6B7280")),
        QColor(QStringLiteral("#B42318")),
        QColor(QStringLiteral("#F7F7F7")),
        QColor(QStringLiteral("#1F2933")),
        QColor(QStringLiteral("#FFFFFF")),
    };
}

QString setup_color_css(const QColor &color) {
    if (color.alpha() < 255) {
        return QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(color.alpha());
    }
    return color.name(QColor::HexRgb).toUpper();
}

QString setup_line_edit_css(const SetupTokens &tokens) {
    return QStringLiteral(
        "QLineEdit { border: 1px solid %1; border-radius: 8px; padding: 0 12px; "
        "background: %2; color: %3; selection-background-color: %4; }")
        .arg(setup_color_css(tokens.border),
            setup_color_css(tokens.control_fill),
            setup_color_css(tokens.value_text),
            setup_color_css(tokens.selected_row));
}

QString setup_combo_css(const SetupTokens &tokens) {
    const auto border = setup_color_css(tokens.border);
    const auto fill = setup_color_css(tokens.control_fill);
    const auto ink = setup_color_css(tokens.value_text);
    const auto accent = setup_color_css(tokens.selection_accent);
    const auto selected = setup_color_css(tokens.selected_row);
    return QStringLiteral(
        "QComboBox { border: 1px solid %1; border-radius: 8px; "
        "padding: 0 28px 0 12px; background: %2; color: %3; }"
        "QComboBox:hover { border: 1px solid %4; }"
        "QComboBox::drop-down { subcontrol-origin: border; "
        "subcontrol-position: center right; width: 28px; border: none; }"
        "QComboBox QAbstractItemView { border: 1px solid %1; border-radius: 8px; "
        "background: %2; color: %3; selection-background-color: %5; "
        "selection-color: %4; padding: 4px; outline: 0; }")
        .arg(border, fill, ink, accent, selected);
}

QString setup_plain_text_css(const SetupTokens &tokens) {
    return QStringLiteral(
        "QPlainTextEdit { border: 1px solid %1; border-radius: 8px; "
        "padding: 8px; background: %2; color: %3; }")
        .arg(setup_color_css(tokens.border),
            setup_color_css(tokens.control_fill),
            setup_color_css(tokens.value_text));
}

QString setup_choice_button_css(const SetupTokens &tokens) {
    // Idle chips must track the page theme: white on light, elevated fill on
    // night. Never leave Fusion/Button defaults to paint a dark plate in light.
    const auto dark = tokens.page_bg.lightness() < 128;
    const auto idle_fill = dark ? tokens.control_fill : tokens.surface;
    const auto idle_text = tokens.value_text;
    return QStringLiteral(
        "QPushButton { border: 1px solid %1; border-radius: 6px; "
        "padding: 0 12px; background-color: %2; color: %3; }"
        "QPushButton:!checked { background-color: %2; color: %3; "
        "border: 1px solid %1; font-weight: 400; }"
        "QPushButton:checked { background-color: %4; color: white; "
        "border: 1px solid %4; font-weight: 600; }")
        .arg(setup_color_css(tokens.border),
            setup_color_css(idle_fill),
            setup_color_css(idle_text),
            setup_color_css(tokens.selection_accent));
}

QString setup_spin_wrap_css(const SetupTokens &tokens) {
    return QStringLiteral(
        "QWidget#lingtai_setup_review_spin_wrap { border: 1px solid %1; "
        "border-radius: 8px; background: %2; }"
        "QSpinBox { border: none; background: transparent; padding: 0 10px; "
        "color: %3; }")
        .arg(setup_color_css(tokens.border),
            setup_color_css(tokens.control_fill),
            setup_color_css(tokens.value_text));
}

QString setup_chip_css(const SetupTokens &tokens) {
    const auto fill = tokens.page_bg.lightness() < 128
        ? tokens.section_band
        : tokens.selected_row;
    return QStringLiteral(
        "background: %1; color: %2; border: 1px solid %3; "
        "border-radius: 10px; padding: 2px 8px;")
        .arg(setup_color_css(fill),
            setup_color_css(tokens.selection_accent),
            setup_color_css(tokens.border));
}

void apply_setup_fusion(QWidget *widget) {
    if (!widget) return;
    widget->setAttribute(Qt::WA_MacShowFocusRect, false);
    if (auto *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        widget->setStyle(fusion);
    }
}

void apply_setup_line_edit(QLineEdit *field, const SetupTokens &tokens) {
    if (!field) return;
    apply_setup_fusion(field);
    field->setStyleSheet(setup_line_edit_css(tokens));
    auto palette = field->palette();
    palette.setColor(QPalette::Base, tokens.control_fill);
    palette.setColor(QPalette::Text, tokens.value_text);
    palette.setColor(QPalette::PlaceholderText, tokens.muted_text);
    field->setPalette(palette);
}

void apply_setup_plain_text(QPlainTextEdit *field, const SetupTokens &tokens) {
    if (!field) return;
    apply_setup_fusion(field);
    field->setStyleSheet(setup_plain_text_css(tokens));
    auto palette = field->palette();
    palette.setColor(QPalette::Base, tokens.control_fill);
    palette.setColor(QPalette::Text, tokens.value_text);
    palette.setColor(QPalette::PlaceholderText, tokens.muted_text);
    field->setPalette(palette);
}

QLabel *make_setup_label(
        QWidget *parent,
        const QString &text,
        const char *object_name,
        int point_size,
        QFont::Weight weight,
        const QColor &color) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(object_name);
    label->setTextFormat(Qt::PlainText);
    label->setAccessibleName(text);
    label->setWordWrap(true);
    auto policy = label->sizePolicy();
    policy.setHeightForWidth(true);
    policy.setVerticalPolicy(QSizePolicy::Fixed);
    label->setSizePolicy(policy);
    auto font = label->font();
    font.setPointSize(point_size);
    font.setWeight(weight);
    label->setFont(font);
    if (color.isValid()) {
        label->setStyleSheet(
            QStringLiteral("color: %1;").arg(setup_color_css(color)));
    }
    return label;
}

void apply_setup_card(QWidget *widget, const SetupTokens &tokens, bool dashed) {
    if (!widget) return;
    const auto border = setup_color_css(tokens.border);
    const auto surface = setup_color_css(tokens.surface);
    const auto style = dashed
        ? QStringLiteral(
            "QWidget { background: %1; border: 1px dashed %2; "
            "border-radius: 10px; }")
        : QStringLiteral(
            "QWidget { background: %1; border: 1px solid %2; "
            "border-radius: 10px; }");
    widget->setStyleSheet(style.arg(surface, border));
}

void apply_setup_primary_button(QPushButton *button) {
    if (!button) return;
    const auto tokens = setup_tokens(button->palette());
    const auto accent = setup_color_css(tokens.selection_accent);
    auto disabled = tokens.selection_accent;
    disabled.setAlpha(st::windowBg->c.lightness() >= 128 ? 160 : 120);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 34px; padding: 0 16px; border-radius: 6px; "
        "background: %1; color: white; border: none; font-weight: 600; } "
        "QPushButton:disabled { background: %2; color: rgba(255,255,255,0.8); }")
        .arg(accent, setup_color_css(disabled)));
}

void apply_setup_secondary_button(QPushButton *button, const SetupTokens &tokens) {
    if (!button) return;
    apply_setup_fusion(button);
    const auto border = setup_color_css(tokens.border);
    const auto fill = setup_color_css(tokens.control_fill);
    const auto ink = setup_color_css(tokens.value_text);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 34px; padding: 0 16px; border-radius: 6px; "
        "background: %1; color: %2; border: 1px solid %3; }")
        .arg(fill, ink, border));
}

} // namespace lingtai::desktop
