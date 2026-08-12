#include "compatibility_probe.h"
#include "project_attachment.h"
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>
namespace lingtai::desktop {
namespace {
namespace fs = std::filesystem;
enum class FileState { present, missing, unreadable };
struct FileObservation {
    FileState state = FileState::unreadable;
    fs::path path; std::string bytes;
};
void add(CompatibilityReport &report, CompatibilityFindingKind kind) {
    report.findings.push_back({kind});
}
bool retain_file_problem(CompatibilityReport &report,
        const FileObservation &file, CompatibilityFindingKind missing,
        CompatibilityFindingKind unreadable) {
    if (file.state == FileState::present) return false;
    add(report, file.state == FileState::missing ? missing : unreadable);
    return true;
}
bool is_blocker(CompatibilityFindingKind kind) {
    switch (kind) {
    case CompatibilityFindingKind::probe_failed:
    case CompatibilityFindingKind::agent_unavailable:
    case CompatibilityFindingKind::init_missing:
    case CompatibilityFindingKind::init_unreadable:
    case CompatibilityFindingKind::init_malformed:
    case CompatibilityFindingKind::init_not_object:
    case CompatibilityFindingKind::init_manifest_missing:
    case CompatibilityFindingKind::init_manifest_not_object:
    case CompatibilityFindingKind::capabilities_shape_unknown:
    case CompatibilityFindingKind::capability_conflict:
        return true;
    default:
        return false;
    }
}
FileObservation read_file(const fs::path &path) {
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error) {
        return {error == std::errc::no_such_file_or_directory
                ? FileState::missing : FileState::unreadable, path, {}};
    }
    if (!fs::exists(status)) return {FileState::missing, path, {}};
    if (!fs::is_regular_file(status)) return {FileState::unreadable, path, {}};
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {FileState::unreadable, path, {}};
    std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad()) return {FileState::unreadable, path, {}};
    return {FileState::present, path, std::move(bytes)};
}
FileObservation read_scoped_file(
        const ProjectAttachment &attachment, const fs::path &relative_path) {
    const auto resolved = attachment.resolve(relative_path);
    if (!resolved) {
        return {resolved.failure == ProjectPathFailure::target_not_found
                ? FileState::missing : FileState::unreadable, {}, {}};
    }
    return read_file(resolved.path);
}
std::optional<QJsonValue> parse_json(const std::string &bytes) {
    if (bytes.size() > static_cast<std::size_t>(
            std::numeric_limits<qsizetype>::max())) return std::nullopt;
    QJsonParseError error;
    const auto value = QJsonValue::fromJson(
        QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
        &error);
    if (error.error != QJsonParseError::NoError) return std::nullopt;
    return value;
}
bool exact_string(const QJsonObject &object, const QString &key,
        const QString &expected) {
    const auto value = object.value(key);
    return value.isString() && value.toString() == expected;
}
bool exact_version_one(const QJsonObject &object) {
    const auto value = object.value(QStringLiteral("schema_version"));
    return value.isDouble() && value.toInteger(0) == 1;
}
std::string selected_string(const QJsonObject &object, const QString &key) {
    const auto value = object.value(key);
    if (!value.isString()) return {};
    const auto utf8 = value.toString().toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}
