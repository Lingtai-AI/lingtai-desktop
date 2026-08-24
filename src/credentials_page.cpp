#include "credentials_page.h"

#include "codex_oauth.h"
#include "credentials_model.h"
#include "setup_style.h"

#include <QtCore/QEvent>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtGui/QDesktopServices>
#include <QtGui/QFont>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

#include <functional>

namespace lingtai::desktop {
namespace {

// Cards must elevate above the page in both themes: white on light grey, and
// night `#202B36` on `#17212B` — never the same fill as the page canvas.
QColor credentials_card_fill(const SetupTokens &tokens, const QPalette &palette) {
    return setup_is_dark(palette) ? tokens.control_fill : tokens.surface;
}

void color_label(QLabel *label, const QColor &color) {
    if (!label) return;
    label->setStyleSheet(QStringLiteral("color: %1;")
        .arg(setup_color_css(color)));
}

QString initials_for(const QString &name) {
    QString letters;
    for (const auto &part : name.split(QRegularExpression(QStringLiteral("[@._\\-\\s]")),
            Qt::SkipEmptyParts)) {
        if (part.isEmpty()) continue;
        letters += part.left(1).toUpper();
        if (letters.size() == 2) break;
    }
    return letters.isEmpty() ? QStringLiteral("C") : letters;
}

void paint_avatar(QLabel *avatar, const QString &name, const SetupTokens &tokens) {
    avatar->setText(initials_for(name));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(44, 44);
    avatar->setAttribute(Qt::WA_StyledBackground, true);
    auto font = avatar->font();
    font.setPointSize(13);
    font.setWeight(QFont::DemiBold);
    avatar->setFont(font);
    avatar->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border-radius: 22px;")
        .arg(setup_color_css(tokens.selected_row),
            setup_color_css(tokens.selection_accent)));
}

QPushButton *make_ghost_button(QWidget *parent, const QString &text, const char *name) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFlat(true);
    button->setFixedHeight(30);
    return button;
}

QWidget *make_account_row(
        QWidget *parent,
        const CodexAccount &account,
        const SetupTokens &tokens,
        const QPalette &palette,
        const std::function<void()> &on_remove) {
    auto *row = new QWidget(parent);
    row->setObjectName(QStringLiteral("lingtai_setup_credentials_row"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setMinimumHeight(72);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(16, 14, 14, 14);
    layout->setSpacing(14);

    auto *avatar = make_setup_label(row, QString(),
        "lingtai_setup_credentials_row_avatar", 13, QFont::DemiBold);
    paint_avatar(avatar, codex_account_display_name(account), tokens);
    layout->addWidget(avatar, 0, Qt::AlignVCenter);

    auto *text_column = new QWidget(row);
    auto *text_layout = new QVBoxLayout(text_column);
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(3);
    const auto title = codex_account_display_name(account);
    // Titles must use value_text — palette WindowText can stay light-mode black
    // while the card fill follows night tokens, which is unreadable.
    text_layout->addWidget(make_setup_label(text_column, title,
        "lingtai_setup_credentials_row_title", 14, QFont::DemiBold,
        tokens.value_text));
    auto *detail = make_setup_label(text_column,
        account.legacy
            ? QStringLiteral("~/.lingtai-tui/codex-auth.json")
            : account.ref,
        "lingtai_setup_credentials_row_detail", 11, QFont::Normal, tokens.muted_text);
    text_layout->addWidget(detail);
    layout->addWidget(text_column, 1);

    auto *status = make_setup_label(row,
        account.valid ? QStringLiteral("Valid") : QStringLiteral("Re-auth needed"),
        "lingtai_setup_credentials_row_status", 11, QFont::DemiBold);
    status->setAlignment(Qt::AlignCenter);
    status->setFixedHeight(24);
    status->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border-radius: 12px; padding: 0 10px;")
        .arg(setup_color_css(account.valid ? tokens.selected_row : tokens.tag_fill),
            setup_color_css(account.valid ? tokens.selection_accent : tokens.danger_text)));
    layout->addWidget(status, 0, Qt::AlignVCenter);

    auto *remove = make_ghost_button(row, QStringLiteral("Remove"),
        "lingtai_setup_credentials_remove");
    remove->setProperty("armed", false);
    remove->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; background: transparent; border: none; "
        "padding: 0 10px; font-weight: 600; border-radius: 8px; } "
        "QPushButton:hover { color: %2; background: %3; }")
        .arg(setup_color_css(tokens.muted_text),
            setup_color_css(tokens.danger_text),
            setup_color_css(tokens.selected_row)));
    QObject::connect(remove, &QPushButton::clicked, row, [remove, on_remove] {
        if (remove->property("armed").toBool()) {
            on_remove();
            return;
        }
        remove->setProperty("armed", true);
        remove->setText(QStringLiteral("Confirm"));
    });
    layout->addWidget(remove, 0, Qt::AlignVCenter);

    const auto card = credentials_card_fill(tokens, palette);
    row->setStyleSheet(QStringLiteral(
        "QWidget#lingtai_setup_credentials_row { background: %1; "
        "border: 1px solid %2; border-radius: 12px; }")
        .arg(setup_color_css(card), setup_color_css(tokens.border)));
    return row;
}

