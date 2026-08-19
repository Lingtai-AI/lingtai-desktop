#pragma once

#include "codex_oauth.h"

#include <QtCore/QJsonObject>
#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;

namespace lingtai::desktop {

class CredentialsPage final : public QWidget {
    Q_OBJECT

public:
    explicit CredentialsPage(QWidget *parent = nullptr);

    void reload();
    void set_back_label(const QString &label);

signals:
    void back_requested();
    void accounts_changed();

protected:
    void changeEvent(QEvent *event) override;

private:
    void rebuild_accounts();
    void apply_chrome();
    void refresh_hero();
    void show_idle();
    void show_method_chooser();
    void start_oauth(CodexOAuthFlow::Mode mode);
    void finish_oauth(bool ok, const QString &error, const QJsonObject &tokens);
    void set_message(const QString &text, bool error);

    QPushButton *back_ = nullptr;
    QWidget *hero_ = nullptr;
    QLabel *hero_avatar_ = nullptr;
    QLabel *hero_kicker_ = nullptr;
    QLabel *hero_title_ = nullptr;
    QLabel *hero_detail_ = nullptr;
    QWidget *list_ = nullptr;
    QWidget *method_panel_ = nullptr;
    QWidget *progress_panel_ = nullptr;
    QLabel *message_ = nullptr;
    QLabel *progress_label_ = nullptr;
    QLabel *device_code_label_ = nullptr;
    QPushButton *open_browser_ = nullptr;
    CodexOAuthFlow *oauth_ = nullptr;
    bool oauth_force_login_ = false;
    bool applying_chrome_ = false;
};

} // namespace lingtai::desktop
