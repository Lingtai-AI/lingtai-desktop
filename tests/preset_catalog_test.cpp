#include "preset_catalog.h"
#include "preset_catalog_presentation.h"

#include <QtCore/QString>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::PresetCatalogLoadFailure;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(bool(stream), "fixture file must open: " + path.string());
    stream << bytes;
    require(bool(stream), "fixture file must write: " + path.string());
}

QString path_text(const fs::path &path) {
    return QString::fromStdString(path.string());
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: preset_catalog_test FIXTURE_ROOT\n";
        return 2;
    }
    try {
        const auto fixture = fs::path(argv[1]);
        std::error_code cleanup_error;
        fs::remove_all(fixture, cleanup_error);
        require(!cleanup_error, "fixture root must start clean");
        const auto global = fixture / "global";

        write_file(global / "presets/saved/zeta.json", R"({
          "name":"zeta",
          "description":{"summary":"Zeta saved","tier":"5"},
          "manifest":{"llm":{"provider":"z","model":"z1"},
            "capabilities":{"vision":true,"tools":{"functions":["read"]}}}
        })");
        write_file(global / "presets/saved/alpha.json", R"({
          "name":"alpha",
          "description":"Alpha legacy",
          "tier":"2",
          "manifest":{"llm":{"provider":"a","model":"a1"}}
        })");
        write_file(global / "presets/templates/codex.json", R"({
          "name":"codex","description":{"summary":"Codex template","tier":"3"},
          "manifest":{"llm":{"provider":"codex","model":"gpt"}}
        })");
        write_file(global / "presets/templates/minimax.json", R"({
          "name":"minimax","description":{"summary":"MiniMax template","tier":"1"},
          "manifest":{"llm":{"provider":"minimax","model":"m2"}}
        })");
        write_file(global / "presets/templates/future.json", R"({
          "name":"future","description":{"summary":"Future template"},
          "manifest":{"llm":{"provider":"future","model":"f1"},
            "capabilities":{"vision":false,"tools":{}}}
        })");
        write_file(global / "presets/saved/_kernel_meta.json",
            R"({"name":"metadata-must-not-appear"})");
        write_file(global / "presets/saved/malformed.json", "{broken");
        write_file(global / "presets/saved/array.json", "[]");
        write_file(global / "presets/saved/blank.json", R"({"name":"  "})");
        write_file(global / "presets/saved/backup.json.bak",
            R"({"name":"backup-must-not-appear"})");
        write_file(global / "presets/saved/note.txt",
            R"({"name":"text-must-not-appear"})");
        fs::create_directories(global / "presets/saved/directory.json");

        const auto before_count = std::distance(
            fs::recursive_directory_iterator(global),
            fs::recursive_directory_iterator());
        const auto loaded = lingtai::desktop::load_preset_catalog(path_text(global));
        require(bool(loaded), "valid catalog directories must load");
        require(loaded.presets.size() == 5,
            "loader must return all and only valid saved/template objects");
        const auto names = std::vector<std::string>{
            loaded.presets[0].name, loaded.presets[1].name,
            loaded.presets[2].name, loaded.presets[3].name,
            loaded.presets[4].name};
        require(names == std::vector<std::string>{
                "alpha", "zeta", "minimax", "codex", "future"},
            "loader order must match saved alphabetical then canonical templates");
        require(loaded.presets[0].description == "Alpha legacy"
                && loaded.presets[0].tier == "2"
                && loaded.presets[0].source == "saved"
                && loaded.presets[0].path
                    == (global / "presets/saved/alpha.json").string(),
            "legacy saved facts and exact path must survive loading");
        require(loaded.presets[2].description == "MiniMax template"
                && loaded.presets[2].tier == "1"
                && loaded.presets[2].source == "template",
            "structured template description and tier must survive loading");

        const auto rows = lingtai::desktop::build_preset_catalog_rows(loaded.presets);
        require(rows.size() == 5
                && rows[0].entry.name == "alpha"
                && rows[1].entry.name == "zeta"
                && rows[2].entry.name == "minimax"
                && rows[3].entry.name == "codex"
                && rows[4].entry.name == "future",
            "catalog rows must keep saved and canonical template ordering");
        require(rows[0].entry.description == "Alpha legacy"
                && rows[0].entry.tier == "2"
                && rows[0].entry.source == "saved"
                && rows[0].entry.path == loaded.presets[0].path
                && rows[0].summary == QStringLiteral("Alpha legacy")
                && rows[0].provider == QStringLiteral("a")
                && rows[0].model == QStringLiteral("a1")
                && rows[0].provider_model == QStringLiteral("a · a1")
                && !rows[0].has_vision && !rows[0].has_tools
                && !rows[0].is_template,
            "saved row fields and provider/model facts must be preserved");
        require(rows[1].summary == QStringLiteral("Zeta saved")
                && rows[1].provider_model == QStringLiteral("z · z1")
                && rows[1].has_vision && rows[1].has_tools
                && !rows[1].is_template,
            "manifest summary, provider/model, and populated capabilities must project");
        require(rows[2].summary == QStringLiteral("MiniMax template")
                && rows[2].provider_model == QStringLiteral("minimax · m2")
                && rows[2].is_template,
            "structured template fields must project");
        require(rows[4].summary == QStringLiteral("Future template")
                && !rows[4].has_vision && !rows[4].has_tools
                && rows[4].is_template,
            "false and empty-object capabilities must remain disabled");

        write_file(fixture / "not-a-directory", "fixture file");
        const auto fallback_rows =
            lingtai::desktop::build_preset_catalog_rows({
                {"malformed", "Malformed fallback", "3", "saved",
                    (global / "presets/saved/malformed.json").string()},
                {"unreadable", "Unreadable fallback", "4", "saved",
                    (fixture / "not-a-directory/unreadable.json").string()},
            });
        require(fallback_rows.size() == 2
                && fallback_rows[0].summary == QStringLiteral("Malformed fallback")
                && fallback_rows[0].provider_model
                    == QStringLiteral("Malformed fallback")
                && fallback_rows[1].summary == QStringLiteral("Unreadable fallback")
                && fallback_rows[1].provider_model
                    == QStringLiteral("Unreadable fallback")
                && fallback_rows[0].provider.isEmpty()
                && fallback_rows[1].model.isEmpty(),
            "malformed and unreadable manifests must retain entry summary fallback");

        const auto refs = std::vector<std::string>{
            (global / "presets/templates/codex.json").string(),
            (global / "presets/saved/zeta.json").string(),
            (fixture / "missing-allowed.json").string(),
            (global / "presets/saved/alpha.json").string(),
        };
        const auto ref_rows =
            lingtai::desktop::build_preset_catalog_rows_from_refs(refs);
        require(ref_rows.size() == refs.size()
                && ref_rows[0].entry.name == "codex"
                && ref_rows[1].entry.name == "zeta"
                && ref_rows[2].entry.name == "missing-allowed"
                && ref_rows[3].entry.name == "alpha",
            "ref rows must preserve published input order");
        require(ref_rows[0].is_template
                && ref_rows[0].provider_model == QStringLiteral("codex · gpt")
                && !ref_rows[1].is_template
                && ref_rows[1].has_vision && ref_rows[1].has_tools
                && ref_rows[2].summary.isEmpty()
                && ref_rows[2].provider_model.isEmpty(),
            "ref rows must share manifest facts and retain missing-ref fallback");
        const auto after_count = std::distance(
            fs::recursive_directory_iterator(global),
            fs::recursive_directory_iterator());
        require(before_count == after_count,
            "catalog loading must not bootstrap or write any file");

        const auto missing = lingtai::desktop::load_preset_catalog(
            path_text(fixture / "missing-global"));
        require(bool(missing) && missing.presets.empty(),
            "missing saved/templates directories must be a successful empty catalog");

        const auto broken = fixture / "broken-global";
        write_file(broken / "presets/saved", "not a directory");
        const auto failed = lingtai::desktop::load_preset_catalog(path_text(broken));
        require(!failed
                && failed.failure == PresetCatalogLoadFailure::directory_read_failed
                && failed.detail.find("presets/saved") != std::string::npos,
            "an existing unreadable directory path must return typed evidence");

        const auto absolute = path_text(global / "presets/saved/alpha.json");
        const auto tilde = QStringLiteral(
            "~/.lingtai-tui/presets/saved/alpha.json");
        require(lingtai::desktop::normalize_preset_reference(
                    absolute, path_text(global))
                == lingtai::desktop::normalize_preset_reference(
                    tilde, path_text(global)),
            "absolute and ~/.lingtai-tui refs must normalize to one catalog row");

        fs::remove_all(fixture, cleanup_error);
        require(!cleanup_error, "fixture root must be removable");
        std::cout << "preset catalog contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "preset catalog contract failed: " << error.what() << '\n';
        return 1;
    }
}
