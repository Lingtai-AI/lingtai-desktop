#include "preset_editor_model.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::PresetEditorLoadRequest;
using lingtai::desktop::PresetEditorModel;
using lingtai::desktop::PresetModelOption;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_json_fixture(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    require(file.good(), "fixture JSON must open for writing");
    file << text;
    require(file.good(), "fixture JSON must be written");
}

const PresetModelOption *find_option(
        const QVector<PresetModelOption> &options, const QString &slug) {
    for (const auto &option : options) {
        if (option.slug == slug) return &option;
    }
    return nullptr;
}

PresetEditorLoadRequest codex_request() {
    PresetEditorLoadRequest request;
    request.name = QStringLiteral("codex-test-preset");
    request.summary = QStringLiteral("Codex test preset");
    request.source = QStringLiteral("saved");
    request.is_template = false;
    return request;
}

void test_model_options_include_current_suggestions_and_custom_row() {
    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("codex"));
    model.set_codex_model_suggestions({
        {QStringLiteral("gpt-6-alpha"), QStringLiteral("GPT-6 Alpha")},
        {QStringLiteral("gpt-6-beta"), QStringLiteral("GPT-6 Beta")},
    });
    model.set_model(QStringLiteral("gpt-legacy-unlisted"));

    const auto options = model.model_options();
    require(options.size() == 4,
        "current (unlisted) + 2 suggestions + 1 explicit custom row");

    const auto *current_row = find_option(options, QStringLiteral("gpt-legacy-unlisted"));
    require(current_row != nullptr,
        "a current model absent from the catalog must remain its own usable row");
    require(current_row->label == QStringLiteral("gpt-legacy-unlisted"),
        "an unmatched current model must be labeled with its own id, not a fabricated name");

    const auto *alpha_row = find_option(options, QStringLiteral("gpt-6-alpha"));
    require(alpha_row != nullptr, "a fetched suggestion must appear in the options");
    require(alpha_row->label == QStringLiteral("GPT-6 Alpha"),
        "a suggestion row must show the catalog display_name as its label");

    require(options.back().slug.isEmpty(),
        "the trailing row must be the explicit free-text custom entry");
}

void test_current_model_matching_suggestion_does_not_duplicate() {
    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("codex"));
    model.set_codex_model_suggestions({
        {QStringLiteral("gpt-6-alpha"), QStringLiteral("GPT-6 Alpha")},
    });
    model.set_model(QStringLiteral("gpt-6-alpha"));

    const auto options = model.model_options();
    auto matches = 0;
    for (const auto &option : options) {
        if (option.slug == QStringLiteral("gpt-6-alpha")) ++matches;
    }
    require(matches == 1,
        "a current model already present in the suggestion list must not duplicate");
    require(options.front().label == QStringLiteral("GPT-6 Alpha"),
        "a matched current model must show the catalog's own display_name, not the raw slug");
}

void test_codex_pool_shares_same_suggestions() {
    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("codex-pool"));
    model.set_codex_model_suggestions({
        {QStringLiteral("gpt-6-alpha"), QStringLiteral("GPT-6 Alpha")},
    });
    const auto options = model.model_options();
    require(find_option(options, QStringLiteral("gpt-6-alpha")) != nullptr,
        "legacy codex-pool must read the same public suggestion list as codex, "
        "with no separate account-pool eligibility query");
}

void test_set_codex_model_suggestions_preserves_other_state() {
    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("codex"));
    model.set_model(QStringLiteral("gpt-6-alpha"));
    model.set_name(QStringLiteral("in-progress-edit"));
    model.set_summary(QStringLiteral("still typing"));

    const auto document_before = model.document();
    const auto name_before = model.name();
    const auto summary_before = model.summary();
    const auto model_before = model.model();

    model.set_codex_model_suggestions({
        {QStringLiteral("gpt-6-alpha"), QStringLiteral("GPT-6 Alpha (refreshed label)")},
        {QStringLiteral("gpt-6-new"), QStringLiteral("GPT-6 New")},
    });

    require(model.document() == document_before,
        "a background catalog refresh must never touch the working JSON document");
    require(model.name() == name_before,
        "a background catalog refresh must never touch an in-progress name edit");
    require(model.summary() == summary_before,
        "a background catalog refresh must never touch an in-progress summary edit");
    require(model.model() == model_before,
        "a background catalog refresh must never change the already-selected/custom model id");
}

