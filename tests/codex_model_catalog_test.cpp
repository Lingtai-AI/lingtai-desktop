#include "codex_model_catalog.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtNetwork/QAbstractSocket>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::CodexModelOption;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

QString path_text(const fs::path &path) {
    return QString::fromStdString(path.string());
}

bool contains_slug(const QVector<CodexModelOption> &options, const QString &slug) {
    for (const auto &option : options) {
        if (option.slug == slug) return true;
    }
    return false;
}

bool wait_until(const std::function<bool()> &predicate, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) return true;
    }
    return predicate();
}

void pump_for(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

void write_http_response(QTcpSocket *socket, const QByteArray &body,
        const QByteArray &status = "200 OK") {
    socket->write("HTTP/1.1 " + status + "\r\n"
        "Content-Type: application/json\r\nConnection: close\r\n\r\n" + body);
    socket->flush();
    socket->disconnectFromHost();
}

void test_parse_filters_and_dedups() {
    // Custom "JSON" raw-string delimiter: the payload below deliberately
    // contains ")" immediately before a closing quote (see the
    // "not GPT-prefixed)" case), which would otherwise terminate a bare
    // R"(...)" literal early.
    const QByteArray bytes = R"JSON({
        "models": [
            {"slug": "gpt-6-alpha", "display_name": "GPT-6 Alpha", "hidden": true},
            {"slug": "gpt-6-beta", "display_name": "gpt-6 beta lowercase excluded"},
            {"slug": "not-gpt", "display_name": "Codex Mini (not GPT-prefixed)"},
            {"slug": "", "display_name": "GPT-Blank-Slug"},
            {"slug": "gpt-6-gamma"},
            {"slug": "gpt-6-alpha", "display_name": "GPT-6 Alpha Duplicate"},
            {"slug": "gpt-6-delta", "display_name": "GPT-6 Delta (padded, preserved verbatim) "},
            {"slug": "gpt-6-zeta", "display_name": " GPT-6 Leading"},
            {"slug": "o9-turbo", "display_name": "GPT-O9 Turbo"},
            {"slug": "gpt-5", "display_name": "Codex 5"},
            "not-an-object",
            {"slug": "gpt-6-epsilon", "display_name": 42}
        ]
    })JSON";
    const auto options = lingtai::desktop::parse_codex_model_catalog(bytes);
    require(options.size() == 3,
        "only slug-bearing exact-GPT-prefixed entries survive, deduplicated by slug");
    require(options[0].slug == QStringLiteral("gpt-6-alpha")
            && options[0].display_name == QStringLiteral("GPT-6 Alpha"),
        "first-seen slug wins and display_name/slug must round-trip exactly");
    require(options[1].slug == QStringLiteral("gpt-6-delta")
            && options[1].display_name
                == QStringLiteral("GPT-6 Delta (padded, preserved verbatim) "),
        "display_name must be preserved byte-for-byte, including trailing padding");
    require(!contains_slug(options, QStringLiteral("not-gpt")),
        "a non-GPT-prefixed display_name must never be retained");
    require(!contains_slug(options, QStringLiteral("gpt-6-beta")),
        "the GPT prefix check must be case-sensitive");
    require(!contains_slug(options, QStringLiteral("gpt-6-zeta")),
        "a leading space before GPT must still fail the exact-prefix check");
    require(contains_slug(options, QStringLiteral("o9-turbo")),
        "an arbitrary slug that does not itself look like a gpt id must be "
        "accepted once its display_name starts with GPT: filtering is by "
        "display_name alone, never by slug shape");
    require(!contains_slug(options, QStringLiteral("gpt-5")),
        "a gpt-looking slug must still be excluded when its display_name is "
        "not GPT-prefixed: filtering is by display_name alone, never by slug "
        "shape");
}

void test_parse_rejects_malformed_or_empty() {
    require(lingtai::desktop::parse_codex_model_catalog("{not json").isEmpty(),
        "malformed JSON must parse to an empty (fail-open) list");
    require(lingtai::desktop::parse_codex_model_catalog("{}").isEmpty(),
        "a missing models array must parse to an empty list");
    require(lingtai::desktop::parse_codex_model_catalog(R"({"models": "nope"})").isEmpty(),
        "a non-array models value must parse to an empty list");
    require(lingtai::desktop::parse_codex_model_catalog(R"({"models": []})").isEmpty(),
        "an empty models array must parse to an empty list");
    require(lingtai::desktop::parse_codex_model_catalog("").isEmpty(),
        "an empty body must parse to an empty list");
}