QWidget *make_method_card(
        QWidget *parent,
        const QString &title,
        const QString &detail,
        const char *name,
        const SetupTokens &tokens) {
    auto *card = new QPushButton(parent);
    card->setObjectName(name);
    card->setCursor(Qt::PointingHandCursor);
    card->setMinimumHeight(88);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(4);
    layout->addWidget(make_setup_label(card, title,
        "lingtai_setup_credentials_method_title", 14, QFont::DemiBold,
        tokens.value_text));
    auto *note = make_setup_label(card, detail,
        "lingtai_setup_credentials_method_detail", 12, QFont::Normal, tokens.muted_text);
    note->setWordWrap(true);
    note->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->itemAt(0)->widget()->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(note);
    return card;
}

} // namespace

CredentialsPage::CredentialsPage(QWidget *parent)
: QWidget(parent) {
    setObjectName("lingtai_setup_credentials_page");
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    oauth_ = new CodexOAuthFlow(this);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    back_ = new QPushButton(QStringLiteral("← Presets"), this);
    back_->setObjectName("lingtai_setup_credentials_back");
    back_->setFlat(true);
    back_->setCursor(Qt::PointingHandCursor);
    back_->setFixedHeight(24);
    root->addWidget(back_, 0, Qt::AlignLeft);

    root->addWidget(make_setup_label(this, QStringLiteral("Credentials"),
        "lingtai_setup_credentials_heading", 22, QFont::DemiBold,
        setup_tokens(palette()).value_text));
    auto *subtitle = make_setup_label(this,
        QStringLiteral("Codex OAuth accounts for this machine. "
            "API keys are read from your environment automatically."),
        "lingtai_setup_credentials_subtitle", 13, QFont::Normal,
        setup_tokens(palette()).muted_text);
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    hero_ = new QWidget(this);
    hero_->setObjectName("lingtai_setup_credentials_hero");
    hero_->setAttribute(Qt::WA_StyledBackground, true);
    auto *hero_layout = new QHBoxLayout(hero_);
    hero_layout->setContentsMargins(0, 0, 16, 0);
    hero_layout->setSpacing(14);
    auto *rail = new QFrame(hero_);
    rail->setObjectName("lingtai_setup_credentials_hero_rail");
    rail->setFixedWidth(3);
    hero_layout->addWidget(rail);
    hero_avatar_ = make_setup_label(hero_, QStringLiteral("C"),
        "lingtai_setup_credentials_hero_avatar", 13, QFont::DemiBold);
    hero_layout->addWidget(hero_avatar_, 0, Qt::AlignVCenter);
    auto *hero_text = new QWidget(hero_);
    auto *hero_text_layout = new QVBoxLayout(hero_text);
    hero_text_layout->setContentsMargins(0, 14, 0, 14);
    hero_text_layout->setSpacing(2);
    hero_kicker_ = make_setup_label(hero_text, QString(),
        "lingtai_setup_credentials_hero_kicker", 11, QFont::DemiBold);
    hero_title_ = make_setup_label(hero_text, QString(),
        "lingtai_setup_credentials_hero_title", 16, QFont::DemiBold);
    hero_detail_ = make_setup_label(hero_text, QString(),
        "lingtai_setup_credentials_hero_detail", 12);
    hero_text_layout->addWidget(hero_kicker_);
    hero_text_layout->addWidget(hero_title_);
    hero_text_layout->addWidget(hero_detail_);
    hero_layout->addWidget(hero_text, 1);
    root->addWidget(hero_);

    message_ = make_setup_label(this, QString(),
        "lingtai_setup_credentials_message", 12, QFont::Normal);
    message_->hide();
    root->addWidget(message_);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("lingtai_setup_credentials_list");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_ = new QWidget(scroll);
    list_->setObjectName("lingtai_setup_credentials_rows");
    auto *list_layout = new QVBoxLayout(list_);
    list_layout->setContentsMargins(0, 0, 8, 8);
    list_layout->setSpacing(10);
    scroll->setWidget(list_);
    root->addWidget(scroll, 1);

    method_panel_ = new QWidget(this);
    method_panel_->setObjectName("lingtai_setup_credentials_method_panel");
    auto *method_layout = new QVBoxLayout(method_panel_);
    method_layout->setContentsMargins(0, 0, 0, 0);
    method_layout->setSpacing(10);
    auto *method_heading = make_setup_label(method_panel_,
        QStringLiteral("Choose a sign-in method"),
        "lingtai_setup_credentials_method_heading", 14, QFont::DemiBold,
        setup_tokens(palette()).value_text);
    method_layout->addWidget(method_heading);
    auto *method_cards = new QHBoxLayout;
    method_cards->setSpacing(12);
    auto *browser = make_method_card(method_panel_,
        QStringLiteral("Browser sign-in"),
        QStringLiteral("Open ChatGPT and return here when finished."),
        "lingtai_setup_credentials_browser_signin",
        setup_tokens(palette()));
    auto *device = make_method_card(method_panel_,
        QStringLiteral("Device code"),
        QStringLiteral("Enter a one-time code on another device."),
        "lingtai_setup_credentials_device_signin",
        setup_tokens(palette()));
    method_cards->addWidget(browser);
    method_cards->addWidget(device);
    method_layout->addLayout(method_cards);
    auto *cancel_method = make_ghost_button(method_panel_, QStringLiteral("Cancel"),
        "lingtai_setup_credentials_method_cancel");
    method_layout->addWidget(cancel_method, 0, Qt::AlignLeft);
    method_panel_->hide();
    root->addWidget(method_panel_);

    progress_panel_ = new QWidget(this);
    progress_panel_->setObjectName("lingtai_setup_credentials_progress_panel");
    progress_panel_->setAttribute(Qt::WA_StyledBackground, true);
    auto *progress_layout = new QVBoxLayout(progress_panel_);
    progress_layout->setContentsMargins(18, 18, 18, 18);
    progress_layout->setSpacing(10);
    progress_label_ = make_setup_label(progress_panel_, QString(),
        "lingtai_setup_credentials_progress", 14, QFont::DemiBold,
        setup_tokens(palette()).value_text);
    device_code_label_ = make_setup_label(progress_panel_, QString(),
        "lingtai_setup_credentials_device_code", 18, QFont::DemiBold,
        setup_tokens(palette()).value_text);
    device_code_label_->setWordWrap(true);
    device_code_label_->hide();
    open_browser_ = new QPushButton(QStringLiteral("Open browser"), progress_panel_);
    open_browser_->setObjectName("lingtai_setup_credentials_open_browser");
    apply_setup_primary_button(open_browser_);
    auto *cancel_oauth = new QPushButton(QStringLiteral("Cancel sign-in"), progress_panel_);
    cancel_oauth->setObjectName("lingtai_setup_credentials_cancel_signin");
    auto *progress_buttons = new QHBoxLayout;
    progress_buttons->setSpacing(8);
    progress_buttons->addWidget(open_browser_);
    progress_buttons->addWidget(cancel_oauth);
    progress_buttons->addStretch();
    progress_layout->addWidget(progress_label_);
    progress_layout->addWidget(device_code_label_);
    progress_layout->addLayout(progress_buttons);
    progress_panel_->hide();
    root->addWidget(progress_panel_);

    QObject::connect(back_, &QPushButton::clicked, this, &CredentialsPage::back_requested);
    QObject::connect(qobject_cast<QPushButton *>(browser), &QPushButton::clicked, this, [this] {
        start_oauth(CodexOAuthFlow::Mode::Browser);
    });
    QObject::connect(qobject_cast<QPushButton *>(device), &QPushButton::clicked, this, [this] {
        start_oauth(CodexOAuthFlow::Mode::DeviceCode);
    });
    QObject::connect(cancel_method, &QPushButton::clicked, this, [this] {
        show_idle();
    });
    QObject::connect(cancel_oauth, &QPushButton::clicked, this, [this] {
        oauth_->cancel();
        show_idle();
        set_message(QStringLiteral("Sign-in cancelled."), false);
    });
    QObject::connect(open_browser_, &QPushButton::clicked, this, [this] {
        if (!open_browser_->property("auth_url").toString().isEmpty()) {
            QDesktopServices::openUrl(QUrl(open_browser_->property("auth_url").toString()));
        }
    });
    QObject::connect(oauth_, &CodexOAuthFlow::auth_url_ready, this,
        [this](const QString &url, const QString &) {
            open_browser_->setProperty("auth_url", url);
            progress_label_->setText(QStringLiteral("Complete sign-in in your browser."));
            open_browser_->show();
        });
    QObject::connect(oauth_, &CodexOAuthFlow::device_code_ready, this,
        [this](const QString &url, const QString &code) {
            device_code_label_->setText(code);
            device_code_label_->show();
            progress_label_->setText(QStringLiteral("Open %1 and enter this code.")
                .arg(url));
            open_browser_->setProperty("auth_url", url);
            open_browser_->setText(QStringLiteral("Open verification page"));
            open_browser_->show();
        });
    QObject::connect(oauth_, &CodexOAuthFlow::status_changed, this,
        [this](const QString &message) {
            progress_label_->setText(message);
        });
    QObject::connect(oauth_, &CodexOAuthFlow::finished, this,
        &CredentialsPage::finish_oauth);

    reload();
    apply_chrome();
}

