#include "codex_oauth.h"

#include <QtConcurrent/QtConcurrent>
#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QEventLoop>
#include <QtCore/QFutureWatcher>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QRandomGenerator>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <memory>

namespace lingtai::desktop {
namespace {

constexpr auto kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr auto kAuthIssuer = "https://auth.openai.com";
constexpr auto kAuthUrl = "https://auth.openai.com/oauth/authorize";
constexpr auto kTokenUrl = "https://auth.openai.com/oauth/token";
constexpr auto kScope =
    "openid profile email offline_access api.connectors.read api.connectors.invoke";
constexpr auto kOriginator = "codex_cli_rs";
constexpr auto kCallbackPath = "/auth/callback";
constexpr auto kDefaultPort = 1455;
constexpr auto kFallbackPort = 1457;

QString base64url(const QByteArray &bytes) {
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString random_base64url(int byte_count) {
    auto bytes = QByteArray(byte_count, Qt::Uninitialized);
    for (auto index = 0; index != byte_count; ++index) {
        bytes[index] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);
    }
    return base64url(bytes);
}

std::pair<QString, QString> generate_pkce() {
    const auto verifier = random_base64url(32);
    const auto challenge = base64url(
        QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256));
    return {verifier, challenge};
}

QString build_authorize_url(
        const QString &redirect_uri,
        const QString &challenge,
        const QString &state,
        bool force_login) {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClientId));
    query.addQueryItem(QStringLiteral("redirect_uri"), redirect_uri);
    query.addQueryItem(QStringLiteral("scope"), QString::fromLatin1(kScope));
    query.addQueryItem(QStringLiteral("code_challenge"), challenge);
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("id_token_add_organizations"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("codex_cli_simplified_flow"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("state"), state);
    query.addQueryItem(QStringLiteral("originator"), QString::fromLatin1(kOriginator));
    if (force_login) {
        query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("login"));
    }
    QUrl url(QString::fromLatin1(kAuthUrl));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QString jwt_email(const QString &token) {
    const auto parts = token.split(QLatin1Char('.'));
    if (parts.size() < 2) return {};
    auto payload = QByteArray::fromBase64(
        parts[1].toUtf8(), QByteArray::Base64UrlEncoding);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const auto object = document.object();
    if (const auto email = object.value(QStringLiteral("email")); email.isString()) {
        return email.toString();
    }
    const auto profile = object.value(QStringLiteral("https://api.openai.com/profile"));
    if (profile.isObject()) {
        return profile.toObject().value(QStringLiteral("email")).toString();
    }
    return {};
}

QJsonObject exchange_code_for_tokens(
        QNetworkAccessManager *network,
        const QString &code,
        const QString &verifier,
        const QString &redirect_uri,
        QString *error) {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    form.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClientId));
    form.addQueryItem(QStringLiteral("code"), code);
    form.addQueryItem(QStringLiteral("code_verifier"), verifier);
    form.addQueryItem(QStringLiteral("redirect_uri"), redirect_uri);
    QNetworkRequest request(QUrl(QString::fromLatin1(kTokenUrl)));
    request.setHeader(
        QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/x-www-form-urlencoded"));
    auto *reply = network->post(
        request, form.query(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const auto body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        *error = reply->errorString();
        reply->deleteLater();
        return {};
    }
    reply->deleteLater();
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Could not parse token response.");
        return {};
    }
    const auto raw = document.object();
    const auto access = raw.value(QStringLiteral("access_token")).toString();
    const auto refresh = raw.value(QStringLiteral("refresh_token")).toString();
    if (refresh.trimmed().isEmpty()) {
        *error = QStringLiteral("Token response did not include a refresh token.");
        return {};
    }
    auto email = jwt_email(raw.value(QStringLiteral("id_token")).toString());
    if (email.isEmpty()) {
        email = jwt_email(access);
    }
    const auto expires_in = raw.value(QStringLiteral("expires_in")).toInteger();
    QJsonObject tokens;
    tokens.insert(QStringLiteral("access_token"), access);
    tokens.insert(QStringLiteral("refresh_token"), refresh);
    tokens.insert(QStringLiteral("expires_at"),
        QDateTime::currentSecsSinceEpoch() + expires_in);
    tokens.insert(QStringLiteral("email"), email);
    return tokens;
}

struct DeviceCodeState {
    QString verification_url;
    QString user_code;
    QString device_auth_id;
    int interval_seconds = 5;
};