void test_default_catalog_is_stable_and_offline() {
    const auto fallback = lingtai::desktop::default_codex_model_catalog();
    require(!fallback.isEmpty(), "the offline fallback must never be empty");
    auto seen = QVector<QString>();
    for (const auto &option : fallback) {
        require(!option.slug.isEmpty(), "every fallback slug must be non-empty");
        require(option.display_name == option.slug,
            "offline (no display_name available) must label rows with their own slug");
        require(!seen.contains(option.slug), "fallback slugs must be unique");
        seen.push_back(option.slug);
    }
}

void test_write_cache_rejects_empty_and_invalid(const fs::path &fixture) {
    const auto global = path_text(fixture / "write-guards");
    const auto good = QVector<CodexModelOption>{
        {QStringLiteral("gpt-good"), QStringLiteral("GPT Good")},
    };
    require(lingtai::desktop::write_codex_model_cache(global, good),
        "seeding a good cache must succeed");
    const auto cache_path = lingtai::desktop::codex_model_cache_path(global);
    QFile before(cache_path);
    require(before.open(QIODevice::ReadOnly), "the seeded cache must be readable");
    const auto before_bytes = before.readAll();
    before.close();

    require(!lingtai::desktop::write_codex_model_cache(global, {}),
        "writing an empty options list must be rejected, never accepted as a "
        "valid (empty) cache that would destroy the last-good data");
    require(!lingtai::desktop::write_codex_model_cache(global, {
            CodexModelOption{QString(), QStringLiteral("GPT Blank Slug")},
        }), "writing an option with an empty slug must be rejected");
    require(!lingtai::desktop::write_codex_model_cache(global, {
            CodexModelOption{QStringLiteral("not-gpt-labeled"), QStringLiteral("Not GPT")},
        }), "writing an option whose display_name is not GPT-prefixed must be rejected");
    require(!lingtai::desktop::write_codex_model_cache(global, {
            CodexModelOption{QStringLiteral("gpt-dup"), QStringLiteral("GPT Dup")},
            CodexModelOption{QStringLiteral("gpt-dup"), QStringLiteral("GPT Dup Again")},
        }), "writing options with a duplicate slug must be rejected");

    QFile after(cache_path);
    require(after.open(QIODevice::ReadOnly), "the cache must remain readable");
    require(after.readAll() == before_bytes,
        "every rejected write must leave the previously-good cache byte-identical");
}

void test_cache_round_trip(const fs::path &fixture) {
    const auto global = path_text(fixture / "global-cache");
    const auto cache_path = lingtai::desktop::codex_model_cache_path(global);
    require(cache_path.endsWith(QStringLiteral("cache/codex-models.json")),
        "the cache must live under the injected global root's cache/ subdirectory");
    require(lingtai::desktop::read_codex_model_cache(global).isEmpty(),
        "reading a nonexistent cache must return an empty list, not throw");
    require(!QFile::exists(cache_path),
        "reading a nonexistent cache must not create anything on disk");

    const auto written = QVector<CodexModelOption>{
        {QStringLiteral("gpt-7-nova"), QStringLiteral("GPT-7 Nova")},
        {QStringLiteral("gpt-7-vega"), QStringLiteral("GPT-7 Vega")},
    };
    require(lingtai::desktop::write_codex_model_cache(global, written),
        "writing the cache under a fresh injected root must succeed");
    const auto read_back = lingtai::desktop::read_codex_model_cache(global);
    require(read_back.size() == 2 && read_back[0].slug == QStringLiteral("gpt-7-nova")
            && read_back[0].display_name == QStringLiteral("GPT-7 Nova")
            && read_back[1].slug == QStringLiteral("gpt-7-vega"),
        "the cache must round-trip slug/display_name pairs exactly, in order");

    QFile corrupt(cache_path);
    require(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "the fixture must be able to overwrite its own cache file");
    corrupt.write("{not json");
    corrupt.close();
    require(lingtai::desktop::read_codex_model_cache(global).isEmpty(),
        "a corrupted cache file must fail open to an empty list, not throw");
}

