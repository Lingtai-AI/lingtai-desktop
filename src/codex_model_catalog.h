#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVector>

#include <memory>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

namespace lingtai::desktop {

// One public-catalog suggestion: `slug` is the persisted model id, exactly as
// published by the upstream catalog; `display_name` is the upstream label,
// kept byte-for-byte for presentation and never persisted.
struct CodexModelOption {
    QString slug;
    QString display_name;

    bool operator==(const CodexModelOption &other) const = default;
};

// Pure parse: keeps only entries whose `display_name` starts with the exact
// case-sensitive prefix "GPT" and have a non-empty `slug`, deduplicated by
// slug in first-seen order. Any other field (numeric generation, visibility,
// account entitlement) is ignored: presence in this list is a public
// suggestion only, never proof an account may use the model. Malformed,
// non-object, or missing-`models` JSON returns an empty list so callers can
// treat that as a failed fetch and fail open on existing good data.
[[nodiscard]] QVector<CodexModelOption> parse_codex_model_catalog(const QByteArray &json_bytes);

// The compiled-in offline fallback used when there is no local cache yet and
// no successful fetch has completed.
[[nodiscard]] QVector<CodexModelOption> default_codex_model_catalog();

// The last-good cache location under the already-injected Desktop global
// root (see `lingtai_global_dir()` / `expand_lingtai_path()`); callers supply
// that root so no path is ever hard-coded to a real HOME during tests.
[[nodiscard]] QString codex_model_cache_path(const QString &global_dir);

// Reads and parses the last-good cache; a missing or unreadable file returns
// an empty list without creating or touching anything on disk.
[[nodiscard]] QVector<CodexModelOption> read_codex_model_cache(const QString &global_dir);

// Overwrites the last-good cache with an already-filtered option list.
// Returns false only on a local write failure; it never partially writes.
bool write_codex_model_cache(const QString &global_dir, const QVector<CodexModelOption> &options);

// The synchronous seed used before any async refresh completes: the last-good
// cache when present, otherwise the compiled-in offline fallback. Never
// empty.
[[nodiscard]] QVector<CodexModelOption> seed_codex_model_catalog(const QString &global_dir);

// A small, per-owner async fetcher for the fixed public Codex model catalog.
// It performs one bounded (~5s timeout, ~2MiB response cap) GET, parses the
// body, and on success alone updates the last-good cache and emits
// `catalog_updated`. Any failure (network error, timeout, oversize response,
// malformed/empty body) fails open: it neither touches the cache nor emits,
// leaving whatever last-good data the caller already has. It owns no global
// mutable state; each owner (e.g. one preset editor page) should hold its own
// instance parented to itself so an in-flight reply is aborted and released
// when the owner is destroyed.
class CodexModelCatalogFetcher final : public QObject {
    Q_OBJECT

public:
    explicit CodexModelCatalogFetcher(QString global_dir, QObject *parent = nullptr);
    ~CodexModelCatalogFetcher() override;

    // Test/local seam: overrides the fixed public source URL. Production
    // code never calls this; it exists so tests point at an injected local
    // endpoint instead of gating on the live upstream host.
    void set_source_url(const QUrl &url);

    // Test/local seam: overrides the fixed ~5s production timeout so tests
    // can exercise timeout and request-supersession behavior quickly.
    // Production code never calls this.
    void set_timeout_ms(int timeout_ms);

    // Starts one bounded fetch, cancelling any reply already in flight.
    void refresh_async();

signals:
    void catalog_updated(QVector<CodexModelOption> options);

private:
    void handle_finished(QNetworkReply *reply);
    void abort_active();

    QString global_dir_;
    QUrl source_url_;
    int timeout_ms_;
    std::unique_ptr<QNetworkAccessManager> network_;
    QPointer<QNetworkReply> active_reply_;
};

} // namespace lingtai::desktop
