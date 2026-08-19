#include "credentials_model.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>

namespace lingtai::desktop {
namespace {

QString json_string(const QJsonValue &value) {
    return value.isString() ? value.toString() : QString();
}

QJsonObject load_json_file(const QString &path, bool *ok) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return {};
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        *ok = false;
        return {};
    }
    *ok = true;
    return document.object();
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
    return json_string(document.object().value(QLatin1String("email")));
}

CodexAccount read_codex_account(const QString &path, const QString &ref, bool legacy) {
    CodexAccount account;
    account.path = path;
    account.ref = ref;
    account.legacy = legacy;
    auto ok = false;
    const auto object = load_json_file(path, &ok);
    if (!ok) return account;
    account.email = json_string(object.value(QLatin1String("email")));
    account.label = json_string(object.value(QLatin1String("label"))).trimmed();
    if (account.email.isEmpty()) {
        account.email = jwt_email(json_string(object.value(QLatin1String("access_token"))));
    }
    account.valid = !json_string(object.value(QLatin1String("refresh_token"))).trimmed().isEmpty();
    return account;
}

QString shorten_home(const QString &path) {
    const auto home = QDir::homePath();
    if (path.startsWith(home + QLatin1Char('/'))) {
        return QStringLiteral("~/") + path.mid(home.size() + 1);
    }
    return path;
}

QString codex_account_slug(const QString &email) {
    auto base = email;
    const auto at = base.indexOf(QLatin1Char('@'));
    if (at > 0) {
        base = base.left(at);
    }
    if (base.trimmed().isEmpty()) {
        base = QStringLiteral("codex-account");
    }
    QString slug;
    slug.reserve(base.size());
    for (const auto ch : base.toLower()) {
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
                || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
            slug.push_back(ch);
        } else {
            slug.push_back(QLatin1Char('-'));
        }
    }
    slug = slug.trimmed();
    while (slug.startsWith(QLatin1Char('-'))) slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('-'))) slug.chop(1);
    if (slug.isEmpty()) {
        slug = QStringLiteral("account");
    }
    return slug;
}

} // namespace

QVector<CodexAccount> list_codex_accounts() {
    auto accounts = QVector<CodexAccount>();
    const auto global = lingtai_global_dir();
    const auto legacy = QDir(global).filePath(QStringLiteral("codex-auth.json"));
    if (QFileInfo::exists(legacy)) {
        accounts.push_back(read_codex_account(legacy, {}, true));
    }
    QDir extra(QDir(global).filePath(QStringLiteral("codex-auth")));
    auto files = extra.entryList({QStringLiteral("*.json")}, QDir::Files);
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
        const auto path = extra.filePath(file);
        accounts.push_back(read_codex_account(path, shorten_home(path), false));
    }
    return accounts;
}

QString codex_account_display_name(const CodexAccount &account) {
    if (!account.label.isEmpty()) return account.label;
    if (!account.email.isEmpty()) return account.email;
    if (account.legacy) return QStringLiteral("default");
    return QFileInfo(account.path).completeBaseName();
}

QString new_codex_auth_path(const QString &email) {
    const auto global = lingtai_global_dir();
    const auto legacy = QDir(global).filePath(QStringLiteral("codex-auth.json"));
    if (!QFileInfo::exists(legacy)) {
        return legacy;
    }
    const auto dir = QDir(global).filePath(QStringLiteral("codex-auth"));
    const auto slug = codex_account_slug(email);
    auto candidate = QDir(dir).filePath(slug + QStringLiteral(".json"));
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }
    for (auto n = 2; ; ++n) {
        candidate = QDir(dir).filePath(
            slug + QLatin1Char('-') + QString::number(n) + QStringLiteral(".json"));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
}

bool save_codex_tokens(const QJsonObject &tokens, const QString &absolute_path) {
    QDir().mkpath(QFileInfo(absolute_path).absolutePath());
    QFile file(absolute_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(tokens).toJson(QJsonDocument::Indented));
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool remove_codex_account_file(const QString &absolute_path) {
    if (!QFileInfo::exists(absolute_path)) return true;
    return QFile::remove(absolute_path);
}

int count_valid_codex_accounts() {
    auto count = 0;
    for (const auto &account : list_codex_accounts()) {
        if (account.valid) ++count;
    }
    return count;
}

CodexCredentialsSummary codex_credentials_summary() {
    CodexCredentialsSummary summary;
    for (const auto &account : list_codex_accounts()) {
        summary.has_accounts = true;
        if (!account.valid) continue;
        summary.has_valid = true;
        if (summary.primary_label.isEmpty()) {
            summary.primary_label = codex_account_display_name(account);
        }
    }
    return summary;
}

} // namespace lingtai::desktop