void test_seed_prefers_cache_over_default(const fs::path &fixture) {
    const auto empty_global = path_text(fixture / "seed-empty");
    const auto seeded_default = lingtai::desktop::seed_codex_model_catalog(empty_global);
    require(seeded_default == lingtai::desktop::default_codex_model_catalog(),
        "with no cache present, the seed must be exactly the offline fallback");

    const auto cached_global = path_text(fixture / "seed-cached");
    const auto cached = QVector<CodexModelOption>{
        {QStringLiteral("gpt-9-override"), QStringLiteral("GPT-9 Override")},
    };
    require(lingtai::desktop::write_codex_model_cache(cached_global, cached),
        "seeding the cache fixture must succeed");
    const auto seeded_cached = lingtai::desktop::seed_codex_model_catalog(cached_global);
    require(seeded_cached.size() == 1
            && seeded_cached[0].slug == QStringLiteral("gpt-9-override"),
        "with a last-good cache present, the seed must prefer it over the offline fallback");
}

void test_fetcher_lifetime_cancellation(const fs::path &fixture) {
    // No event loop is ever run (no exec()/processEvents() drive the socket
    // state machine), so this never depends on a live endpoint answering.
    // It only proves construction, cancellation, and destruction while a
    // request is nominally in flight never crash and never touch the cache.
    const auto global = path_text(fixture / "fetcher-lifetime");
    quint16 closed_port = 0;
    {
        QTcpServer probe;
        require(probe.listen(QHostAddress::LocalHost),
            "must be able to find a free loopback port");
        closed_port = probe.serverPort();
    } // Closed immediately: this exact port now refuses connections, loopback-only.
    {
        auto *fetcher = new lingtai::desktop::CodexModelCatalogFetcher(global);
        fetcher->set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
            .arg(closed_port)));
        fetcher->refresh_async();
        fetcher->refresh_async(); // Re-entrant refresh must cancel the first cleanly.
        delete fetcher; // Destruction mid-flight must not crash.
    }
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    require(lingtai::desktop::read_codex_model_cache(global).isEmpty(),
        "no network round-trip ever completed, so the cache must remain untouched");
}

// The tests below drive a real Qt event loop against a real local
// QTcpServer on loopback: they exercise the actual async state machine
// (QNetworkAccessManager/QNetworkReply/QTimer), not just construction and
// destruction with no socket activity. Never any live/external endpoint.

void test_fetch_success_updates_cache_and_emits(const fs::path &fixture) {
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    const auto global = path_text(fixture / "fetch-success");
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost),
        "fetch-success: server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            connections.push_back(server.nextPendingConnection());
        }
    });

    lingtai::desktop::CodexModelCatalogFetcher fetcher(global);
    fetcher.set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort())));
    auto updates = QVector<QVector<CodexModelOption>>();
    QObject::connect(&fetcher, &lingtai::desktop::CodexModelCatalogFetcher::catalog_updated,
        &fetcher, [&](QVector<CodexModelOption> options) { updates.push_back(options); });

    fetcher.refresh_async();
    require(wait_until([&] { return !connections.isEmpty(); }, 2000),
        "fetch-success: the fetcher must open a real loopback connection to "
        "the injected source URL");
    write_http_response(connections[0],
        R"({"models": [{"slug": "gpt-9-live", "display_name": "GPT-9 Live"}]})");

    require(wait_until([&] { return !updates.isEmpty(); }, 2000),
        "fetch-success: a successful fetch must emit catalog_updated");
    require(updates.size() == 1 && updates[0].size() == 1
            && updates[0][0].slug == QStringLiteral("gpt-9-live")
            && updates[0][0].display_name == QStringLiteral("GPT-9 Live"),
        "fetch-success: catalog_updated must carry the parsed options from "
        "the real HTTP response");
    require(lingtai::desktop::read_codex_model_cache(global) == updates[0],
        "fetch-success: a successful fetch must persist the same options "
        "into the last-good cache");
}

