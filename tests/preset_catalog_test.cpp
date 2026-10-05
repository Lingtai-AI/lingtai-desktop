#include "preset_catalog.h"

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
          "manifest":{"llm":{"provider":"z","model":"z1"}}
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
          "manifest":{"llm":{"provider":"future","model":"f1"}}
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
