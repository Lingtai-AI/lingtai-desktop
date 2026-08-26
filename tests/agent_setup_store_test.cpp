#include "agent_setup_store.h"
#include "project_attachment.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;
using namespace lingtai::desktop;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const fs::path &path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "fixture parent: " + error.message());
    auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "fixture write: " + path.string());
}

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

mode_t file_mode(const fs::path &path) {
    struct stat status {};
    require(::stat(path.c_str(), &status) == 0, "fixture stat: " + path.string());
    return status.st_mode & 07777;
}

QJsonObject read_json(const fs::path &path) {
    const auto bytes = read_file(path);
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
    require(document.isObject(), "fixture output is an object: " + path.string());
    return document.object();
}

std::map<std::string, std::string> tree_snapshot(const fs::path &root) {
    std::map<std::string, std::string> result;
    std::error_code error;
    for (fs::recursive_directory_iterator i(
             root, fs::directory_options::skip_permission_denied, error), end;
            !error && i != end; i.increment(error)) {
        const auto key = i->path().lexically_relative(root).generic_string();
        const auto status = i->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) {
            result[key] = "link:" + fs::read_symlink(i->path()).generic_string();
        } else if (fs::is_directory(status)) {
            result[key] = "directory";
        } else if (fs::is_regular_file(status)) {
            result[key] = "file:" + read_file(i->path());
        } else {
            result[key] = "other";
        }
    }
    require(!error, "snapshot succeeds: " + error.message());
    return result;
}

ProjectAttachment attach(const fs::path &project) {
    auto result = attach_project(project);
    require(static_cast<bool>(result), "fixture project attaches");
    return *result.attachment;
}

std::string json_bytes(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Indented).toStdString();
}

QJsonObject preset_block(
        const char *active, const char *default_ref,
        std::initializer_list<const char *> allowed) {
    QJsonArray list;
    for (const auto *value : allowed) list.append(QString::fromUtf8(value));
    return {
        {"active", QString::fromUtf8(active)},
        {"default", QString::fromUtf8(default_ref)},
        {"allowed", list},
        {"custom_policy", QJsonObject{{"keep", true}}},
    };
}

void make_valid_agent(
        const fs::path &project, const std::string &key,
        const QJsonObject &preset, const fs::path &env_path,
        bool main_admin = true) {
    const auto agent = project / ".lingtai" / key;
    QJsonObject manifest{
        {"agent_name", QString::fromStdString(key)},
        {"language", "en"},
        {"llm", QJsonObject{{"provider", "old"}, {"model", "old-model"}}},
        {"capabilities", QJsonObject{{"shell", QJsonObject{{"enabled", true}}}}},
        {"admin", QJsonObject{{"karma", main_admin}, {"nirvana", false}, {"custom", "keep"}}},
        {"context_limit", 300000},
        {"max_rpm", 60},
        {"max_aed_attempts", 5},
        {"soul", QJsonObject{{"delay", 7200}, {"voice", "keep"}}},
        {"preset", preset},
        {"custom_manifest", QJsonObject{{"huge", QJsonValue(9007199254740993LL)}}},
    };
    QJsonObject init{
        {"manifest", manifest},
        {"env_file", QString::fromStdString(fs::canonical(env_path).string())},
        {"venv_path", "/custom/venv"},
        {"pad", QJsonObject{{"custom", 7}}},
        {"addons", QJsonArray{"imap"}},
        {"mcp", QJsonObject{{"custom", QJsonObject{{"command", "mine"}}}}},
        {"covenant_file", "old-covenant.md"},
        {"soul_file", "old-soul.md"},
        {"comment_file", "old-comment.md"},
        {"custom_top", QJsonArray{1, "two", true}},
    };
    write_file(agent / "init.json", json_bytes(init));

    QJsonObject identity{
        {"agent_id", QString::fromStdString("id-" + key)},
        {"agent_name", QString::fromStdString(key)},
        {"address", QString::fromStdString(key)},
        {"created_at", "2025-01-01T00:00:00Z"},
        {"started_at", "2025-01-02T00:00:00Z"},
        {"molt_count", 17},
        {"nickname", "Ancient One"},
        {"soul_voice", QJsonObject{{"tone", "dry"}}},
        {"state", "ACTIVE"},
        {"capabilities", QJsonArray{"cached"}},
        {"admin", QJsonObject{{"karma", main_admin}, {"nirvana", false}, {"custom", "keep"}}},
        {"unknown_identity", QJsonObject{{"keep", true}}},
    };
    write_file(agent / ".agent.json", json_bytes(identity));
}