void inspect_receipt(
        CompatibilityReport &report, const fs::path &receipt_path) {
    if (!receipt_path.is_absolute()) {
        add(report, CompatibilityFindingKind::receipt_unreadable);
        return;
    }
    const auto file = read_file(receipt_path);
    if (retain_file_problem(report, file,
            CompatibilityFindingKind::receipt_missing,
            CompatibilityFindingKind::receipt_unreadable)) return;
    const auto parsed = parse_json(file.bytes);
    if (!parsed || !parsed->isObject()) {
        add(report, CompatibilityFindingKind::receipt_malformed);
        return;
    }
    const auto object = parsed->toObject();
    const auto schema_ok = exact_string(
        object,
        QStringLiteral("schema"),
        QStringLiteral("lingtai.tui.install/v1"));
    const auto version_ok = exact_version_one(object);
    if (!schema_ok) add(report, CompatibilityFindingKind::receipt_schema_unsupported);
    if (!version_ok) add(report, CompatibilityFindingKind::receipt_version_unsupported);
    if (schema_ok && version_ok) {
        report.install_receipt = InstallReceiptFacts{
            .install_method = selected_string(object, QStringLiteral("install_method")),
            .stamped_version = selected_string(object, QStringLiteral("stamped_version")),
            .resolved_commit = selected_string(object, QStringLiteral("resolved_commit")),
            .kernel_source = selected_string(object, QStringLiteral("kernel_source")),
            .kernel_version = selected_string(object, QStringLiteral("kernel_version")),
        };
    }
}
void inspect_raw_init(
        CompatibilityReport &report, const FileObservation &file) {
    if (retain_file_problem(report, file,
            CompatibilityFindingKind::init_missing,
            CompatibilityFindingKind::init_unreadable)) return;
    const auto parsed = parse_json(file.bytes);
    if (!parsed) {
        add(report, CompatibilityFindingKind::init_malformed);
        return;
    }
    if (!parsed->isObject()) {
        add(report, CompatibilityFindingKind::init_not_object);
        return;
    }
    const auto object = parsed->toObject();
    const auto manifest_value = object.value(QStringLiteral("manifest"));
    if (!object.contains(QStringLiteral("manifest"))) {
        add(report, CompatibilityFindingKind::init_manifest_missing);
        return;
    }
    if (!manifest_value.isObject()) {
        add(report, CompatibilityFindingKind::init_manifest_not_object);
        return;
    }
    RawInitFacts facts;
    const auto manifest = manifest_value.toObject();
    if (manifest.contains(QStringLiteral("capabilities"))) {
        const auto capabilities_value = manifest.value(QStringLiteral("capabilities"));
        if (!capabilities_value.isObject()) {
            facts.capability_alias = CapabilityAlias::shape_unknown;
            add(report, CompatibilityFindingKind::capabilities_shape_unknown);
        } else {
            const auto capabilities = capabilities_value.toObject();
            const auto has_bash = capabilities.contains(QStringLiteral("bash"));
            const auto has_shell = capabilities.contains(QStringLiteral("shell"));
            if (has_bash && has_shell) {
                if (capabilities.value(QStringLiteral("bash"))
                    == capabilities.value(QStringLiteral("shell"))) {
                    facts.capability_alias = CapabilityAlias::equal;
                    add(report, CompatibilityFindingKind::legacy_bash_alias);
                } else {
                    facts.capability_alias = CapabilityAlias::conflict;
                    add(report, CompatibilityFindingKind::capability_conflict);
                }
            } else if (has_bash) {
                facts.capability_alias = CapabilityAlias::bash_only;
                add(report, CompatibilityFindingKind::legacy_bash_alias);
            } else if (has_shell) {
                facts.capability_alias = CapabilityAlias::shell_only;
            }
        }
    }
    report.raw_init = std::move(facts);
}
void inspect_resolved_manifest(
        CompatibilityReport &report,
        const FileObservation &resolved_file,
        const FileObservation &init_file) {
    if (retain_file_problem(report, resolved_file,
            CompatibilityFindingKind::resolved_missing,
            CompatibilityFindingKind::resolved_unreadable)) return;
    const auto parsed = parse_json(resolved_file.bytes);
    if (!parsed || !parsed->isObject()) {
        add(report, CompatibilityFindingKind::resolved_malformed);
        return;
    }
    const auto object = parsed->toObject();
    const auto schema_ok = exact_string(
        object,
        QStringLiteral("schema"),
        QStringLiteral("lingtai.manifest.resolved/v1"));
    const auto version_ok = exact_version_one(object);
    const auto source_ok = exact_string(
        object,
        QStringLiteral("source"),
        QStringLiteral("kernel"));
    const auto manifest_ok = object.value(QStringLiteral("manifest")).isObject();
    if (!schema_ok) add(report, CompatibilityFindingKind::resolved_schema_unsupported);
    if (!version_ok) add(report, CompatibilityFindingKind::resolved_version_unsupported);
    if (!source_ok) add(report, CompatibilityFindingKind::resolved_source_unsupported);
    if (!manifest_ok) add(report, CompatibilityFindingKind::resolved_manifest_invalid);
    if (!schema_ok || !version_ok || !source_ok || !manifest_ok) {
        return;
    }
    if (init_file.state != FileState::present) {
        add(report, CompatibilityFindingKind::resolved_freshness_unavailable);
        return;
    }
    std::error_code init_error;
    std::error_code resolved_error;
    const auto init_time = fs::last_write_time(init_file.path, init_error);
    const auto resolved_time =
        fs::last_write_time(resolved_file.path, resolved_error);
    if (init_error || resolved_error) {
        add(report, CompatibilityFindingKind::resolved_freshness_unavailable);
    } else if (resolved_time < init_time) {
        add(report, CompatibilityFindingKind::resolved_stale);
    } else {
        report.resolved_manifest = ResolvedManifestFacts{};
    }
}
CompatibilityReport finish_report(
        CompatibilityReport report,
        bool agent_available) {
    const auto blocked = std::ranges::any_of(
        report.findings,
        [](const auto &finding) { return is_blocker(finding.kind); });
    report.disposition = blocked
        ? CompatibilityDisposition::blocked
        : (report.findings.empty()
              ? CompatibilityDisposition::compatible
              : CompatibilityDisposition::degraded);
    report.commands_allowed = agent_available
        && report.findings.empty()
        && report.install_receipt.has_value()
        && report.resolved_manifest.has_value()
        && report.raw_init.has_value();
    return report;
}
CompatibilityReport probe_impl(
        const ProjectAttachment &attachment,
        const std::optional<fs::path> &agent_relative_directory,
        const fs::path &install_receipt_path) {
    CompatibilityReport report;
    inspect_receipt(report, install_receipt_path);
    if (!agent_relative_directory) {
        add(report, CompatibilityFindingKind::no_agent_requested);
        return finish_report(std::move(report), false);
    }
    const auto agent = attachment.resolve(*agent_relative_directory);
    std::error_code error;
    if (!agent || !fs::is_directory(agent.path, error) || error) {
        add(report, CompatibilityFindingKind::agent_unavailable);
        if (!agent && agent.failure == ProjectPathFailure::target_not_found) {
            add(report, CompatibilityFindingKind::init_missing);
            add(report, CompatibilityFindingKind::resolved_missing);
        }
        return finish_report(std::move(report), false);
    }
    const auto init_file = read_scoped_file(
        attachment, *agent_relative_directory / "init.json");
    const auto resolved_file = read_scoped_file(
        attachment,
        *agent_relative_directory / "system/manifest.resolved.json");
    inspect_raw_init(report, init_file);
    inspect_resolved_manifest(report, resolved_file, init_file);
    return finish_report(std::move(report), true);
}
} // namespace
CompatibilityReport probe_compatibility(
        const ProjectAttachment &attachment,
        const std::optional<fs::path> &agent_relative_directory,
        const fs::path &install_receipt_path) noexcept {
    try {
        return probe_impl(
            attachment, agent_relative_directory, install_receipt_path);
    } catch (...) {
        CompatibilityReport report;
        report.disposition = CompatibilityDisposition::blocked;
        try {
            add(report, CompatibilityFindingKind::probe_failed);
        } catch (...) {
        }
        return report;
    }
}
} // namespace lingtai::desktop