void expect_fetch_failure_preserves_cache(const fs::path &fixture_root,
        const QByteArray &response_status, const QByteArray &response_body,
        const std::string &label) {
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    const auto global = path_text(fixture_root);
    const auto seed = QVector<CodexModelOption>{
        {QStringLiteral("gpt-seed"), QStringLiteral("GPT Seed")},
    };
    require(lingtai::desktop::write_codex_model_cache(global, seed),
        label + ": seeding the pre-existing good cache must succeed");
    const auto cache_path = lingtai::desktop::codex_model_cache_path(global);
    QFile before_file(cache_path);
    require(before_file.open(QIODevice::ReadOnly),
        label + ": the seeded cache must be readable before the failing fetch");
    const auto before_bytes = before_file.readAll();
    before_file.close();

    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost),
        label + ": server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            connections.push_back(server.nextPendingConnection());
        }
    });

    lingtai::desktop::CodexModelCatalogFetcher fetcher(global);
    fetcher.set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort())));
    auto updates = 0;
    QObject::connect(&fetcher, &lingtai::desktop::CodexModelCatalogFetcher::catalog_updated,
        &fetcher, [&](QVector<CodexModelOption>) { ++updates; });

    fetcher.refresh_async();
    require(wait_until([&] { return !connections.isEmpty(); }, 2000),
        label + ": the fetcher must open a real loopback connection");
    write_http_response(connections[0], response_body, response_status);

    pump_for(300); // Bounded window for a (wrongly) emitted signal to appear.
    require(updates == 0,
        label + ": a failing fetch must never emit catalog_updated");

    QFile after_file(cache_path);
    require(after_file.open(QIODevice::ReadOnly),
        label + ": the cache must remain readable after the failing fetch");
    require(after_file.readAll() == before_bytes,
        label + ": a failing fetch must leave the last-good cache byte-identical");
}

void test_fetch_http_error_fails_open(const fs::path &fixture) {
    expect_fetch_failure_preserves_cache(fixture / "fetch-http-error",
        "500 Internal Server Error", "oops", "http-error");
}

void test_fetch_malformed_body_fails_open(const fs::path &fixture) {
    expect_fetch_failure_preserves_cache(fixture / "fetch-malformed",
        "200 OK", "{not json", "malformed-body");
}

void test_fetch_empty_models_fails_open(const fs::path &fixture) {
    expect_fetch_failure_preserves_cache(fixture / "fetch-empty",
        "200 OK", "{}", "empty-body");
}

void test_fetch_oversize_body_fails_open(const fs::path &fixture) {
    const auto oversize_body = QByteArray(3 * 1024 * 1024, 'x');
    expect_fetch_failure_preserves_cache(fixture / "fetch-oversize",
        "200 OK", oversize_body, "oversize-body");
}

void test_fetch_timeout_fails_open_and_recovers(const fs::path &fixture) {
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    const auto global = path_text(fixture / "fetch-timeout");
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost),
        "timeout: server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            connections.push_back(server.nextPendingConnection());
        }
    });

    lingtai::desktop::CodexModelCatalogFetcher fetcher(global);
    fetcher.set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort())));
    fetcher.set_timeout_ms(150);
    auto updates = 0;
    QObject::connect(&fetcher, &lingtai::desktop::CodexModelCatalogFetcher::catalog_updated,
        &fetcher, [&](QVector<CodexModelOption>) { ++updates; });

    fetcher.refresh_async();
    require(wait_until([&] { return !connections.isEmpty(); }, 2000),
        "timeout: the fetcher must open a real loopback connection");
    // Never respond: only the fetcher's own bounded timeout ends this request.
    pump_for(500);
    require(updates == 0,
        "timeout: a request that never completes must fail open, never emit");
    require(lingtai::desktop::read_codex_model_cache(global).isEmpty(),
        "timeout: with no prior cache, it must remain empty after a timeout");

    auto connections2 = QVector<QTcpSocket *>();
    QObject::disconnect(&server, nullptr, nullptr, nullptr);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            connections2.push_back(server.nextPendingConnection());
        }
    });
    fetcher.refresh_async();
    require(wait_until([&] { return !connections2.isEmpty(); }, 2000),
        "timeout: a subsequent refresh must open a fresh connection");
    write_http_response(connections2[0],
        R"({"models": [{"slug": "gpt-after-timeout", "display_name": "GPT After Timeout"}]})");
    require(wait_until([&] { return updates == 1; }, 2000),
        "timeout: the fetcher must recover and succeed on a later refresh");
}

