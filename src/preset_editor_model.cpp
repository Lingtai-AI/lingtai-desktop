#include "preset_editor_model.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTextStream>

#include <algorithm>
#include <map>
#include <optional>

namespace lingtai::desktop {
namespace {

QJsonObject clone_object(const QJsonObject &object) {
    return QJsonDocument::fromJson(QJsonDocument(object).toJson()).object();
}

QString json_string(const QJsonValue &value) {
    return value.isString() ? value.toString() : QString();
}

const std::map<QString, QStringList> &provider_models() {
    static const auto catalog = std::map<QString, QStringList>{
        {QStringLiteral("minimax"), {
            QStringLiteral("MiniMax-M3"),
            QStringLiteral("MiniMax-M2.7"),
            QStringLiteral("MiniMax-M2.7-highspeed"),
        }},
        {QStringLiteral("zhipu"), {
            QStringLiteral("GLM-5.2"), QStringLiteral("GLM-5.1"),
            QStringLiteral("glm-5.2"), QStringLiteral("glm-5.1"),
        }},
        {QStringLiteral("mimo"), {
            QStringLiteral("mimo-v2.5"), QStringLiteral("mimo-v2.5-pro"),
            QStringLiteral("mimo-v2-pro"), QStringLiteral("mimo-v2-omni"),
        }},
        {QStringLiteral("deepseek"), {
            QStringLiteral("deepseek-v4-pro"), QStringLiteral("deepseek-v4-flash"),
        }},
        {QStringLiteral("grok"), {QStringLiteral("grok-4.5")}},
        {QStringLiteral("nvidia"), {
            QStringLiteral("meta/llama-3.3-70b-instruct"),
            QStringLiteral("meta/llama-3.1-70b-instruct"),
            QStringLiteral("qwen/qwen3-coder-480b-a35b-instruct"),
            QStringLiteral("moonshotai/kimi-k2-thinking"),
            QStringLiteral("openai/gpt-oss-120b"),
            QStringLiteral("nvidia/llama-3.1-nemotron-ultra-253b-v1"),
            QStringLiteral("mistralai/mistral-nemotron"),
            QStringLiteral("microsoft/phi-4-mini-instruct"),
        }},
        {QStringLiteral("codex"), {
            QStringLiteral("gpt-5.6-sol"), QStringLiteral("gpt-5.6-terra"),
            QStringLiteral("gpt-5.6-luna"), QStringLiteral("gpt-5.5"),
        }},
        {QStringLiteral("codex-pool"), {
            QStringLiteral("gpt-5.6-sol"), QStringLiteral("gpt-5.6-terra"),
            QStringLiteral("gpt-5.6-luna"), QStringLiteral("gpt-5.5"),
        }},
        {QStringLiteral("claude-code"), {
            QStringLiteral("opus"), QStringLiteral("fable"),
            QStringLiteral("sonnet"), QStringLiteral("haiku"),
        }},
        {QStringLiteral("claude_code"), {
            QStringLiteral("opus"), QStringLiteral("fable"),
            QStringLiteral("sonnet"), QStringLiteral("haiku"),
        }},
        {QStringLiteral("claude-agent-sdk"), {
            QStringLiteral("opus"), QStringLiteral("fable"),
            QStringLiteral("sonnet"), QStringLiteral("haiku"),
        }},
        {QStringLiteral("claude_agent_sdk"), {
            QStringLiteral("opus"), QStringLiteral("fable"),
            QStringLiteral("sonnet"), QStringLiteral("haiku"),
        }},
    };
    return catalog;
}

const std::map<QString, QVector<PresetRegionOption>> &provider_regions() {
    static const auto regions = std::map<QString, QVector<PresetRegionOption>>{
        {QStringLiteral("deepseek"), {
            {QStringLiteral("DeepSeek API"),
                QStringLiteral("https://api.deepseek.com"),
                QStringLiteral("DEEPSEEK_API_KEY")},
            {QStringLiteral("OpenCode Go"),
                QStringLiteral("https://opencode.ai/zen/go/v1"),
                QStringLiteral("OPENCODE_GO_API_KEY")},
            {QStringLiteral("Custom"), {}, {}},
        }},
        {QStringLiteral("zhipu"), {
            {QStringLiteral("CN"),
                QStringLiteral("https://open.bigmodel.cn/api/coding/paas/v4"), {}},
            {QStringLiteral("INTL"),
                QStringLiteral("https://api.z.ai/api/coding/paas/v4"), {}},
            {QStringLiteral("OpenCode Go"),
                QStringLiteral("https://opencode.ai/zen/go/v1"),
                QStringLiteral("OPENCODE_GO_API_KEY")},
        }},
        {QStringLiteral("minimax"), {
            {QStringLiteral("CN"),
                QStringLiteral("https://api.minimaxi.com/anthropic"), {}},
            {QStringLiteral("INTL"),
                QStringLiteral("https://api.minimax.io/anthropic"), {}},
            {QStringLiteral("OpenCode Go"),
                QStringLiteral("https://opencode.ai/zen/go/v1"),
                QStringLiteral("OPENCODE_GO_API_KEY")},
        }},
        {QStringLiteral("kimi"), {
            {QStringLiteral("Kimi Code"),
                QStringLiteral("https://api.kimi.com/coding/v1"), {}},
            {QStringLiteral("OpenCode Go"),
                QStringLiteral("https://opencode.ai/zen/go/v1"),
                QStringLiteral("OPENCODE_GO_API_KEY")},
            {QStringLiteral("Custom"), {}, {}},
        }},
        {QStringLiteral("mimo"), {
            {QStringLiteral("MiMo"),
                QStringLiteral("https://api.xiaomimimo.com/v1"), {}},
            {QStringLiteral("OpenCode Go"),
                QStringLiteral("https://opencode.ai/zen/go/v1"),
                QStringLiteral("OPENCODE_GO_API_KEY")},
            {QStringLiteral("Custom"), {}, {}},
        }},
        {QStringLiteral("grok"), {
            {QStringLiteral("OpenCode Go"),
                QStringLiteral("https://opencode.ai/zen/go/v1"),
                QStringLiteral("OPENCODE_GO_API_KEY")},
            {QStringLiteral("Custom"), {}, {}},
        }},
    };
    return regions;
}

const std::map<QString, QString> &provider_default_model() {
    static const auto models = std::map<QString, QString>{
        {QStringLiteral("gemini"), QStringLiteral("gemini-3-flash-preview")},
        {QStringLiteral("kimi"), QStringLiteral("kimi-for-coding")},
        {QStringLiteral("openrouter"), QStringLiteral("z-ai/glm-5.1")},
        {QStringLiteral("claude-code"), QStringLiteral("opus")},
        {QStringLiteral("claude_code"), QStringLiteral("opus")},
        {QStringLiteral("claude-agent-sdk"), QStringLiteral("opus")},
        {QStringLiteral("claude_agent_sdk"), QStringLiteral("opus")},
        {QStringLiteral("nvidia"), QStringLiteral("meta/llama-3.3-70b-instruct")},
    };
    return models;
}

const std::map<QString, QString> &provider_fixed_base_url() {
    static const auto urls = std::map<QString, QString>{
        {QStringLiteral("nvidia"),
            QStringLiteral("https://integrate.api.nvidia.com/v1")},
        {QStringLiteral("codex"),
            QStringLiteral("https://chatgpt.com/backend-api/codex")},
        {QStringLiteral("codex_oauth"),
            QStringLiteral("https://chatgpt.com/backend-api/codex")},
        {QStringLiteral("codex-pool"),
            QStringLiteral("https://chatgpt.com/backend-api/codex")},
        {QStringLiteral("codex_pool"),
            QStringLiteral("https://chatgpt.com/backend-api/codex")},
        {QStringLiteral("gemini"), {}},
        {QStringLiteral("openrouter"), {}},
        {QStringLiteral("custom"), {}},
        {QStringLiteral("claude-code"), {}},
        {QStringLiteral("claude_code"), {}},
        {QStringLiteral("claude-agent-sdk"), {}},
        {QStringLiteral("claude_agent_sdk"), {}},
    };
    return urls;
}

std::optional<QString> default_base_url_for(const QString &provider) {
    const auto regions = provider_regions().find(provider);
    if (regions != provider_regions().end() && !regions->second.isEmpty()) {
        return regions->second.front().url;
    }
    const auto found = provider_fixed_base_url().find(provider);
    if (found != provider_fixed_base_url().end()) return found->second;
    return std::nullopt;
}

const std::map<QString, QString> &provider_default_env() {
    static const auto env = std::map<QString, QString>{
        {QStringLiteral("minimax"), QStringLiteral("MINIMAX_API_KEY")},
        {QStringLiteral("zhipu"), QStringLiteral("ZHIPU_API_KEY")},
        {QStringLiteral("mimo"), QStringLiteral("XIAOMI_API_KEY")},
        {QStringLiteral("deepseek"), QStringLiteral("DEEPSEEK_API_KEY")},
        {QStringLiteral("gemini"), QStringLiteral("GEMINI_API_KEY")},
        {QStringLiteral("kimi"), QStringLiteral("KIMI_CODE_API_KEY")},
        {QStringLiteral("grok"), QStringLiteral("GROK_API_KEY")},
        {QStringLiteral("nvidia"), QStringLiteral("NVIDIA_API_KEY")},
        {QStringLiteral("openrouter"), QStringLiteral("OPENROUTER_API_KEY")},
        {QStringLiteral("custom"), QStringLiteral("LLM_API_KEY")},
    };
    return env;
}

QString region_suffix(const QString &provider, const QString &base_url) {
    const auto found = provider_regions().find(provider);
    if (found != provider_regions().end()) {
        for (const auto &region : found->second) {
            if (!region.url.isEmpty() && region.url == base_url && !region.env.isEmpty()) {
                return {};
            }
        }
    }
    if (provider == QLatin1String("minimax")) {
        return base_url.contains(QLatin1String("minimaxi.com"))
            ? QStringLiteral("CN") : QStringLiteral("INTL");
    }
    if (provider == QLatin1String("zhipu")) {
        return base_url.contains(QLatin1String("api.z.ai"))
            ? QStringLiteral("INTL") : QStringLiteral("CN");
    }
    return {};
}

bool region_declared_env(const QString &provider, const QString &env) {
    if (env.isEmpty()) return false;
    const auto found = provider_regions().find(provider);
    if (found == provider_regions().end()) return false;
    return std::any_of(found->second.begin(), found->second.end(),
        [&](const PresetRegionOption &region) { return region.env == env; });
}

bool uses_region_declared_env(const QJsonObject &llm) {
    const auto env = json_string(llm.value(QLatin1String("api_key_env")));
    const auto base_url = json_string(llm.value(QLatin1String("base_url")));
    if (env.isEmpty() || base_url.isEmpty()) return false;
    const auto provider = json_string(llm.value(QLatin1String("provider")));
    const auto found = provider_regions().find(provider);
    if (found == provider_regions().end()) return false;
    const auto default_env = provider_default_env().find(provider);
    for (const auto &region : found->second) {
        if (region.url == base_url && !region.env.isEmpty()) {
            return region.env == env
                && (default_env == provider_default_env().end()
                    || region.env != default_env->second);
        }
    }
    return false;
}

bool is_thinking_level(const QString &value) {
    return value == QLatin1String("none")
        || value == QLatin1String("minimal")
        || value == QLatin1String("low")
        || value == QLatin1String("medium")
        || value == QLatin1String("high")
        || value == QLatin1String("xhigh");
}

bool llm_has_level_thinking(const QJsonObject &llm) {
    const auto provider = json_string(llm.value(QLatin1String("provider")));
    const auto family = credential_family(provider);
    if (family == QLatin1String("codex_single")
            || family == QLatin1String("codex_pool")) {
        return false;
    }
    if (provider == QLatin1String("anthropic")
            || provider == QLatin1String("openai")) {
        return true;
    }
    const auto compat = json_string(llm.value(QLatin1String("api_compat")));
    return compat == QLatin1String("openai")
        || compat == QLatin1String("anthropic");
}

QString env_file_path(const QString &global_dir) {
    return QDir(global_dir).filePath(QStringLiteral(".env"));
}

QString read_env_value(const QString &path, const QString &key) {
    QFile file(path);
    if (key.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const auto line = stream.readLine().trimmed();
        if (line.startsWith(QLatin1Char('#')) || !line.contains(QLatin1Char('='))) {
            continue;
        }
        const auto split = line.indexOf(QLatin1Char('='));
        if (line.left(split) == key) {
            auto value = line.mid(split + 1);
            if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
                value = value.mid(1, value.size() - 2);
            }
            return value;
        }
    }
    return {};
}

QStringList env_key_names(const QString &path) {
    auto names = QStringList();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return names;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const auto line = stream.readLine().trimmed();
        if (line.startsWith(QLatin1Char('#')) || !line.contains(QLatin1Char('='))) {
            continue;
        }
        names.push_back(line.left(line.indexOf(QLatin1Char('='))));
    }
    return names;
}