void test_commit_persists_slug_never_display_label() {
    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("codex"));
    model.set_codex_model_suggestions({
        {QStringLiteral("gpt-6-alpha"), QStringLiteral("GPT-6 Alpha, Display Label")},
    });
    model.set_model(QStringLiteral("gpt-6-alpha"));

    const auto commit = model.commit({});
    require(commit.ok, "an in-memory (not-yet-on-disk) preset must commit successfully");
    const auto persisted = commit.document.value(QStringLiteral("manifest")).toObject()
        .value(QStringLiteral("llm")).toObject().value(QStringLiteral("model")).toString();
    require(persisted == QStringLiteral("gpt-6-alpha"),
        "the committed document must persist the slug");
    require(!persisted.contains(QStringLiteral("Display Label")),
        "the committed document must never contain display-label text");
}

void test_switch_to_codex_defaults_only_when_unset() {
    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("minimax"));
    model.set_codex_model_suggestions({
        {QStringLiteral("gpt-6-alpha"), QStringLiteral("GPT-6 Alpha")},
        {QStringLiteral("gpt-6-beta"), QStringLiteral("GPT-6 Beta")},
    });

    model.set_provider(QStringLiteral("codex"));
    require(model.model() == QStringLiteral("gpt-6-alpha"),
        "switching into Codex with no already-valid model must default to the first suggestion, "
        "matching every other provider's existing switch-time default");

    model.set_model(QStringLiteral("gpt-6-beta"));
    model.set_provider(QStringLiteral("codex-pool"));
    require(model.model() == QStringLiteral("gpt-6-beta"),
        "switching within the Codex family must never override an already-valid model");
}

void test_non_codex_model_options_unchanged_shape() {
    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("minimax"));
    const auto options = model.model_options();
    require(!options.isEmpty(), "a static-catalog provider must still list its models");
    for (const auto &option : options) {
        require(option.slug == option.label,
            "non-Codex providers must keep label == slug exactly as before this change");
        require(!option.slug.isEmpty(), "non-Codex rows must never be the empty-slug sentinel");
    }
}

void test_load_seeds_suggestions_from_injected_cache(const fs::path &fixture) {
    const auto global = fixture / "cache-seed-global";
    const auto global_text = QString::fromStdString(global.string());
    require(lingtai::desktop::write_codex_model_cache(global_text, {
        {QStringLiteral("gpt-cache-only"), QStringLiteral("GPT Cache Only")},
    }), "seeding the injected cache fixture must succeed");

    const auto previous = qgetenv("LINGTAI_TUI_DIR");
    qputenv("LINGTAI_TUI_DIR", QByteArray::fromStdString(global.string()));

    PresetEditorModel model;
    model.load(codex_request());
    model.set_provider(QStringLiteral("codex"));
    const auto options = model.model_options();

    if (previous.isNull()) {
        qunsetenv("LINGTAI_TUI_DIR");
    } else {
        qputenv("LINGTAI_TUI_DIR", previous);
    }

    require(find_option(options, QStringLiteral("gpt-cache-only")) != nullptr,
        "load() must seed its per-instance suggestions from the injected last-good cache");
}