void test_fetcher_destroy_while_connected(const fs::path &fixture) {
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    const auto global = path_text(fixture / "destroy-while-connected");
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost),
        "destroy-while-connected: server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            connections.push_back(server.nextPendingConnection());
        }
    });

    auto *fetcher = new lingtai::desktop::CodexModelCatalogFetcher(global);
    fetcher->set_source_url(QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort())));
    fetcher->refresh_async();
    require(wait_until([&] { return !connections.isEmpty(); }, 2000),
        "destroy-while-connected: a real socket must be in flight before destruction");
    delete fetcher; // Must not crash even with a live, unanswered connection.
    pump_for(200);
    require(lingtai::desktop::read_codex_model_cache(global).isEmpty(),
        "destroy-while-connected: no response ever completed, so the cache "
        "must stay untouched");
}

void test_stale_timeout_never_aborts_a_superseding_request(const fs::path &fixture) {
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    const auto global = path_text(fixture / "superseded-timer");
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost),
        "superseded-timer: server must bind a loopback port");
    auto connections = QVector<QTcpSocket *>();
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            connections.push_back(server.nextPendingConnection());
        }
    });
    const auto url = QUrl(QStringLiteral("http://127.0.0.1:%1/models.json")
        .arg(server.serverPort()));

    lingtai::desktop::CodexModelCatalogFetcher fetcher(global);
    fetcher.set_source_url(url);
    fetcher.set_timeout_ms(1000);
    auto updates = QVector<QVector<CodexModelOption>>();
    QObject::connect(&fetcher, &lingtai::desktop::CodexModelCatalogFetcher::catalog_updated,
        &fetcher, [&](QVector<CodexModelOption> options) { updates.push_back(options); });

    fetcher.refresh_async(); // R1; T1 fires at ~t=1000ms. Connection #1 is never answered.
    require(wait_until([&] { return connections.size() >= 1; }, 2000),
        "superseded-timer: the first (superseded) request must actually connect");

    pump_for(500); // ~t=500ms.
    fetcher.refresh_async(); // R2 supersedes R1; T2 fires at ~t=1500ms.
    require(wait_until([&] { return connections.size() >= 2; }, 2000),
        "superseded-timer: the superseding request must open its own connection");

    pump_for(600); // ~t=1100ms: past T1's ~1000ms deadline, well before T2's ~1500ms.
    require(connections[1]->state() == QAbstractSocket::ConnectedState,
        "superseded-timer: the superseding request's connection must survive "
        "past the first (superseded) request's own stale timeout deadline");
    require(updates.isEmpty(), "superseded-timer: nothing should have completed yet");

    write_http_response(connections[1],
        R"({"models": [{"slug": "gpt-superseding", "display_name": "GPT Superseding"}]})");
    require(wait_until([&] { return !updates.isEmpty(); }, 2000),
        "superseded-timer: the superseding request must still be able to complete");
    require(updates.size() == 1 && updates[0].size() == 1
            && updates[0][0].slug == QStringLiteral("gpt-superseding"),
        "superseded-timer: the emitted catalog must come from the superseding "
        "request, never the stale superseded one");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: codex_model_catalog_test FIXTURE_ROOT\n";
        return 2;
    }
    try {
        const auto fixture = fs::path(argv[1]);
        std::error_code cleanup_error;
        fs::remove_all(fixture, cleanup_error);
        require(!cleanup_error, "fixture root must start clean");

        test_parse_filters_and_dedups();
        test_parse_rejects_malformed_or_empty();
        test_default_catalog_is_stable_and_offline();
        test_write_cache_rejects_empty_and_invalid(fixture);
        test_cache_round_trip(fixture);
        test_seed_prefers_cache_over_default(fixture);
        test_fetcher_lifetime_cancellation(fixture);
        test_fetch_success_updates_cache_and_emits(fixture);
        test_fetch_http_error_fails_open(fixture);
        test_fetch_malformed_body_fails_open(fixture);
        test_fetch_empty_models_fails_open(fixture);
        test_fetch_oversize_body_fails_open(fixture);
        test_fetch_timeout_fails_open_and_recovers(fixture);
        test_fetcher_destroy_while_connected(fixture);
        test_stale_timeout_never_aborts_a_superseding_request(fixture);

        fs::remove_all(fixture, cleanup_error);
        std::cout << "codex_model_catalog_test: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "codex_model_catalog_test FAILED: " << error.what() << "\n";
        return 1;
    }
}