bool write_env_value(const QString &path, const QString &key, const QString &value) {
    if (key.isEmpty()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    auto lines = QStringList();
    auto replaced = false;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const auto line = stream.readLine();
            const auto trimmed = line.trimmed();
            if (!trimmed.startsWith(QLatin1Char('#')) && trimmed.contains(QLatin1Char('='))
                    && trimmed.left(trimmed.indexOf(QLatin1Char('='))) == key) {
                if (!value.isEmpty()) {
                    lines.push_back(key + QLatin1Char('=') + value);
                }
                replaced = true;
                continue;
            }
            lines.push_back(line);
        }
        file.close();
    }
    if (!replaced && !value.isEmpty()) {
        lines.push_back(key + QLatin1Char('=') + value);
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    for (const auto &line : lines) {
        out << line << QLatin1Char('\n');
    }
    return true;
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

bool write_json_file(const QString &path, const QJsonObject &object) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
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

QString auto_env_var_name(const QJsonObject &llm, const QStringList &existing) {
    const auto provider = json_string(llm.value(QLatin1String("provider"))).toUpper();
    if (provider.isEmpty()) return {};
    auto prefix = provider;
    const auto suffix = region_suffix(
        json_string(llm.value(QLatin1String("provider"))),
        json_string(llm.value(QLatin1String("base_url"))));
    if (!suffix.isEmpty()) {
        prefix += QLatin1Char('_') + suffix;
    }
    const auto want = prefix + QLatin1Char('_');
    auto used = std::map<int, bool>{};
    for (const auto &name : existing) {
        if (!name.startsWith(want) || !name.endsWith(QLatin1String("_API_KEY"))) {
            continue;
        }
        const auto mid = name.mid(want.size(),
            name.size() - want.size() - 8);
        bool ok = false;
        const auto n = mid.toInt(&ok);
        if (ok && n > 0 && mid == QString::number(n)) {
            used[n] = true;
        }
    }
    for (auto n = 1; ; ++n) {
        if (!used[n]) {
            return prefix + QLatin1Char('_') + QString::number(n)
                + QStringLiteral("_API_KEY");
        }
    }
}

void sync_capability_api_key_env(QJsonObject &manifest) {
    const auto llm = manifest.value(QLatin1String("llm")).toObject();
    const auto provider = json_string(llm.value(QLatin1String("provider")));
    const auto env = json_string(llm.value(QLatin1String("api_key_env")));
    if (provider.isEmpty() || env.isEmpty()) return;
    auto caps = manifest.value(QLatin1String("capabilities")).toObject();
    if (caps.isEmpty()) return;
    for (auto it = caps.begin(); it != caps.end(); ++it) {
        if (!it->isObject()) continue;
        auto cfg = it->toObject();
        if (json_string(cfg.value(QLatin1String("provider"))) == provider) {
            cfg.insert(QStringLiteral("api_key_env"), env);
            it.value() = cfg;
        }
    }
    manifest.insert(QStringLiteral("capabilities"), caps);
}

} // namespace

