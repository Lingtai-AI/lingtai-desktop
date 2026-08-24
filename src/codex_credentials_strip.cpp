#include "codex_credentials_strip.h"

#include "credentials_model.h"
#include "setup_style.h"

#include <QtGui/QEnterEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace lingtai::desktop {

CodexCredentialsStrip::CodexCredentialsStrip(QWidget *parent)
: QWidget(parent) {
    setObjectName("lingtai_setup_codex_credentials_strip");
    setAccessibleName(QStringLiteral("Codex credentials"));
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(56);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(12);

    auto *text_column = new QWidget(this);
    text_column->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *text_layout = new QVBoxLayout(text_column);
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(2);
    title_ = make_setup_label(text_column,
        QStringLiteral("Codex credentials"),
        "lingtai_setup_codex_credentials_title", 13, QFont::DemiBold);
    title_->setAttribute(Qt::WA_TransparentForMouseEvents);
    status_ = make_setup_label(text_column, QString(),
        "lingtai_setup_codex_credentials_status", 12);
    status_->setAttribute(Qt::WA_TransparentForMouseEvents);
    text_layout->addWidget(title_);
    text_layout->addWidget(status_);
    layout->addWidget(text_column, 1);

    action_ = new QPushButton(QStringLiteral("Manage"), this);
    action_->setObjectName("lingtai_setup_codex_credentials_action");
    action_->setCursor(Qt::PointingHandCursor);
    action_->setFlat(true);
    action_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(action_, 0, Qt::AlignVCenter);
    QObject::connect(action_, &QPushButton::clicked,
        this, &CodexCredentialsStrip::manage_requested);

    refresh();
    apply_chrome();
}

void CodexCredentialsStrip::refresh() {
    const auto summary = codex_credentials_summary();
    if (!summary.has_valid) {
        status_->setText(summary.has_accounts
            ? QStringLiteral("Sign-in required")
            : QStringLiteral("Not signed in"));
    } else {
        status_->setText(QStringLiteral("✓ %1").arg(summary.primary_label));
    }
}

void CodexCredentialsStrip::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        apply_chrome();
    }
}

void CodexCredentialsStrip::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        emit manage_requested();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CodexCredentialsStrip::enterEvent(QEnterEvent *event) {
    hovered_ = true;
    apply_chrome();
    QWidget::enterEvent(event);
}

void CodexCredentialsStrip::leaveEvent(QEvent *event) {
    hovered_ = false;
    apply_chrome();
    QWidget::leaveEvent(event);
}

void CodexCredentialsStrip::apply_chrome() {
    if (applying_chrome_) return;
    applying_chrome_ = true;
    const auto tokens = setup_tokens(palette());
    action_->setStyleSheet(QStringLiteral(
        "QPushButton#lingtai_setup_codex_credentials_action { "
        "color: %1; background: transparent; border: none; "
        "padding: 4px 10px; font-weight: 600; } "
        "QPushButton#lingtai_setup_codex_credentials_action:hover { "
        "background: %2; border-radius: 8px; }")
        .arg(setup_color_css(tokens.selection_accent),
            setup_color_css(tokens.tag_fill)));
    title_->setStyleSheet(QStringLiteral("color: %1;")
        .arg(setup_color_css(tokens.value_text)));
    status_->setStyleSheet(QStringLiteral("color: %1;")
        .arg(setup_color_css(tokens.muted_text)));
    // Elevate above the setup page in both themes (same rule as Credentials).
    const auto surface = setup_color_css(
        setup_is_dark(palette()) ? tokens.control_fill : tokens.surface);
    const auto border = setup_color_css(tokens.border);
    const auto hover = setup_color_css(tokens.selected_row);
    setStyleSheet(QStringLiteral(
        "QWidget#lingtai_setup_codex_credentials_strip { "
        "background: %1; border: 1px solid %2; border-radius: 10px; }")
        .arg(hovered_ ? hover : surface, border));
    applying_chrome_ = false;
}

} // namespace lingtai::desktop
