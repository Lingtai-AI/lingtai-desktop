#include "codex_model_catalog.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace lingtai::desktop {
namespace {

constexpr auto kCodexModelCatalogUrl =
    "https://raw.githubusercontent.com/openai/codex/main/codex-rs/models-manager/models.json";
constexpr qint64 kMaxCodexModelCatalogBytes = 2 * 1024 * 1024;
constexpr int kCodexModelCatalogTimeoutMs = 5000;

} // namespace

QVector<CodexModelOption> parse_codex_model_catalog(const QByteArray &json_bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json_bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const auto models = document.object().value(QLatin1String("models"));
    if (!models.isArray()) {
        return {};
    }
    auto result = QVector<CodexModelOption>();
    auto seen = QSet<QString>();
    for (const auto &entry : models.toArray()) {
        if (!entry.isObject()) continue;
        const auto object = entry.toObject();
        const auto slug_value = object.value(QLatin1String("slug"));
        const auto display_value = object.value(QLatin1String("display_name"));
        if (!slug_value.isString() || !display_value.isString()) continue;
        const auto slug = slug_value.toString().trimmed();
        const auto display_name = display_value.toString();
        // Case-sensitive by construction: QString::startsWith defaults to
        // Qt::CaseSensitive, and display_name is stored/compared unmodified.
        if (slug.isEmpty() || !display_name.startsWith(QLatin1String("GPT"))) continue;
        if (seen.contains(slug)) continue;
        seen.insert(slug);
        result.push_back(CodexModelOption{slug, display_name});
    }
    return result;
}

QVector<CodexModelOption> default_codex_model_catalog() {
    // Offline fallback only: no live display_name is available, so the label
    // mirrors the slug exactly as this picker showed before the public
    // catalog existed.
    return {
        CodexModelOption{QStringLiteral("gpt-5.6-sol"), QStringLiteral("gpt-5.6-sol")},
        CodexModelOption{QStringLiteral("gpt-5.6-terra"), QStringLiteral("gpt-5.6-terra")},
        CodexModelOption{QStringLiteral("gpt-5.6-luna"), QStringLiteral("gpt-5.6-luna")},
        CodexModelOption{QStringLiteral("gpt-5.5"), QStringLiteral("gpt-5.5")},
    };
}

QString codex_model_cache_path(const QString &global_dir) {
    return QDir(global_dir).filePath(QStringLiteral("cache/codex-models.json"));
}

QVector<CodexModelOption> read_codex_model_cache(const QString &global_dir) {
    QFile file(codex_model_cache_path(global_dir));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    if (file.size() > kMaxCodexModelCatalogBytes) {
        return {};
    }
    // Belt-and-suspenders bound in case `size()` is ever unreliable (e.g. a
    // special file): never actually read more than the cap into memory.
    const auto bytes = file.read(kMaxCodexModelCatalogBytes + 1);
    if (bytes.size() > kMaxCodexModelCatalogBytes) {
        return {};
    }
    return parse_codex_model_catalog(bytes);
}

bool write_codex_model_cache(const QString &global_dir, const QVector<CodexModelOption> &options) {
    // An empty catalog almost always means "nothing fetched yet" or "the
    // caller has nothing worth keeping"; writing it would destroy whatever
    // last-good data is already on disk, so it is rejected outright rather
    // than accepted as a valid (if pointless) cache state.
    if (options.isEmpty()) {
        return false;
    }
    QJsonArray array;
    for (const auto &option : options) {
        array.push_back(QJsonObject{
            {QStringLiteral("slug"), option.slug},
            {QStringLiteral("display_name"), option.display_name},
        });
    }
    const auto document = QJsonDocument(QJsonObject{{QStringLiteral("models"), array}});
    const auto bytes = document.toJson(QJsonDocument::Compact);
    // Round-trip through the same pure parser used on read: anything that
    // would not survive it unchanged (a blank/duplicate slug, a non-GPT
    // label, wrong order) is invalid input and rejected outright, so the
    // last-good cache is either replaced by an equally-valid catalog or left
    // alone — never partially or incorrectly overwritten.
    if (parse_codex_model_catalog(bytes) != options) {
        return false;
    }
    const auto path = codex_model_cache_path(global_dir);
    QDir().mkpath(QFileInfo(path).absolutePath());
    // QSaveFile writes to a sibling temp file and only replaces the real
    // path on a successful commit(): a crash, write error, or failed commit
    // leaves the previous last-good cache byte-identical. No direct-write
    // fallback: if the atomic path fails, the write fails closed.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QVector<CodexModelOption> seed_codex_model_catalog(const QString &global_dir) {
    const auto cached = read_codex_model_cache(global_dir);
    return cached.isEmpty() ? default_codex_model_catalog() : cached;
}

CodexModelCatalogFetcher::CodexModelCatalogFetcher(QString global_dir, QObject *parent)
: QObject(parent)
, global_dir_(std::move(global_dir))
, source_url_(QString::fromLatin1(kCodexModelCatalogUrl))
, timeout_ms_(kCodexModelCatalogTimeoutMs) {}

CodexModelCatalogFetcher::~CodexModelCatalogFetcher() {
    abort_active();
}

void CodexModelCatalogFetcher::set_source_url(const QUrl &url) {
    source_url_ = url;
}

void CodexModelCatalogFetcher::set_timeout_ms(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

void CodexModelCatalogFetcher::abort_active() {
    if (!active_reply_) return;
    auto *reply = active_reply_.data();
    active_reply_.clear();
    reply->abort();
    reply->deleteLater();
}

void CodexModelCatalogFetcher::refresh_async() {
    abort_active();
    if (!network_) {
        network_ = std::make_unique<QNetworkAccessManager>();
    }
    auto *reply = network_->get(QNetworkRequest(source_url_));
    active_reply_ = reply;
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply](qint64 received, qint64) {
        if (reply && received > kMaxCodexModelCatalogBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handle_finished(reply);
    });
    // Bound to this exact reply via QPointer, not to whatever active_reply_
    // happens to be when the timer fires: if a later refresh_async() call
    // supersedes this request before the timeout elapses, `timed_reply` is
    // either already deleted (null) or simply no longer the active reply, so
    // a stale timer from an old request can never abort a newer, unrelated,
    // still-in-flight request. Parent-owned: this lambda is auto-disconnected
    // by Qt if `this` (the fetcher) is destroyed before it fires, so no
    // dangling access.
    QPointer<QNetworkReply> timed_reply(reply);
    QTimer::singleShot(timeout_ms_, this, [this, timed_reply] {
        if (timed_reply && timed_reply.data() == active_reply_.data()) {
            timed_reply->abort();
        }
    });
}

void CodexModelCatalogFetcher::handle_finished(QNetworkReply *reply) {
    if (reply != active_reply_.data()) {
        // A stale reply from a superseded/aborted request; release it only.
        reply->deleteLater();
        return;
    }
    active_reply_.clear();
    const auto network_error = reply->error();
    const auto body = reply->bytesAvailable() <= kMaxCodexModelCatalogBytes
        ? reply->readAll() : QByteArray();
    reply->deleteLater();
    if (network_error != QNetworkReply::NoError) {
        return; // Fail open: keep whatever last-good data the caller has.
    }
    const auto parsed = parse_codex_model_catalog(body);
    if (parsed.isEmpty()) {
        return; // Malformed/empty body: fail open, never clear good data.
    }
    write_codex_model_cache(global_dir_, parsed);
    emit catalog_updated(parsed);
}

} // namespace lingtai::desktop
