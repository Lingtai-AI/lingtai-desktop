#include "tui_executable_resolver.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::TuiExecutableSearch;
using lingtai::desktop::resolve_tui_executable;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void reset(const fs::path &root) {
    std::error_code error;
    fs::remove_all(root, error);
    require(!error, "fixture reset: " + error.message());
    fs::create_directories(root, error);
    require(!error, "fixture create: " + error.message());
}

void write_file(
        const fs::path &path, std::string_view bytes, mode_t mode = 0644) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "fixture parent: " + error.message());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "fixture write: " + path.string());
    stream.close();
    require(::chmod(path.c_str(), mode) == 0, "fixture chmod: " + path.string());
}

fs::path executable(const fs::path &directory) {
    const auto path = directory / "lingtai-tui";
    write_file(path, "#!/bin/sh\nexit 0\n", 0755);
    return path;
}

std::string joined_path(std::initializer_list<fs::path> directories) {
    std::string result;
    for (const auto &directory : directories) {
        if (!result.empty()) result += ':';
        result += directory.string();
    }
    return result;
}

TuiExecutableSearch search_for(const fs::path &root) {
    return {
        .inherited_path = {},
        .home = root / "home",
        .usr_local_bin = root / "system" / "usr-local-bin",
        .opt_homebrew_bin = root / "system" / "opt-homebrew-bin",
    };
}

void write_receipt(
        const fs::path &home,
        const fs::path &bin_dir,
        QJsonValue schema = QStringLiteral("lingtai.tui.install/v1"),
        QJsonValue schema_version = 1,
        QJsonValue managed_binaries = QJsonValue(QJsonValue::Undefined)) {
    if (managed_binaries.isUndefined()) {
        managed_binaries = QJsonArray{
            QString::fromStdString((bin_dir / "lingtai-tui").string())};
    }
    const QJsonObject object{
        {QStringLiteral("schema"), schema},
        {QStringLiteral("schema_version"), schema_version},
        {QStringLiteral("bin_dir"), QString::fromStdString(bin_dir.string())},
        {QStringLiteral("managed_binaries"), managed_binaries},
    };
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    write_file(
        home / ".lingtai-tui" / "install.json",
        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

void verify_path_precedence(const fs::path &root) {
    reset(root);
    auto search = search_for(root);
    const auto first = executable(root / "path-first");
    static_cast<void>(executable(root / "path-second"));
    static_cast<void>(executable(search.home / ".local" / "bin"));
    search.inherited_path = joined_path({root / "path-first", root / "path-second"});
    require(resolve_tui_executable(search) == first,
        "first inherited PATH executable wins over every later candidate");
}

void verify_finder_system_fallbacks(const fs::path &root) {
    reset(root);
    auto search = search_for(root);
    search.inherited_path = joined_path({
        root / "finder" / "usr-bin",
        root / "finder" / "bin",
        root / "finder" / "usr-sbin",
        root / "finder" / "sbin"});
    const auto usr_local = executable(search.usr_local_bin);
    static_cast<void>(executable(search.opt_homebrew_bin));
    require(resolve_tui_executable(search) == usr_local,
        "restricted Finder PATH falls back to injected /usr/local/bin first");

    fs::remove(usr_local);
    require(resolve_tui_executable(search)
            == search.opt_homebrew_bin / "lingtai-tui",
        "injected /opt/homebrew/bin is the final fallback");
}

void verify_managed_and_local_fallbacks(const fs::path &root) {
    reset(root);
    auto search = search_for(root);
    const auto managed = executable(root / "managed-bin");
    write_receipt(search.home, managed.parent_path());
    static_cast<void>(executable(search.home / ".local" / "bin"));
    require(resolve_tui_executable(search) == managed,
        "valid managed receipt precedes local fallback");

    fs::remove_all(search.home / ".lingtai-tui");
    require(resolve_tui_executable(search)
            == search.home / ".local" / "bin" / "lingtai-tui",
        "home local bin is used after PATH and receipt");
}

void verify_bad_receipts_are_ignored(const fs::path &root) {
    const auto verify_ignored = [&](const QJsonValue &schema,
                                    const QJsonValue &version,
                                    const QJsonValue &managed,
                                    const std::string &label) {
        reset(root);
        auto search = search_for(root);
        const auto managed_path = executable(root / "managed-bin");
        write_receipt(
            search.home, managed_path.parent_path(), schema, version, managed);
        const auto fallback = executable(search.home / ".local" / "bin");
        require(resolve_tui_executable(search) == fallback, label);
    };
    verify_ignored("wrong", 1, QJsonArray{"ignored"},
        "wrong receipt schema is ignored");
    verify_ignored("lingtai.tui.install/v1", 2, QJsonArray{"ignored"},
        "wrong receipt version is ignored");
    verify_ignored("lingtai.tui.install/v1", 1, QStringLiteral("not-an-array"),
        "wrong managed_binaries shape is ignored");

    reset(root);
    auto search = search_for(root);
    write_file(search.home / ".lingtai-tui" / "install.json", "{broken");
    const auto fallback = executable(search.home / ".local" / "bin");
    require(resolve_tui_executable(search) == fallback,
        "malformed receipt is ignored");
}

void verify_candidate_validation(const fs::path &root) {
    reset(root);
    auto search = search_for(root);
    const auto absent = root / "absent";
    const auto directory = root / "directory";
    fs::create_directories(directory / "lingtai-tui");
    const auto non_executable = root / "non-executable";
    write_file(non_executable / "lingtai-tui", "not executable", 0644);
    const auto dangling = root / "dangling";
    fs::create_directories(dangling);
    fs::create_symlink(root / "missing-target", dangling / "lingtai-tui");
    const auto non_regular = root / "non-regular";
    fs::create_directories(non_regular);
    require(::mkfifo((non_regular / "lingtai-tui").c_str(), 0755) == 0,
        "fixture fifo");
    search.inherited_path = joined_path(
        {absent, directory, non_executable, dangling, non_regular});
    require(resolve_tui_executable(search).empty(),
        "absent, directory, non-executable, dangling, and non-regular reject");

    const auto target = executable(root / "symlink-target");
    const auto links = root / "links";
    fs::create_directories(links);
    const auto link = links / "lingtai-tui";
    fs::create_symlink(target, link);
    search.inherited_path = links.string();
    require(resolve_tui_executable(search) == link,
        "benign executable symlink is accepted and its path is preserved");
}

void verify_empty_result(const fs::path &root) {
    reset(root);
    auto search = search_for(root);
    search.inherited_path = ":";
    require(resolve_tui_executable(search).empty(),
        "no candidate and empty PATH entries fail closed");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "expected one injected fixture root");
        const fs::path root(argv[1]);
        verify_path_precedence(root);
        verify_finder_system_fallbacks(root);
        verify_managed_and_local_fallbacks(root);
        verify_bad_receipts_are_ignored(root);
        verify_candidate_validation(root);
        verify_empty_result(root);
        std::error_code error;
        fs::remove_all(root, error);
        require(!error, "fixture cleanup: " + error.message());
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