void CredentialsPage::reload() {
    rebuild_accounts();
    refresh_hero();
    show_idle();
    set_message({}, false);
}

void CredentialsPage::set_back_label(const QString &label) {
    back_->setText(label);
}

void CredentialsPage::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        apply_chrome();
        rebuild_accounts();
        refresh_hero();
    }
}

void CredentialsPage::rebuild_accounts() {
    const auto tokens = setup_tokens(palette());
    auto *layout = qobject_cast<QVBoxLayout *>(list_->layout());
    if (!layout) return;
    while (layout->count() > 0) {
        if (auto *item = layout->takeAt(0)) {
            if (auto *widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
    }

    auto *section = make_setup_label(list_, QStringLiteral("CODEX ACCOUNTS"),
        "lingtai_setup_credentials_section", 11, QFont::DemiBold, tokens.section_text);
    auto section_font = section->font();
    section_font.setLetterSpacing(QFont::PercentageSpacing, 118);
    section->setFont(section_font);
    layout->addWidget(section);

    const auto accounts = list_codex_accounts();
    for (const auto &account : accounts) {
        auto *row = make_account_row(list_, account, tokens, palette(),
            [this, account] {
            if (!remove_codex_account_file(account.path)) {
                set_message(QStringLiteral("Could not remove the account file."), true);
                return;
            }
            reload();
            emit accounts_changed();
        });
        layout->addWidget(row);
    }

    auto *add = new QPushButton(list_);
    add->setObjectName("lingtai_setup_credentials_add");
    add->setCursor(Qt::PointingHandCursor);
    add->setMinimumHeight(64);
    oauth_force_login_ = !accounts.isEmpty();
    auto *add_layout = new QHBoxLayout(add);
    add_layout->setContentsMargins(16, 12, 16, 12);
    add_layout->setSpacing(12);
    auto *plus = make_setup_label(add, QStringLiteral("+"),
        "lingtai_setup_credentials_add_plus", 16, QFont::DemiBold, tokens.selection_accent);
    plus->setAlignment(Qt::AlignCenter);
    plus->setFixedSize(28, 28);
    plus->setAttribute(Qt::WA_StyledBackground, true);
    plus->setAttribute(Qt::WA_TransparentForMouseEvents);
    plus->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border-radius: 14px;")
        .arg(setup_color_css(tokens.selected_row),
            setup_color_css(tokens.selection_accent)));
    add_layout->addWidget(plus, 0, Qt::AlignVCenter);
    auto *add_label = make_setup_label(add,
        accounts.isEmpty()
            ? QStringLiteral("Add Codex OAuth")
            : QStringLiteral("Add another Codex account"),
        "lingtai_setup_credentials_add_label", 13, QFont::DemiBold,
        tokens.selection_accent);
    add_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    add_layout->addWidget(add_label, 1);
    const auto card = credentials_card_fill(tokens, palette());
    add->setStyleSheet(QStringLiteral(
        "QPushButton { text-align: left; border-radius: 12px; "
        "border: 1px dashed %1; background: %2; } "
        "QPushButton:hover { background: %3; }")
        .arg(setup_color_css(tokens.border),
            setup_color_css(card),
            setup_color_css(tokens.selected_row)));
    QObject::connect(add, &QPushButton::clicked, this, &CredentialsPage::show_method_chooser);
    layout->addWidget(add);
    layout->addStretch();
}

void CredentialsPage::refresh_hero() {
    const auto tokens = setup_tokens(palette());
    const auto summary = codex_credentials_summary();
    if (summary.has_valid) {
        hero_kicker_->setText(QStringLiteral("Signed in"));
        hero_title_->setText(summary.primary_label);
        hero_detail_->setText(summary.has_accounts
            ? QStringLiteral("Accounts on this machine are ready for Codex presets.")
            : QStringLiteral("This account is ready for Codex presets."));
        paint_avatar(hero_avatar_, summary.primary_label, tokens);
    } else if (summary.has_accounts) {
        hero_kicker_->setText(QStringLiteral("Sign-in required"));
        hero_title_->setText(QStringLiteral("Codex needs a valid account"));
        hero_detail_->setText(QStringLiteral("Re-authenticate or add another ChatGPT login."));
        paint_avatar(hero_avatar_, QStringLiteral("!"), tokens);
    } else {
        hero_kicker_->setText(QStringLiteral("Not signed in"));
        hero_title_->setText(QStringLiteral("No Codex account yet"));
        hero_detail_->setText(QStringLiteral("Add Codex OAuth to use Codex presets on this machine."));
        paint_avatar(hero_avatar_, QStringLiteral("C"), tokens);
    }
    color_label(hero_kicker_, tokens.selection_accent);
    color_label(hero_title_, tokens.value_text);
    color_label(hero_detail_, tokens.muted_text);
}

void CredentialsPage::apply_chrome() {
    if (applying_chrome_) return;
    applying_chrome_ = true;
    const auto tokens = setup_tokens(palette());
    const auto card = credentials_card_fill(tokens, palette());
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "QWidget#lingtai_setup_credentials_page { background: %1; }")
        .arg(setup_color_css(tokens.page_bg)));
    if (auto *heading = findChild<QLabel *>("lingtai_setup_credentials_heading")) {
        color_label(heading, tokens.value_text);
    }
    if (auto *subtitle = findChild<QLabel *>("lingtai_setup_credentials_subtitle")) {
        color_label(subtitle, tokens.muted_text);
    }
    if (auto *method_heading = findChild<QLabel *>(
            "lingtai_setup_credentials_method_heading")) {
        color_label(method_heading, tokens.value_text);
    }
    color_label(progress_label_, tokens.value_text);
    color_label(device_code_label_, tokens.value_text);
    back_->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: none; background: transparent; "
        "text-align: left; padding: 0; font-weight: 600; }")
        .arg(setup_color_css(tokens.selection_accent)));
    if (auto *scroll = findChild<QScrollArea *>("lingtai_setup_credentials_list")) {
        scroll->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; }"));
        scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    }
    if (list_) {
        list_->setStyleSheet(QStringLiteral("background: transparent;"));
    }
    if (auto *rail = findChild<QFrame *>("lingtai_setup_credentials_hero_rail")) {
        rail->setStyleSheet(QStringLiteral("background: %1; border: none;")
            .arg(setup_color_css(tokens.selection_accent)));
    }
    hero_->setStyleSheet(QStringLiteral(
        "QWidget#lingtai_setup_credentials_hero { background: %1; "
        "border: 1px solid %2; border-radius: 12px; }")
        .arg(setup_color_css(card), setup_color_css(tokens.border)));
    const auto method_card = QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2; border-radius: 12px; "
        "text-align: left; padding: 0; } "
        "QPushButton:hover { background: %3; }")
        .arg(setup_color_css(card),
            setup_color_css(tokens.border),
            setup_color_css(tokens.selected_row));
    if (auto *browser = findChild<QPushButton *>("lingtai_setup_credentials_browser_signin")) {
        browser->setStyleSheet(method_card);
    }
    if (auto *device = findChild<QPushButton *>("lingtai_setup_credentials_device_signin")) {
        device->setStyleSheet(method_card);
    }
    if (auto *cancel_method = findChild<QPushButton *>(
            "lingtai_setup_credentials_method_cancel")) {
        cancel_method->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; background: transparent; border: none; "
            "padding: 0 10px; font-weight: 600; }")
            .arg(setup_color_css(tokens.muted_text)));
    }
    progress_panel_->setStyleSheet(QStringLiteral(
        "QWidget#lingtai_setup_credentials_progress_panel { background: %1; "
        "border: 1px solid %2; border-radius: 12px; }")
        .arg(setup_color_css(card), setup_color_css(tokens.border)));
    apply_setup_primary_button(open_browser_);
    apply_setup_secondary_button(
        findChild<QPushButton *>("lingtai_setup_credentials_cancel_signin"), tokens);
    applying_chrome_ = false;
}