DeviceCodeState request_device_code(QNetworkAccessManager *network, QString *error) {
    QJsonObject payload;
    payload.insert(QStringLiteral("client_id"), QString::fromLatin1(kClientId));
    QNetworkRequest request(QUrl(QString::fromLatin1(kAuthIssuer)
        + QStringLiteral("/api/accounts/deviceauth/usercode")));
    request.setHeader(
        QNetworkRequest::ContentTypeHeader,
        QStringLiteral("application/json"));
    auto *reply = network->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const auto body = reply->readAll();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
        *error = status == 404
            ? QStringLiteral("Device code login is unavailable; use browser sign-in.")
            : QStringLiteral("Device code request failed.");
        reply->deleteLater();
        return {};
    }
    reply->deleteLater();
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Could not parse device code response.");
        return {};
    }
    const auto object = document.object();
    DeviceCodeState state;
    state.device_auth_id = object.value(QStringLiteral("device_auth_id")).toString();
    state.user_code = object.value(QStringLiteral("user_code")).toString();
    if (state.user_code.isEmpty()) {
        state.user_code = object.value(QStringLiteral("usercode")).toString();
    }
    if (state.device_auth_id.isEmpty() || state.user_code.isEmpty()) {
        *error = QStringLiteral("Device code response was incomplete.");
        return {};
    }
    if (const auto interval = object.value(QStringLiteral("interval")); interval.isDouble()) {
        state.interval_seconds = std::max(1, interval.toInt());
    }
    state.verification_url = QString::fromLatin1(kAuthIssuer) + QStringLiteral("/codex/device");
    return state;
}

QJsonObject complete_device_auth(
        QNetworkAccessManager *network,
        const DeviceCodeState &device,
        bool *cancelled,
        QString *error) {
    const auto deadline = QDateTime::currentDateTimeUtc().addSecs(15 * 60);
    const auto poll_url = QString::fromLatin1(kAuthIssuer)
        + QStringLiteral("/api/accounts/deviceauth/token");
    const auto redirect_uri = QString::fromLatin1(kAuthIssuer)
        + QStringLiteral("/deviceauth/callback");
    while (QDateTime::currentDateTimeUtc() < deadline) {
        if (*cancelled) {
            *error = QStringLiteral("Sign-in cancelled.");
            return {};
        }
        QJsonObject payload;
        payload.insert(QStringLiteral("device_auth_id"), device.device_auth_id);
        payload.insert(QStringLiteral("user_code"), device.user_code);
        QNetworkRequest request{QUrl{poll_url}};
        request.setHeader(
            QNetworkRequest::ContentTypeHeader,
            QStringLiteral("application/json"));
        auto *reply = network->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const auto body = reply->readAll();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status >= 200 && status < 300) {
            QJsonParseError parse_error;
            const auto document = QJsonDocument::fromJson(body, &parse_error);
            if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
                *error = QStringLiteral("Could not parse device sign-in response.");
                return {};
            }
            const auto object = document.object();
            const auto code = object.value(QStringLiteral("authorization_code")).toString();
            const auto verifier = object.value(QStringLiteral("code_verifier")).toString();
            if (code.isEmpty() || verifier.isEmpty()) {
                *error = QStringLiteral("Device sign-in response was incomplete.");
                return {};
            }
            return exchange_code_for_tokens(network, code, verifier, redirect_uri, error);
        }
        if (status == 403 || status == 404) {
            QThread::sleep(static_cast<unsigned long>(device.interval_seconds));
            continue;
        }
        *error = QStringLiteral("Device sign-in failed.");
        return {};
    }
    *error = QStringLiteral("Device sign-in timed out.");
    return {};
}

} // namespace

class CodexOAuthFlow::Private final : public QObject {
public:
    explicit Private(CodexOAuthFlow *owner)
    : owner_(owner) {}

    void start(CodexOAuthFlow::Mode mode, bool force_login) {
        cancel();
        cancelled_ = false;
        network_ = std::make_unique<QNetworkAccessManager>();
        if (mode == CodexOAuthFlow::Mode::Browser) {
            start_browser(force_login);
        } else {
            start_device_code();
        }
    }

    void cancel() {
        cancelled_ = true;
        if (server_) {
            server_->close();
            server_.reset();
        }
        if (timeout_) {
            timeout_->stop();
            timeout_.reset();
        }
    }

private:
    void emit_status(const QString &message) {
        emit owner_->status_changed(message);
    }

    void emit_failure(const QString &error) {
        emit owner_->finished(false, error, {});
    }

    void emit_success(const QJsonObject &tokens) {
        emit owner_->finished(true, {}, tokens);
    }

