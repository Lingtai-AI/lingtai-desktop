#pragma once

#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QPalette>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QWidget;

namespace lingtai::desktop {

struct SetupTokens {
    QColor surface;
    QColor header;
    QColor section_band;
    QColor section_text;
    QColor divider;
    QColor selected_row;
    QColor selection_accent;
    QColor border;
    QColor tag_fill;
    QColor muted_text;
    QColor danger_text;
    QColor page_bg;
    QColor value_text;
    QColor control_fill;
};

[[nodiscard]] bool setup_is_dark(const QPalette &palette);
[[nodiscard]] SetupTokens setup_tokens(const QPalette &palette);
[[nodiscard]] QString setup_color_css(const QColor &color);
[[nodiscard]] QString setup_line_edit_css(const SetupTokens &tokens);
[[nodiscard]] QString setup_combo_css(const SetupTokens &tokens);
[[nodiscard]] QString setup_plain_text_css(const SetupTokens &tokens);
[[nodiscard]] QString setup_choice_button_css(const SetupTokens &tokens);
[[nodiscard]] QString setup_spin_wrap_css(const SetupTokens &tokens);
[[nodiscard]] QString setup_chip_css(const SetupTokens &tokens);

void apply_setup_fusion(QWidget *widget);
void apply_setup_line_edit(QLineEdit *field, const SetupTokens &tokens);
void apply_setup_plain_text(QPlainTextEdit *field, const SetupTokens &tokens);

QLabel *make_setup_label(
    QWidget *parent,
    const QString &text,
    const char *object_name,
    int point_size,
    QFont::Weight weight = QFont::Normal,
    const QColor &color = {});

void apply_setup_card(QWidget *widget, const SetupTokens &tokens, bool dashed = false);
void apply_setup_primary_button(QPushButton *button);
void apply_setup_secondary_button(QPushButton *button, const SetupTokens &tokens);

} // namespace lingtai::desktop