void expect_no_transaction_artifacts(const fs::path &root) {
    for (const auto &[key, value] : tree_snapshot(root)) {
        (void)value;
        require(key.find(".lingtai-setup-") == std::string::npos,
            "transaction temp/backup is cleaned: " + key);
    }
}

void test_reconciliation_and_preservation(const fs::path &base) {
    const auto project = base / "success";
    const auto env = base / "global-success/.env";
    write_file(env, "# heading\r\nSECRET=keep\r\nLINGTAI_SOUL_FLOW_ENABLED=0\r\nTAIL=stay");
    make_valid_agent(project, "alpha",
        preset_block("old-active", "old-default", {"old-active", "old-default"}), env);
    make_valid_agent(project, "beta",
        preset_block("other", "old-default", {"other", "old-default"}), env, false);

    const auto beta_agent_before = read_file(project / ".lingtai/beta/.agent.json");
    AgentSetupStore store(attach(project));
    const auto loaded = store.load("alpha");
    require(static_cast<bool>(loaded),
        "valid current Agent setup loads: " + loaded.detail);
    require(loaded.state->has_virtual_keep_current,
        "load exposes the virtual Keep Current choice");
    require(loaded.state->draft.preset.choice == AgentSetupPresetChoice::keep_current,
        "Keep Current is the initial typed selection");

    auto draft = loaded.state->draft;
    draft.preset.choice = AgentSetupPresetChoice::select_preset;
    draft.preset.reference = "new-default";
    draft.preset.manifest = QJsonObject{
        {"llm", QJsonObject{{"provider", "new"}, {"model", "new-model"}}},
        {"capabilities", QJsonObject{{"vision", QJsonObject{{"enabled", true}}}}},
    };
    draft.allowed_presets = {"new-default", "other", "new-default"};
    draft.agent_name = "Renamed Alpha";
    draft.language = "wen";
    draft.context_limit = 456789;
    draft.max_rpm = 42;
    draft.max_aed_attempts = 9;
    draft.soul_delay = 3600.0;
    draft.karma = true;
    draft.nirvana = true;
    draft.soul_flow_enabled = true;
    draft.covenant_file = "new-covenant.md";
    draft.comment_file = "new-comment.md";

    const auto saved = store.save(*loaded.state, draft);
    require(saved && saved.status == AgentSetupSaveStatus::saved,
        "one validated setup transaction saves");

    const auto init = read_json(project / ".lingtai/alpha/init.json");
    require(init.value("env_file") == loaded.state->init_document.value("env_file")
            && init.value("venv_path") == loaded.state->init_document.value("venv_path")
            && init.value("pad") == loaded.state->init_document.value("pad")
            && init.value("addons") == loaded.state->init_document.value("addons")
            && init.value("mcp") == loaded.state->init_document.value("mcp")
            && init.value("soul_file") == loaded.state->init_document.value("soul_file")
            && init.value("custom_top") == loaded.state->init_document.value("custom_top"),
        "preset switch preserves env/venv/pad/addons/MCP/top-level custom values");
    const auto manifest = init.value("manifest").toObject();
    require(manifest.value("custom_manifest")
                == loaded.state->init_document.value("manifest").toObject().value("custom_manifest"),
        "custom manifest values, including large integers, survive semantically");
    require(manifest.value("llm").toObject().value("provider") == "new"
            && manifest.value("capabilities").toObject().contains("vision"),
        "selected preset updates only setup-owned llm/capability fields");
    const auto policy = manifest.value("preset").toObject();
    require(policy.value("active") == "new-default"
            && policy.value("default") == "new-default"
            && policy.value("allowed").toArray()
                == QJsonArray{"new-default", "other"}
            && policy.value("custom_policy").toObject().value("keep").toBool(),
        "revoked active deterministically falls back to default and policy extras survive");
    require(read_file(project / ".lingtai/alpha/init.json").find("keep_current")
            == std::string::npos,
        "the virtual Keep Current sentinel is never serialized");

    const auto identity = read_json(project / ".lingtai/alpha/.agent.json");
    for (const auto *key : {"agent_id", "created_at", "started_at", "molt_count",
             "nickname", "soul_voice", "state", "capabilities", "unknown_identity"}) {
        require(identity.value(key) == loaded.state->agent_document.value(key),
            std::string("long-lived identity/runtime field survives: ") + key);
    }
    require(identity.value("agent_name") == "Renamed Alpha"
            && identity.value("admin").toObject().value("nirvana").toBool()
            && identity.value("admin").toObject().value("custom") == "keep",
        "only selected Agent setup-owned identity/admin leaves change");

    require(read_file(env)
            == "# heading\r\nSECRET=keep\r\nLINGTAI_SOUL_FLOW_ENABLED=1\r\nTAIL=stay",
        "env merge changes one active key while preserving CRLF, secrets, order, and no final newline");

    const auto beta = read_json(project / ".lingtai/beta/init.json");
    const auto beta_manifest = beta.value("manifest").toObject();
    const auto beta_policy = beta_manifest.value("preset").toObject();
    require(beta_policy.value("active") == "other"
            && beta_policy.value("default") == "new-default"
            && beta_policy.value("allowed").toArray()
                == QJsonArray{"new-default", "other"},
        "peer propagation preserves a still-allowed active and applies default/allowed");
    require(beta_manifest.value("llm") == manifest.value("llm")
            && beta_manifest.value("capabilities") == manifest.value("capabilities")
            && beta_manifest.value("soul") == manifest.value("soul")
            && beta_manifest.value("context_limit") == manifest.value("context_limit")
            && beta.value("env_file") == init.value("env_file"),
        "orchestrator runtime fields propagate to peers");
    require(beta_manifest.value("admin").toObject().value("karma") == false
            && beta_manifest.value("admin").toObject().value("nirvana") == false
            && beta_manifest.value("admin").toObject().value("custom") == "keep"
            && !beta.contains("addons")
            && beta.value("mcp").toObject().contains("custom")
            && read_file(project / ".lingtai/beta/.agent.json") == beta_agent_before,
        "peer propagation strips addons, forces admin false, and leaves MCP/identity untouched");
    expect_no_transaction_artifacts(base);
}