void test_load_preserves_json_values_and_document_snapshot(const fs::path &fixture) {
    const auto source = fixture / "presets" / "arbitrary.json";
    const std::string json = R"({
        "name":"arbitrary",
        "description":{"summary":"Preserve values","private":{"enabled":true,"count":7,"ratio":1.25,"nullable":null},"items":["alpha",false,3]},
        "manifest":{"llm":{"provider":"openai","model":"gpt-6-alpha"},"untouched":[{"k":"v"}]},
        "extension":{"flag":false,"value":null,"number":9223372036854775807}
    })";
    write_json_fixture(source, json);

    auto request = codex_request();
    request.path = QString::fromStdString(source.string());
    PresetEditorModel model;
    model.load(request);
    const auto expected = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
    require(model.loaded_from_disk() && model.document() == expected,
        "load() must retain parsed values of every JSON type and untouched nested fields");

    const auto before = model.document();
    auto snapshot = before;
    auto extension = snapshot.value(QStringLiteral("extension")).toObject();
    extension.insert(QStringLiteral("flag"), true);
    snapshot.insert(QStringLiteral("extension"), extension);
    require(model.document() == before && !model.has_semantic_edits(),
        "editing a nested document snapshot must not change the working or original document");

    auto missing = codex_request();
    missing.path = QString::fromStdString((fixture / "presets" / "missing.json").string());
    PresetEditorModel fallback;
    fallback.load(missing);
    const QJsonObject expected_fallback{
        {QStringLiteral("name"), missing.name},
        {QStringLiteral("description"), QJsonObject{
            {QStringLiteral("summary"), missing.summary},
        }},
        {QStringLiteral("manifest"), QJsonObject{
            {QStringLiteral("llm"), QJsonObject{}},
        }},
    };
    require(!fallback.loaded_from_disk() && fallback.document() == expected_fallback,
        "load() must preserve the existing fallback JSON document");

    const auto unnamed_source = fixture / "presets" / "unnamed.json";
    write_json_fixture(unnamed_source,
        R"({"description":{"summary":"Unnamed"},"manifest":{"llm":{}}})");
    PresetEditorLoadRequest unnamed_request;
    unnamed_request.path = QString::fromStdString(unnamed_source.string());
    PresetEditorModel unnamed;
    unnamed.load(unnamed_request);
    require(unnamed.name().isEmpty() && !unnamed.has_semantic_edits(),
        "the missing-name snapshot must keep the loaded model semantically unchanged");
}

void test_loaded_commit_does_not_mutate_working_or_original(const fs::path &fixture) {
    const auto json = R"({"name":"copy-check","description":{"summary":"Fixture"},"manifest":{"llm":{"provider":"openai","model":"initial"}}})";
    for (const auto is_template : {false, true}) {
        const auto source = fixture / (is_template ? "template.json" : "saved.json");
        write_json_fixture(source, json);
        auto request = codex_request();
        request.path = QString::fromStdString(source.string());
        request.is_template = is_template;

        PresetEditorModel model;
        model.load(request);
        const auto original_name = model.original_name();
        model.set_model(QStringLiteral("edited"));
        const auto working_before_commit = model.document();
        require(model.has_semantic_edits(), "the model edit must be detected before commit");

        const auto committed = model.commit({});
        require(committed.ok && committed.document.value(QStringLiteral("manifest")).toObject()
                .value(QStringLiteral("llm")).toObject().value(QStringLiteral("model")).toString()
                == QStringLiteral("edited"),
            "commit() must return the edited document");
        require(model.document() == working_before_commit
                && model.original_name() == original_name && model.has_semantic_edits(),
            "commit normalization and template naming must not mutate working/original state");
        if (is_template) {
            require(committed.name != original_name,
                "an edited template commit must keep its saved-preset naming behavior");
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: preset_editor_model_test FIXTURE_ROOT\n";
        return 2;
    }
    try {
        const auto fixture = fs::path(argv[1]);
        std::error_code cleanup_error;
        fs::remove_all(fixture, cleanup_error);
        require(!cleanup_error, "fixture root must start clean");

        // Every load() below seeds its Codex suggestions from
        // LINGTAI_TUI_DIR's cache; pin it under the fixture root for the
        // whole run so nothing ever reads a real HOME.
        const auto previous_global = qgetenv("LINGTAI_TUI_DIR");
        qputenv("LINGTAI_TUI_DIR",
            QByteArray::fromStdString((fixture / "default-global").string()));

        test_model_options_include_current_suggestions_and_custom_row();
        test_current_model_matching_suggestion_does_not_duplicate();
        test_codex_pool_shares_same_suggestions();
        test_set_codex_model_suggestions_preserves_other_state();
        test_commit_persists_slug_never_display_label();
        test_switch_to_codex_defaults_only_when_unset();
        test_non_codex_model_options_unchanged_shape();
        test_load_seeds_suggestions_from_injected_cache(fixture);
        test_load_preserves_json_values_and_document_snapshot(fixture);
        test_loaded_commit_does_not_mutate_working_or_original(fixture);

        if (previous_global.isNull()) {
            qunsetenv("LINGTAI_TUI_DIR");
        } else {
            qputenv("LINGTAI_TUI_DIR", previous_global);
        }

        fs::remove_all(fixture, cleanup_error);
        std::cout << "preset_editor_model_test: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "preset_editor_model_test FAILED: " << error.what() << "\n";
        return 1;
    }
}