    void start_browser(bool force_login) {
        const auto pkce = generate_pkce();
        verifier_ = pkce.first;
        const auto challenge = pkce.second;
        const auto state = random_base64url(32);

        server_ = std::make_unique<QTcpServer>();
        auto listen_port = kDefaultPort;
        if (!server_->listen(QHostAddress::LocalHost, listen_port)) {
            listen_port = kFallbackPort;
            if (!server_->listen(QHostAddress::LocalHost, listen_port)) {
                emit_failure(QStringLiteral("Could not start the local sign-in listener."));
                return;
            }
        }
        const auto redirect_uri = QStringLiteral("http://localhost:%1%2")
            .arg(server_->serverPort())
            .arg(QString::fromLatin1(kCallbackPath));
        const auto auth_url = build_authorize_url(redirect_uri, challenge, state, force_login);
        emit owner_->auth_url_ready(auth_url, redirect_uri);
        emit_status(QStringLiteral("Waiting for browser sign-in…"));
        QDesktopServices::openUrl(QUrl(auth_url));

        timeout_ = std::make_unique<QTimer>();
        timeout_->setSingleShot(true);
        timeout_->setInterval(5 * 60 * 1000);
        connect(timeout_.get(), &QTimer::timeout, this, [this] {
            emit_failure(QStringLiteral("Sign-in timed out."));
            cancel();
        });
        timeout_->start();

        connect(server_.get(), &QTcpServer::newConnection, this, [this, state, redirect_uri] {
            while (server_ && server_->hasPendingConnections()) {
                auto *socket = server_->nextPendingConnection();
                if (!socket) continue;
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, state, redirect_uri] {
                    handle_callback(socket, state, redirect_uri);
                });
            }
        });
    }

    void handle_callback(QTcpSocket *socket, const QString &state, const QString &redirect_uri) {
        const auto request = QString::fromUtf8(socket->readAll());
        const auto first_line = request.section(QLatin1Char('\n'), 0, 0);
        const auto target = first_line.section(QLatin1Char(' '), 1, 1);
        const auto path_and_query = target.section(QLatin1Char('?'), 0, 0);
        if (path_and_query != QString::fromLatin1(kCallbackPath)) {
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }
        const auto query_string = target.section(QLatin1Char('?'), 1);
        QUrlQuery query(query_string);
        const auto response_header = QStringLiteral("HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
        if (const auto oauth_error = query.queryItemValue(QStringLiteral("error"));
                !oauth_error.isEmpty()) {
            socket->write(response_header.toUtf8());
            socket->write("<html><body><h1>Sign-in failed</h1></body></html>");
            socket->disconnectFromHost();
            socket->deleteLater();
            emit_failure(oauth_error);
            cancel();
            return;
        }
        if (query.queryItemValue(QStringLiteral("state")) != state) {
            socket->write(response_header.toUtf8());
            socket->write("<html><body><h1>Sign-in failed</h1></body></html>");
            socket->disconnectFromHost();
            socket->deleteLater();
            emit_failure(QStringLiteral("Sign-in state mismatch."));
            cancel();
            return;
        }
        const auto code = query.queryItemValue(QStringLiteral("code"));
        if (code.isEmpty()) {
            socket->write(response_header.toUtf8());
            socket->write("<html><body><h1>Sign-in failed</h1></body></html>");
            socket->disconnectFromHost();
            socket->deleteLater();
            emit_failure(QStringLiteral("Missing authorization code."));
            cancel();
            return;
        }
        socket->write(response_header.toUtf8());
        socket->write("<html><body><h1>Sign-in successful</h1>"
            "<p>You can close this tab and return to LingTai.</p></body></html>");
        socket->disconnectFromHost();
        socket->deleteLater();
        cancel();
        emit_status(QStringLiteral("Completing sign-in…"));
        QString error;
        const auto tokens = exchange_code_for_tokens(
            network_.get(), code, verifier_, redirect_uri, &error);
        if (tokens.isEmpty()) {
            emit_failure(error);
            return;
        }
        emit_success(tokens);
    }

    void start_device_code() {
        QString error;
        const auto device = request_device_code(network_.get(), &error);
        if (device.verification_url.isEmpty()) {
            emit_failure(error);
            return;
        }
        emit owner_->device_code_ready(device.verification_url, device.user_code);
        emit_status(QStringLiteral("Waiting for device approval…"));
        auto cancelled = &cancelled_;
        auto *owner = owner_;
        (void)QtConcurrent::run([device, cancelled, owner]() {
            auto network = std::make_unique<QNetworkAccessManager>();
            QString error;
            const auto tokens = complete_device_auth(
                network.get(), device, cancelled, &error);
            QTimer::singleShot(0, owner, [owner, tokens, error]() {
                if (tokens.isEmpty()) {
                    emit owner->finished(false, error, {});
                } else {
                    emit owner->finished(true, {}, tokens);
                }
            });
        });
    }

    CodexOAuthFlow *owner_ = nullptr;
    std::unique_ptr<QNetworkAccessManager> network_;
    std::unique_ptr<QTcpServer> server_;
    std::unique_ptr<QTimer> timeout_;
    QString verifier_;
    bool cancelled_ = false;
};

CodexOAuthFlow::CodexOAuthFlow(QObject *parent)
: QObject(parent)
, d_(new Private(this)) {}

void CodexOAuthFlow::start(Mode mode, bool force_login) {
    d_->start(mode, force_login);
}

void CodexOAuthFlow::cancel() {
    d_->cancel();
}

} // namespace lingtai::desktop
