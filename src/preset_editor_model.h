#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace lingtai::desktop {

struct PresetRegionOption {
    QString label;
    QString url;
    QString env;
};

struct CodexAccount {
    QString path;
    QString ref;
    QString label;
    QString email;
    bool legacy = false;
    bool valid = false;
};

struct PresetEditorLoadRequest {
    QString path;
    QString name;
    QString summary;
    QString source;
    bool is_template = false;
    QStringList existing_names;
};

struct PresetEditorCommit {
    bool ok = false;
    bool wrote_disk = false;
    QString error;
    QString name;
    QJsonObject document;
    QString api_key;
    bool api_key_set = false;
};

class PresetEditorModel final {
public:
    void load(const PresetEditorLoadRequest &request);

    [[nodiscard]] bool loaded_from_disk() const noexcept { return loaded_from_disk_; }
    [[nodiscard]] bool is_template() const noexcept { return is_template_; }
    [[nodiscard]] QJsonObject document() const { return working_; }
    [[nodiscard]] QString original_name() const { return original_name_; }

    [[nodiscard]] QString name() const;
    void set_name(const QString &name);

    [[nodiscard]] QString summary() const;
    void set_summary(const QString &summary);

    [[nodiscard]] QString tier() const;
    void set_tier(const QString &tier);

    [[nodiscard]] QString extra(const QString &key) const;
    void set_extra(const QString &key, const QString &value);

    [[nodiscard]] QString provider() const;
    void set_provider(const QString &provider);

    [[nodiscard]] QString model() const;
    void set_model(const QString &model);

    [[nodiscard]] QString service_tier() const;
    void set_service_tier(const QString &tier);

    [[nodiscard]] QString thinking() const;
    void set_thinking(const QString &effort);

    [[nodiscard]] QString api_compat() const;
    void set_api_compat(const QString &compat);

    [[nodiscard]] QString wire_api() const;
    void set_wire_api(const QString &wire);

    [[nodiscard]] QString responses_transport() const;
    void set_responses_transport(const QString &transport);

    [[nodiscard]] QString base_url() const;
    void set_base_url(const QString &url);

    [[nodiscard]] QString api_key_env() const;
    void set_api_key(const QString &key);
    [[nodiscard]] bool api_key_set() const noexcept { return api_key_set_; }
    [[nodiscard]] QString api_key() const { return api_key_; }
    [[nodiscard]] bool has_saved_api_key() const noexcept { return !existing_api_key_.isEmpty(); }

    [[nodiscard]] QString codex_auth_ref() const;
    void set_codex_auth_ref(const QString &ref);

    [[nodiscard]] bool service_tier_visible() const;
    [[nodiscard]] bool thinking_visible() const;
    [[nodiscard]] bool wire_api_visible() const;
    [[nodiscard]] bool responses_transport_visible() const;
    [[nodiscard]] bool is_codex_provider() const;
    [[nodiscard]] bool is_codex_thinking_provider() const;
    [[nodiscard]] bool is_codex_pool_provider() const;
    [[nodiscard]] bool is_claude_cli_provider() const;
    [[nodiscard]] bool uses_api_key_field() const;

    [[nodiscard]] QStringList provider_options() const;
    [[nodiscard]] QStringList model_options() const;
    [[nodiscard]] bool model_has_picker() const;
    [[nodiscard]] QStringList thinking_options() const;
    [[nodiscard]] QVector<PresetRegionOption> region_options() const;
    [[nodiscard]] int selected_region_index() const;

    [[nodiscard]] QVector<CodexAccount> codex_accounts() const;
    [[nodiscard]] QString codex_bound_label() const;
    [[nodiscard]] bool codex_bound_valid() const;
    [[nodiscard]] bool codex_auth_allows_editor() const;

    [[nodiscard]] QStringList validate() const;
    [[nodiscard]] bool has_semantic_edits() const;
    [[nodiscard]] PresetEditorCommit commit(const QStringList &existing_names) const;
    [[nodiscard]] bool save_commit(PresetEditorCommit &commit) const;

private:
    [[nodiscard]] QJsonObject llm() const;
    void put_llm(QJsonObject llm);
    [[nodiscard]] QString llm_string(const QString &key) const;
    void set_llm_string(const QString &key, const QString &value);
    void remove_llm_key(const QString &key);
    [[nodiscard]] QJsonObject description() const;
    void put_description(QJsonObject description);
    void apply_provider_defaults(const QString &old_provider, const QString &new_provider);
    void normalize_for_commit(QJsonObject &root) const;
    void stamp_auto_env(QJsonObject &root, const QStringList &existing_env_names) const;

    QJsonObject original_;
    QJsonObject working_;
    QString original_name_;
    QString source_path_;
    bool loaded_from_disk_ = false;
    bool is_template_ = false;
    QString api_key_;
    QString existing_api_key_;
    bool api_key_set_ = false;
    QString region_env_before_adopt_;
};

[[nodiscard]] QString lingtai_global_dir();
[[nodiscard]] QString expand_lingtai_path(const QString &path);
[[nodiscard]] QStringList saved_preset_names(const QString &global_dir);
[[nodiscard]] QString auto_saved_preset_name(
    const QString &template_name, const QStringList &existing);
[[nodiscard]] QString credential_family(const QString &provider);
[[nodiscard]] bool validate_safe_preset_name(const QString &name, QString *error);

} // namespace lingtai::desktop
