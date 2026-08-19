#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>

namespace lingtai::desktop {

class CodexOAuthFlow final : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        Browser,
        DeviceCode,
    };

    explicit CodexOAuthFlow(QObject *parent = nullptr);

    void start(Mode mode, bool force_login);
    void cancel();

signals:
    void auth_url_ready(const QString &url, const QString &redirect_uri);
    void device_code_ready(const QString &verification_url, const QString &user_code);
    void status_changed(const QString &message);
    void finished(bool ok, const QString &error, const QJsonObject &tokens);

private:
    class Private;
    Private *d_ = nullptr;
};

} // namespace lingtai::desktop