void test_external_env_on_off(const fs::path &base) {
    const auto project = base / "external-env";
    const auto env = base / "global-external/.env";
    write_file(env, "SECRET=x\nLINGTAI_SOUL_FLOW_ENABLED=YeS\nTAIL=y\n");
    require(::chmod(env.c_str(), 0600) == 0, "external env fixture mode is set");
    make_valid_agent(project, "alpha",
        preset_block("old", "old", {"old"}), env, false);
    AgentSetupStore store(attach(project));
    auto loaded = store.load("alpha");
    require(static_cast<bool>(loaded), "absolute external env fixture loads: " + loaded.detail);
    require(loaded.state->draft.soul_flow_enabled,
        "external env accepts TUI-compatible case-insensitive truthy values");
    auto off = loaded.state->draft;
    off.soul_flow_enabled = false;
    require(static_cast<bool>(store.save(*loaded.state, off)),
        "external env can be switched off");
    require(read_file(env) == "SECRET=x\nTAIL=y\n",
        "OFF removes only the external soul-flow key");
    require(file_mode(env) == 0600, "OFF preserves the external env mode");

    loaded = store.load("alpha");
    require(static_cast<bool>(loaded), "external env reloads after OFF");
    auto on = loaded.state->draft;
    on.soul_flow_enabled = true;
    require(static_cast<bool>(store.save(*loaded.state, on)),
        "external env can be switched on");
    require(read_file(env) == "SECRET=x\nTAIL=y\nLINGTAI_SOUL_FLOW_ENABLED=1\n",
        "ON appends only the external soul-flow key with line bytes preserved");
    require(file_mode(env) == 0600, "ON preserves the external env mode");
    expect_no_transaction_artifacts(base);
}