QString lingtai_global_dir() {
    const auto override = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("LINGTAI_TUI_DIR")).trimmed();
    if (!override.isEmpty()) return override;
    return QDir::homePath() + QStringLiteral("/.lingtai-tui");
}

QString expand_lingtai_path(const QString &path) {
    if (path.startsWith(QLatin1String("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    return path;
}

QStringList saved_preset_names(const QString &global_dir) {
    auto names = QStringList();
    QDir dir(QDir(global_dir).filePath(QStringLiteral("presets/saved")));
    for (const auto &entry : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        names.push_back(QFileInfo(entry).completeBaseName());
    }
    return names;
}

QString auto_saved_preset_name(
        const QString &template_name, const QStringList &existing) {
    if (template_name.isEmpty()) return {};
    const auto prefix = template_name + QLatin1Char('-');
    auto used = std::map<int, bool>{};
    for (const auto &name : existing) {
        if (!name.startsWith(prefix)) continue;
        const auto mid = name.mid(prefix.size());
        bool ok = false;
        const auto n = mid.toInt(&ok);
        if (ok && n > 0 && mid == QString::number(n)) {
            used[n] = true;
        }
    }
    for (auto n = 1; ; ++n) {
        if (!used[n]) {
            return prefix + QString::number(n);
        }
    }
}

QString credential_family(const QString &provider) {
    if (provider == QLatin1String("codex")
            || provider == QLatin1String("codex_oauth")) {
        return QStringLiteral("codex_single");
    }
    if (provider == QLatin1String("codex-pool")
            || provider == QLatin1String("codex_pool")) {
        return QStringLiteral("codex_pool");
    }
    if (provider == QLatin1String("claude-code")
            || provider == QLatin1String("claude_code")
            || provider == QLatin1String("claude-agent-sdk")
            || provider == QLatin1String("claude_agent_sdk")) {
        return QStringLiteral("claude_cli");
    }
    return QStringLiteral("other");
}

bool validate_safe_preset_name(const QString &name, QString *error) {
    if (name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("must not be blank");
        return false;
    }
    if (name == QLatin1String(".") || name == QLatin1String("..")) {
        if (error) *error = QStringLiteral("must not be \"%1\"").arg(name);
        return false;
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        if (error) *error = QStringLiteral("must not contain a path separator");
        return false;
    }
    return true;
}

void PresetEditorModel::load(const PresetEditorLoadRequest &request) {
    original_ = {};
    working_ = {};
    original_name_ = request.name;
    source_path_ = expand_lingtai_path(request.path);
    loaded_from_disk_ = false;
    is_template_ = request.is_template
        || request.source.compare(QLatin1String("template"), Qt::CaseInsensitive) == 0;
    api_key_.clear();
    existing_api_key_.clear();
    api_key_set_ = false;
    region_env_before_adopt_.clear();

    auto ok = false;
    auto root = load_json_file(source_path_, &ok);
    if (!ok) {
        root = QJsonObject{
            {QStringLiteral("name"), request.name},
            {QStringLiteral("description"), QJsonObject{
                {QStringLiteral("summary"), request.summary},
            }},
            {QStringLiteral("manifest"), QJsonObject{
                {QStringLiteral("llm"), QJsonObject{}},
            }},
        };
    } else {
        loaded_from_disk_ = true;
    }
    if (!root.contains(QLatin1String("name")) && !request.name.isEmpty()) {
        root.insert(QStringLiteral("name"), request.name);
    }
    auto description = root.value(QLatin1String("description"));
    if (description.isString()) {
        root.insert(QStringLiteral("description"), QJsonObject{
            {QStringLiteral("summary"), description.toString()},
        });
    } else if (!description.isObject()) {
        root.insert(QStringLiteral("description"), QJsonObject{
            {QStringLiteral("summary"), request.summary},
        });
    }
    original_ = clone_object(root);
    working_ = clone_object(root);
    original_name_ = name();
    if (original_name_.isEmpty()) {
        original_name_ = request.name;
        set_name(request.name);
        original_ = clone_object(working_);
    }
    if (!is_template_) {
        existing_api_key_ = read_env_value(
            env_file_path(lingtai_global_dir()), api_key_env());
        api_key_ = existing_api_key_;
    }
}

QJsonObject PresetEditorModel::llm() const {
    return working_.value(QLatin1String("manifest")).toObject()
        .value(QLatin1String("llm")).toObject();
}

void PresetEditorModel::put_llm(QJsonObject llm_object) {
    auto manifest = working_.value(QLatin1String("manifest")).toObject();
    manifest.insert(QStringLiteral("llm"), llm_object);
    working_.insert(QStringLiteral("manifest"), manifest);
}

QString PresetEditorModel::llm_string(const QString &key) const {
    return json_string(llm().value(key));
}

void PresetEditorModel::set_llm_string(const QString &key, const QString &value) {
    auto object = llm();
    if (value.isEmpty()) {
        object.remove(key);
    } else {
        object.insert(key, value);
    }
    put_llm(object);
}

void PresetEditorModel::remove_llm_key(const QString &key) {
    auto object = llm();
    object.remove(key);
    put_llm(object);
}

QJsonObject PresetEditorModel::description() const {
    return working_.value(QLatin1String("description")).toObject();
}

void PresetEditorModel::put_description(QJsonObject object) {
    working_.insert(QStringLiteral("description"), object);
}

QString PresetEditorModel::name() const {
    return json_string(working_.value(QLatin1String("name")));
}

void PresetEditorModel::set_name(const QString &value) {
    working_.insert(QStringLiteral("name"), value);
}

QString PresetEditorModel::summary() const {
    return json_string(description().value(QLatin1String("summary")));
}

void PresetEditorModel::set_summary(const QString &value) {
    auto object = description();
    object.insert(QStringLiteral("summary"), value);
    put_description(object);
}

QString PresetEditorModel::tier() const {
    return json_string(description().value(QLatin1String("tier")));
}

void PresetEditorModel::set_tier(const QString &value) {
    auto object = description();
    if (value.isEmpty()) {
        object.remove(QStringLiteral("tier"));
    } else {
        object.insert(QStringLiteral("tier"), value);
    }
    put_description(object);
}

QString PresetEditorModel::extra(const QString &key) const {
    return json_string(description().value(key));
}

void PresetEditorModel::set_extra(const QString &key, const QString &value) {
    auto object = description();
    if (value.trimmed().isEmpty()) {
        object.remove(key);
    } else {
        object.insert(key, value);
    }
    put_description(object);
}

QString PresetEditorModel::provider() const {
    return llm_string(QStringLiteral("provider"));
}

void PresetEditorModel::set_provider(const QString &value) {
    const auto old = provider();
    set_llm_string(QStringLiteral("provider"), value);
    if (old != value) {
        apply_provider_defaults(old, value);
    }
}

QString PresetEditorModel::model() const {
    return llm_string(QStringLiteral("model"));
}

void PresetEditorModel::set_model(const QString &value) {
    set_llm_string(QStringLiteral("model"), value);
}

QString PresetEditorModel::service_tier() const {
    return llm_string(QStringLiteral("service_tier")) == QLatin1String("fast")
        ? QStringLiteral("fast") : QStringLiteral("normal");
}

void PresetEditorModel::set_service_tier(const QString &value) {
    if (!is_codex_provider() || value != QLatin1String("fast")) {
        remove_llm_key(QStringLiteral("service_tier"));
        return;
    }
    set_llm_string(QStringLiteral("service_tier"), QStringLiteral("fast"));
}

QString PresetEditorModel::thinking() const {
    if (is_codex_thinking_provider()) {
        const auto value = llm_string(QStringLiteral("thinking"));
        if (value == QLatin1String("low") || value == QLatin1String("medium")
                || value == QLatin1String("high") || value == QLatin1String("xhigh")) {
            return value;
        }
        return QStringLiteral("xhigh");
    }
    if (llm_has_level_thinking(llm())) {
        const auto value = llm_string(QStringLiteral("thinking"));
        return is_thinking_level(value) ? value : QStringLiteral("default");
    }
    return {};
}

void PresetEditorModel::set_thinking(const QString &effort) {
    if (is_codex_thinking_provider()) {
        if (effort == QLatin1String("low") || effort == QLatin1String("medium")
                || effort == QLatin1String("high") || effort == QLatin1String("xhigh")) {
            set_llm_string(QStringLiteral("thinking"), effort);
        } else {
            set_llm_string(QStringLiteral("thinking"), QStringLiteral("xhigh"));
        }
        return;
    }
    if (!llm_has_level_thinking(llm()) || !is_thinking_level(effort)) {
        remove_llm_key(QStringLiteral("thinking"));
        return;
    }
    set_llm_string(QStringLiteral("thinking"), effort);
}

QString PresetEditorModel::api_compat() const {
    return llm_string(QStringLiteral("api_compat"));
}

void PresetEditorModel::set_api_compat(const QString &compat) {
    set_llm_string(QStringLiteral("api_compat"), compat);
    auto root = working_;
    normalize_for_commit(root);
    working_ = root;
}

QString PresetEditorModel::wire_api() const {
    const auto value = llm_string(QStringLiteral("wire_api"));
    return value.isEmpty() ? QStringLiteral("auto") : value;
}

void PresetEditorModel::set_wire_api(const QString &wire) {
    if (wire.isEmpty() || wire == QLatin1String("auto")) {
        remove_llm_key(QStringLiteral("wire_api"));
    } else {
        set_llm_string(QStringLiteral("wire_api"), wire);
    }
    auto root = working_;
    normalize_for_commit(root);
    working_ = root;
}

QString PresetEditorModel::responses_transport() const {
    return llm_string(QStringLiteral("responses_transport")) == QLatin1String("websocket")
        ? QStringLiteral("websocket") : QStringLiteral("http");
}

void PresetEditorModel::set_responses_transport(const QString &transport) {
    if (transport == QLatin1String("websocket")) {
        set_llm_string(QStringLiteral("responses_transport"), transport);
    } else {
        remove_llm_key(QStringLiteral("responses_transport"));
    }
}

QString PresetEditorModel::base_url() const {
    return llm_string(QStringLiteral("base_url"));
}

void PresetEditorModel::set_base_url(const QString &url) {
    const auto provider_name = provider();
    const auto regions = region_options();
    const auto previous_index = selected_region_index();
    PresetRegionOption previous;
    if (previous_index >= 0 && previous_index < regions.size()) {
        previous = regions[previous_index];
    }
    auto llm_object = llm();
    llm_object.insert(QStringLiteral("base_url"), url);
    put_llm(llm_object);
    auto next = PresetRegionOption{};
    const auto next_index = selected_region_index();
    if (next_index >= 0 && next_index < regions.size()) {
        next = regions[next_index];
    }
    if (!next.env.isEmpty()) {
        const auto current = api_key_env();
        if (previous.env.isEmpty() && !region_declared_env(provider_name, current)) {
            region_env_before_adopt_ = current;
        }
        set_llm_string(QStringLiteral("api_key_env"), next.env);
    } else if (region_declared_env(provider_name, api_key_env())) {
        auto restore = region_env_before_adopt_;
        if (restore.isEmpty()) {
            const auto found = provider_default_env().find(provider_name);
            if (found != provider_default_env().end()) restore = found->second;
        }
        set_llm_string(QStringLiteral("api_key_env"), restore);
        region_env_before_adopt_.clear();
    }
}

QString PresetEditorModel::api_key_env() const {
    return llm_string(QStringLiteral("api_key_env"));
}

void PresetEditorModel::set_api_key(const QString &key) {
    api_key_ = key;
    api_key_set_ = true;
}

QString PresetEditorModel::codex_auth_ref() const {
    return llm_string(QStringLiteral("codex_auth_path"));
}

void PresetEditorModel::set_codex_auth_ref(const QString &ref) {
    if (!is_codex_provider() || ref.trimmed().isEmpty()) {
        remove_llm_key(QStringLiteral("codex_auth_path"));
        return;
    }
    set_llm_string(QStringLiteral("codex_auth_path"), ref);
}

bool PresetEditorModel::is_codex_provider() const {
    return credential_family(provider()) == QLatin1String("codex_single");
}

bool PresetEditorModel::is_codex_thinking_provider() const {
    const auto family = credential_family(provider());
    return family == QLatin1String("codex_single")
        || family == QLatin1String("codex_pool");
}

bool PresetEditorModel::is_codex_pool_provider() const {
    return credential_family(provider()) == QLatin1String("codex_pool");
}

bool PresetEditorModel::is_claude_cli_provider() const {
    return credential_family(provider()) == QLatin1String("claude_cli");
}

bool PresetEditorModel::uses_api_key_field() const {
    return credential_family(provider()) == QLatin1String("other");
}

bool PresetEditorModel::service_tier_visible() const {
    return is_codex_provider();
}

bool PresetEditorModel::thinking_visible() const {
    return is_codex_thinking_provider() || llm_has_level_thinking(llm());
}

bool PresetEditorModel::wire_api_visible() const {
    return provider() == QLatin1String("custom")
        && api_compat() == QLatin1String("openai");
}

bool PresetEditorModel::responses_transport_visible() const {
    return wire_api_visible() && wire_api() == QLatin1String("responses");
}

QStringList PresetEditorModel::provider_options() const {
    // TUI BuiltinPresets order. The TUI cycle omits kimi/gemini/codex-pool/
    // claude because ←/→ is tedious; a dropdown can offer the full set.
    auto options = QStringList{
        QStringLiteral("minimax"), QStringLiteral("zhipu"),
        QStringLiteral("mimo"), QStringLiteral("deepseek"),
        QStringLiteral("gemini"), QStringLiteral("kimi"),
        QStringLiteral("grok"), QStringLiteral("nvidia"),
        QStringLiteral("openrouter"), QStringLiteral("codex"),
        QStringLiteral("codex-pool"), QStringLiteral("claude-code"),
        QStringLiteral("custom"),
    };
    const auto current = provider();
    if (!current.isEmpty() && !options.contains(current)) {
        options.push_back(current);
    }
    return options;
}

QStringList PresetEditorModel::model_options() const {
    const auto found = provider_models().find(provider());
    if (found == provider_models().end()) return {};
    auto options = found->second;
    const auto current = model();
    if (!current.isEmpty() && !options.contains(current)) {
        options.push_front(current);
    }
    return options;
}

bool PresetEditorModel::model_has_picker() const {
    return provider_models().contains(provider());
}

QStringList PresetEditorModel::thinking_options() const {
    if (is_codex_thinking_provider()) {
        return {QStringLiteral("low"), QStringLiteral("medium"),
            QStringLiteral("high"), QStringLiteral("xhigh")};
    }
    if (llm_has_level_thinking(llm())) {
        return {QStringLiteral("default"), QStringLiteral("none"),
            QStringLiteral("minimal"), QStringLiteral("low"),
            QStringLiteral("medium"), QStringLiteral("high"),
            QStringLiteral("xhigh")};
    }
    return {};
}

QVector<PresetRegionOption> PresetEditorModel::region_options() const {
    const auto found = provider_regions().find(provider());
    if (found == provider_regions().end()) return {};
    return found->second;
}

int PresetEditorModel::selected_region_index() const {
    const auto regions = region_options();
    const auto current = base_url();
    for (auto index = 0; index != regions.size(); ++index) {
        if (!regions[index].url.isEmpty() && regions[index].url == current) {
            return index;
        }
    }
    for (auto index = 0; index != regions.size(); ++index) {
        if (regions[index].url.isEmpty()) return index;
    }
    return -1;
}

QVector<CodexAccount> PresetEditorModel::codex_accounts() const {
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

QString PresetEditorModel::codex_bound_label() const {
    const auto ref = codex_auth_ref();
    const auto global = lingtai_global_dir();
    const auto path = ref.isEmpty()
        ? QDir(global).filePath(QStringLiteral("codex-auth.json"))
        : expand_lingtai_path(ref);
    const auto account = read_codex_account(path, ref, ref.isEmpty());
    if (!account.email.isEmpty()) return account.email;
    if (!account.label.isEmpty()) return account.label;
    if (ref.isEmpty()) return QStringLiteral("default account");
    return QFileInfo(path).completeBaseName();
}

bool PresetEditorModel::codex_bound_valid() const {
    const auto ref = codex_auth_ref();
    const auto global = lingtai_global_dir();
    const auto path = ref.isEmpty()
        ? QDir(global).filePath(QStringLiteral("codex-auth.json"))
        : expand_lingtai_path(ref);
    return read_codex_account(path, ref, ref.isEmpty()).valid;
}

bool PresetEditorModel::codex_auth_allows_editor() const {
    const auto family = credential_family(provider());
    if (family == QLatin1String("codex_single")) {
        return loaded_from_disk_ && codex_bound_valid();
    }
    if (family == QLatin1String("codex_pool")) {
        if (!loaded_from_disk_) return true;
        const auto accounts = codex_accounts();
        return std::any_of(accounts.begin(), accounts.end(),
            [](const CodexAccount &account) { return account.valid; });
    }
    return true;
}

void PresetEditorModel::apply_provider_defaults(
        const QString &old_provider, const QString &new_provider) {
    Q_UNUSED(old_provider);
    remove_llm_key(QStringLiteral("thinking"));
    const auto models = provider_models().find(new_provider);
    if (models != provider_models().end() && !models->second.isEmpty()) {
        if (!models->second.contains(model())) {
            set_model(models->second.front());
        }
    } else {
        const auto fallback = provider_default_model().find(new_provider);
        if (fallback != provider_default_model().end()) {
            set_model(fallback->second);
        }
    }
    if (const auto url = default_base_url_for(new_provider)) {
        set_llm_string(QStringLiteral("base_url"), *url);
    }
    const auto regions = provider_regions().find(new_provider);
    const auto env = provider_default_env().find(new_provider);
    if (env != provider_default_env().end()) {
        auto want = env->second;
        if (regions != provider_regions().end() && !regions->second.isEmpty()
                && !regions->second.front().env.isEmpty()) {
            want = regions->second.front().env;
        }
        set_llm_string(QStringLiteral("api_key_env"), want);
    }
    region_env_before_adopt_.clear();
    auto root = working_;
    normalize_for_commit(root);
    working_ = root;
}

void PresetEditorModel::normalize_for_commit(QJsonObject &root) const {
    auto manifest = root.value(QLatin1String("manifest")).toObject();
    auto llm_object = manifest.value(QLatin1String("llm")).toObject();
    const auto provider_name = json_string(llm_object.value(QLatin1String("provider")));
    const auto family = credential_family(provider_name);
    if (family != QLatin1String("codex_single")
            || json_string(llm_object.value(QLatin1String("service_tier")))
                != QLatin1String("fast")) {
        llm_object.remove(QStringLiteral("service_tier"));
    }
    if (family == QLatin1String("codex_single")
            || family == QLatin1String("codex_pool")) {
        const auto thinking_value = json_string(llm_object.value(QLatin1String("thinking")));
        if (thinking_value != QLatin1String("low")
                && thinking_value != QLatin1String("medium")
                && thinking_value != QLatin1String("high")
                && thinking_value != QLatin1String("xhigh")) {
            llm_object.insert(QStringLiteral("thinking"), QStringLiteral("xhigh"));
        }
    } else if (llm_has_level_thinking(llm_object)) {
        if (!is_thinking_level(json_string(llm_object.value(QLatin1String("thinking"))))) {
            llm_object.remove(QStringLiteral("thinking"));
        }
    } else {
        llm_object.remove(QStringLiteral("thinking"));
    }
    const auto custom_openai = provider_name == QLatin1String("custom")
        && json_string(llm_object.value(QLatin1String("api_compat")))
            == QLatin1String("openai");
    if (custom_openai) {
        if (json_string(llm_object.value(QLatin1String("wire_api")))
                == QLatin1String("auto")) {
            llm_object.remove(QStringLiteral("wire_api"));
        }
    } else {
        llm_object.remove(QStringLiteral("wire_api"));
    }
    const auto custom_responses = custom_openai
        && json_string(llm_object.value(QLatin1String("wire_api")))
            == QLatin1String("responses");
    if (!custom_responses
            || json_string(llm_object.value(QLatin1String("responses_transport")))
                != QLatin1String("websocket")) {
        llm_object.remove(QStringLiteral("responses_transport"));
    }
    manifest.insert(QStringLiteral("llm"), llm_object);
    root.insert(QStringLiteral("manifest"), manifest);
}

void PresetEditorModel::stamp_auto_env(
        QJsonObject &root, const QStringList &existing_env_names) const {
    auto manifest = root.value(QLatin1String("manifest")).toObject();
    auto llm_object = manifest.value(QLatin1String("llm")).toObject();
    if (credential_family(json_string(llm_object.value(QLatin1String("provider"))))
            != QLatin1String("other")) {
        return;
    }
    if (!json_string(llm_object.value(QLatin1String("api_key_env"))).isEmpty()) {
        return;
    }
    const auto minted = auto_env_var_name(llm_object, existing_env_names);
    if (minted.isEmpty()) return;
    llm_object.insert(QStringLiteral("api_key_env"), minted);
    manifest.insert(QStringLiteral("llm"), llm_object);
    root.insert(QStringLiteral("manifest"), manifest);
}

QStringList PresetEditorModel::validate() const {
    auto errors = QStringList();
    if (summary().trimmed().isEmpty()) {
        errors.push_back(QStringLiteral("description.summary must be non-empty"));
    }
    const auto tier_value = tier();
    if (!tier_value.isEmpty()
            && !QStringList{QStringLiteral("1"), QStringLiteral("2"),
                QStringLiteral("3"), QStringLiteral("4"),
                QStringLiteral("5")}.contains(tier_value)) {
        errors.push_back(QStringLiteral("description.tier must be one of 1..5"));
    }
    const auto llm_object = llm();
    if (!working_.value(QLatin1String("manifest")).toObject()
            .contains(QLatin1String("llm"))) {
        errors.push_back(QStringLiteral("manifest.llm must be an object"));
        return errors;
    }
    if (provider().isEmpty()) {
        errors.push_back(QStringLiteral("manifest.llm.provider must be non-empty"));
    }
    if (model().isEmpty()) {
        errors.push_back(QStringLiteral("manifest.llm.model must be non-empty"));
    }
    if (provider_regions().contains(provider())) {
        if (llm_object.contains(QLatin1String("base_url")) && base_url().isEmpty()) {
            errors.push_back(QStringLiteral(
                "manifest.llm.base_url must be non-empty for this provider"));
        } else if (!llm_object.contains(QLatin1String("base_url"))
                && region_suffix(provider(), {}).isEmpty()) {
            errors.push_back(QStringLiteral(
                "manifest.llm.base_url must be non-empty for this provider"));
        }
    }
    return errors;
}

bool PresetEditorModel::has_semantic_edits() const {
    if (name() != original_name_) return true;
    const auto working_manifest = QJsonDocument(
        working_.value(QLatin1String("manifest")).toObject()).toJson(QJsonDocument::Compact);
    const auto original_manifest = QJsonDocument(
        original_.value(QLatin1String("manifest")).toObject()).toJson(QJsonDocument::Compact);
    return working_manifest != original_manifest;
}

PresetEditorCommit PresetEditorModel::commit(const QStringList &existing_names) const {
    PresetEditorCommit result;
    if (!loaded_from_disk_) {
        result.ok = true;
        result.name = name().isEmpty() ? original_name_ : name();
        result.document = working_;
        result.api_key = api_key_;
        result.api_key_set = api_key_set_;
        return result;
    }
    const auto errors = validate();
    if (!errors.isEmpty()) {
        result.error = errors.front();
        return result;
    }
    QString name_error;
    if (!validate_safe_preset_name(name(), &name_error)) {
        result.error = QStringLiteral("invalid preset name: ") + name_error;
        return result;
    }
    auto committed = clone_object(working_);
    if (is_template_ && (has_semantic_edits() || api_key_set_)) {
        if (name() == original_name_) {
            const auto auto_name = auto_saved_preset_name(original_name_, existing_names);
            if (!auto_name.isEmpty()) {
                committed.insert(QStringLiteral("name"), auto_name);
            }
        }
        auto manifest = committed.value(QLatin1String("manifest")).toObject();
        auto llm_object = manifest.value(QLatin1String("llm")).toObject();
        if (!uses_region_declared_env(llm_object)) {
            llm_object.remove(QStringLiteral("api_key_env"));
            manifest.insert(QStringLiteral("llm"), llm_object);
            committed.insert(QStringLiteral("manifest"), manifest);
        }
    }
    normalize_for_commit(committed);
    stamp_auto_env(committed, env_key_names(env_file_path(lingtai_global_dir())));
    auto manifest = committed.value(QLatin1String("manifest")).toObject();
    sync_capability_api_key_env(manifest);
    committed.insert(QStringLiteral("manifest"), manifest);
    result.ok = true;
    result.name = json_string(committed.value(QLatin1String("name")));
    result.document = committed;
    result.api_key = api_key_;
    result.api_key_set = api_key_set_;
    return result;
}

bool PresetEditorModel::save_commit(PresetEditorCommit &result) const {
    if (!result.ok) return false;
    if (!loaded_from_disk_) return true;
    const auto path = QDir(lingtai_global_dir())
        .filePath(QStringLiteral("presets/saved/") + result.name + QStringLiteral(".json"));
    if (!write_json_file(path, result.document)) {
        result.ok = false;
        result.error = QStringLiteral("failed to write saved preset");
        return false;
    }
    result.wrote_disk = true;
    if (result.api_key_set && uses_api_key_field()) {
        const auto env = json_string(result.document.value(QLatin1String("manifest"))
            .toObject().value(QLatin1String("llm")).toObject()
            .value(QLatin1String("api_key_env")));
        write_env_value(env_file_path(lingtai_global_dir()), env, result.api_key);
    }
    return true;
}

} // namespace lingtai::desktop