void CredentialsPage::show_idle() {
    if (auto *scroll = findChild<QScrollArea *>("lingtai_setup_credentials_list")) {
        scroll->show();
    }
    hero_->show();
    method_panel_->hide();
    progress_panel_->hide();
    device_code_label_->hide();
    open_browser_->hide();
}

void CredentialsPage::show_method_chooser() {
    if (auto *scroll = findChild<QScrollArea *>("lingtai_setup_credentials_list")) {
        scroll->hide();
    }
    method_panel_->show();
    progress_panel_->hide();
    device_code_label_->hide();
    open_browser_->hide();
}

void CredentialsPage::start_oauth(CodexOAuthFlow::Mode mode) {
    if (auto *scroll = findChild<QScrollArea *>("lingtai_setup_credentials_list")) {
        scroll->hide();
    }
    method_panel_->hide();
    progress_panel_->show();
    device_code_label_->hide();
    open_browser_->hide();
    open_browser_->setProperty("auth_url", {});
    progress_label_->setText(QStringLiteral("Starting sign-in…"));
    oauth_->start(mode, oauth_force_login_);
}

void CredentialsPage::finish_oauth(
        bool ok, const QString &error, const QJsonObject &tokens) {
    if (!ok) {
        show_idle();
        set_message(error.isEmpty()
            ? QStringLiteral("Sign-in failed.")
            : error,
            true);
        return;
    }
    const auto email = tokens.value(QStringLiteral("email")).toString();
    const auto target = new_codex_auth_path(email);
    if (!save_codex_tokens(tokens, target)) {
        show_idle();
        set_message(QStringLiteral("Could not save the Codex account."), true);
        return;
    }
    reload();
    set_message(QStringLiteral("Codex account saved."), false);
    emit accounts_changed();
}

void CredentialsPage::set_message(const QString &text, bool error) {
    if (text.trimmed().isEmpty()) {
        message_->hide();
        return;
    }
    const auto tokens = setup_tokens(palette());
    message_->setText(text);
    message_->setStyleSheet(QStringLiteral("color: %1;")
        .arg(setup_color_css(error ? tokens.danger_text : tokens.selection_accent)));
    message_->show();
}

} // namespace lingtai::desktop