void test_keep_current_and_noop(const fs::path &base) {
    const auto project = base / "keep-current";
    const auto env = project / ".lingtai/.env";
    write_file(env, "SECRET=x\n");
    make_valid_agent(project, "alpha",
        preset_block("old-active", "old-default", {"old-active", "old-default"}), env);
    auto minimal = read_json(project / ".lingtai/alpha/init.json");
    minimal.remove("covenant_file");
    minimal.remove("soul_file");
    minimal.remove("comment_file");
    minimal["comment"] = "legacy-comment";
    minimal["prompt"] = "legacy-prompt";
    write_file(project / ".lingtai/alpha/init.json", json_bytes(minimal));
    AgentSetupStore store(attach(project));
    auto loaded = store.load("alpha");
    require(static_cast<bool>(loaded),
        "keep-current fixture loads: " + loaded.detail);
    const auto before = tree_snapshot(project);
    const auto saved = store.save(*loaded.state, loaded.state->draft);
    require(saved && saved.status == AgentSetupSaveStatus::no_change,
        "unchanged typed draft is a true no-op");
    require(tree_snapshot(project) == before,
        "absent optional paths and legacy comment/prompt remain byte-identical on no-op");

    auto changed = loaded.state->draft;
    changed.language = "zh";
    changed.soul_flow_enabled = true;
    const auto changed_result = store.save(*loaded.state, changed);
    require(static_cast<bool>(changed_result),
        "non-preset setup edit saves with Keep Current");
    const auto policy = read_json(project / ".lingtai/alpha/init.json")
        .value("manifest").toObject().value("preset").toObject();
    require(policy.value("active") == "old-active"
            && policy.value("default") == "old-default"
            && read_file(project / ".lingtai/alpha/init.json").find("keep_current")
                == std::string::npos,
        "Keep Current preserves real active/default and never persists its sentinel");
    const auto changed_init = read_json(project / ".lingtai/alpha/init.json");
    require(!changed_init.contains("covenant_file")
            && !changed_init.contains("soul_file")
            && !changed_init.contains("comment_file")
            && changed_init.value("comment") == "legacy-comment"
            && changed_init.value("prompt") == "legacy-prompt",
        "empty optional paths stay absent and legacy comment/prompt stay unchanged");
    require(read_file(env) == "SECRET=x\nLINGTAI_SOUL_FLOW_ENABLED=1\n",
        "an absent soul-flow key is appended with the existing newline style "
        "and final-newline state");
}

void test_empty_allowed_preserves_selected_and_peers(const fs::path &base) {
    const auto project = base / "empty-allowed";
    const auto env = project / ".lingtai/.env";
    write_file(env, "SECRET=x\n");
    make_valid_agent(project, "alpha",
        preset_block("active", "default", {"default", "extra"}), env, false);
    make_valid_agent(project, "beta",
        preset_block("peer-active", "peer-default", {"peer-active", "peer-default"}), env, false);
    AgentSetupStore store(attach(project));
    const auto loaded = store.load("alpha");
    require(static_cast<bool>(loaded), "empty-allowed fixture loads: " + loaded.detail);
    auto draft = loaded.state->draft;
    draft.allowed_presets.clear();
    draft.language = "zh";
    const auto peer_before = read_file(project / ".lingtai/beta/init.json");
    const auto saved = store.save(*loaded.state, draft);
    require(static_cast<bool>(saved), "empty allowed-list draft saves");
    const auto policy = read_json(project / ".lingtai/alpha/init.json")
        .value("manifest").toObject().value("preset").toObject();
    require(policy.value("active") == "active"
            && policy.value("default") == "default"
            && policy.value("allowed").toArray()
                == QJsonArray{"default", "extra", "active"},
        "empty allowed request preserves active by adding it to allowed");
    require(read_file(project / ".lingtai/beta/init.json") == peer_before,
        "empty allowed request does not propagate preset policy to peers");
}

void test_unrelated_admin_boolean_is_not_orchestrator(const fs::path &base) {
    const auto project = base / "non-orchestrator-admin-extra";
    const auto env = project / ".lingtai/.env";
    write_file(env, "SECRET=x\n");
    make_valid_agent(project, "alpha",
        preset_block("old", "old", {"old"}), env, false);
    make_valid_agent(project, "beta",
        preset_block("peer", "peer", {"peer"}), env, false);

    auto selected = read_json(project / ".lingtai/alpha/init.json");
    auto manifest = selected.value("manifest").toObject();
    auto admin = manifest.value("admin").toObject();
    admin["custom_flag"] = true;
    manifest["admin"] = admin;
    selected["manifest"] = manifest;
    write_file(project / ".lingtai/alpha/init.json", json_bytes(selected));

    AgentSetupStore store(attach(project));
    const auto loaded = store.load("alpha");
    require(static_cast<bool>(loaded),
        "non-orchestrator admin-extra fixture loads: " + loaded.detail);
    auto draft = loaded.state->draft;
    draft.allowed_presets.clear();
    const auto peer_before = read_file(project / ".lingtai/beta/init.json");
    const auto saved = store.save(*loaded.state, draft);
    require(saved && saved.status == AgentSetupSaveStatus::no_change,
        "unrelated true admin extra does not classify the selected Agent as orchestrator");
    require(read_file(project / ".lingtai/beta/init.json") == peer_before,
        "unrelated true admin extra leaves peer orchestrator fields and addons untouched");
}

void expect_load_failure_unchanged(
        const fs::path &project, const fs::path &key,
        AgentSetupFailure expected, const std::string &message) {
    const auto before = tree_snapshot(project);
    AgentSetupStore store(attach(project));
    const auto loaded = store.load(key);
    require(!loaded && loaded.failure == expected, message);
    require(tree_snapshot(project) == before, message + " leaves zero mutation");
}

void test_invalid_inputs(const fs::path &base) {
    {
        const auto project = base / "malformed";
        write_file(project / ".lingtai/alpha/init.json", "{");
        write_file(project / ".lingtai/alpha/.agent.json",
            R"({"agent_id":"id","agent_name":"alpha"})");
        expect_load_failure_unchanged(project, "alpha",
            AgentSetupFailure::malformed_json, "malformed JSON is rejected");
    }
    {
        const auto project = base / "oversize";
        write_file(project / ".lingtai/alpha/init.json",
            std::string(1024U * 1024U + 1U, 'x'));
        write_file(project / ".lingtai/alpha/.agent.json",
            R"({"agent_id":"id","agent_name":"alpha"})");
        expect_load_failure_unchanged(project, "alpha",
            AgentSetupFailure::oversized, "oversize input is rejected");
    }
    {
        const auto project = base / "nonregular";
        fs::create_directories(project / ".lingtai/alpha/init.json");
        write_file(project / ".lingtai/alpha/.agent.json",
            R"({"agent_id":"id","agent_name":"alpha"})");
        expect_load_failure_unchanged(project, "alpha",
            AgentSetupFailure::not_regular, "non-regular input is rejected");
    }
    {
        const auto project = base / "symlink";
        const auto outside = base / "outside-init.json";
        write_file(outside, "{}");
        fs::create_directories(project / ".lingtai/alpha");
        fs::create_symlink(outside, project / ".lingtai/alpha/init.json");
        write_file(project / ".lingtai/alpha/.agent.json",
            R"({"agent_id":"id","agent_name":"alpha"})");
        expect_load_failure_unchanged(project, "alpha",
            AgentSetupFailure::symlink_rejected, "symlink leaf is rejected");
    }
    {
        const auto project = base / "external-env-symlink";
        const auto target = base / "external-env-target/.env";
        const auto link = base / "external-env-link";
        write_file(target, "SECRET=outside\n");
        fs::create_directories(project / ".lingtai");
        make_valid_agent(project, "alpha",
            preset_block("old", "old", {"old"}), target, false);
        fs::create_symlink(target.parent_path(), link);
        auto init = read_json(project / ".lingtai/alpha/init.json");
        init["env_file"] = QString::fromStdString((link / ".env").string());
        write_file(project / ".lingtai/alpha/init.json", json_bytes(init));
        const auto before = tree_snapshot(base);
        AgentSetupStore store(attach(project));
        const auto loaded = store.load("alpha");
        require(!loaded && loaded.failure == AgentSetupFailure::unsafe_path,
            "absolute env path with a symlink component is rejected");
        require(tree_snapshot(base) == before,
            "external env symlink rejection leaves all fixture bytes unchanged");
    }
    {
        const auto project = base / "missing-identity";
        write_file(project / ".lingtai/alpha/init.json", R"({"manifest":{}})");
        write_file(project / ".lingtai/alpha/.agent.json", R"({"agent_name":"alpha"})");
        expect_load_failure_unchanged(project, "alpha",
            AgentSetupFailure::missing_identity, "missing required identity is rejected");
    }
    {
        const auto project = base / "unsafe-key";
        fs::create_directories(project);
        expect_load_failure_unchanged(project, "../alpha",
            AgentSetupFailure::unsafe_agent_key, "unsafe selected leaf is rejected");
    }
}

void test_transaction_failures(const fs::path &base) {
    const auto project = base / "transaction";
    const auto env = base / "global-transaction/.env";
    write_file(env, "SECRET=x\n");
    require(::chmod(env.c_str(), 0600) == 0, "transaction env fixture mode is set");
    make_valid_agent(project, "alpha",
        preset_block("old", "old", {"old"}), env);
    make_valid_agent(project, "beta",
        preset_block("old", "old", {"old"}), env, false);
    AgentSetupStore store(attach(project));
    auto loaded = store.load("alpha");
    require(static_cast<bool>(loaded),
        "transaction fixture loads: " + loaded.detail);
    auto draft = loaded.state->draft;
    draft.language = "wen";
    draft.soul_flow_enabled = true;
    const auto before = tree_snapshot(base);

    const auto staged = store.save(
        *loaded.state, draft, AgentSetupFailurePoint::staging_after_first);
    require(!staged && staged.failure == AgentSetupFailure::staging_failed,
        "injected staging failure is surfaced");
    require(tree_snapshot(base) == before,
        "staging failure leaves every target unchanged");
    require(file_mode(env) == 0600, "staging failure preserves external env mode");
    expect_no_transaction_artifacts(base);

    const auto published = store.save(
        *loaded.state, draft, AgentSetupFailurePoint::publish_after_first);
    require(!published && published.failure == AgentSetupFailure::publish_failed,
        "injected publish failure is surfaced");
    require(tree_snapshot(base) == before,
        "publish failure restores project and external env bytes without partial damage");
    require(file_mode(env) == 0600, "publish rollback preserves external env mode");
    expect_no_transaction_artifacts(base);
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "fixture root argument is required");
        const auto base = fs::path(argv[1]);
        std::error_code error;
        fs::remove_all(base, error);
        require(!error, "old fixture root is removed");
        fs::create_directories(base, error);
        require(!error, "fixture root is created");

        test_reconciliation_and_preservation(base);
        test_external_env_on_off(base);
        test_keep_current_and_noop(base);
        test_empty_allowed_preserves_selected_and_peers(base);
        test_unrelated_admin_boolean_is_not_orchestrator(base);
        test_invalid_inputs(base);
        test_transaction_failures(base);

        fs::remove_all(base, error);
        require(!error, "fixture root is removed");
        std::cout << "agent setup store contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
