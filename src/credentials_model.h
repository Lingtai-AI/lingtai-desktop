#pragma once

#include "preset_editor_model.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace lingtai::desktop {

[[nodiscard]] QVector<CodexAccount> list_codex_accounts();
[[nodiscard]] QString codex_account_display_name(const CodexAccount &account);
[[nodiscard]] QString new_codex_auth_path(const QString &email);
[[nodiscard]] bool save_codex_tokens(
    const QJsonObject &tokens, const QString &absolute_path);
[[nodiscard]] bool remove_codex_account_file(const QString &absolute_path);
[[nodiscard]] int count_valid_codex_accounts();

struct CodexCredentialsSummary {
    bool has_accounts = false;
    bool has_valid = false;
    QString primary_label;
};

[[nodiscard]] CodexCredentialsSummary codex_credentials_summary();

} // namespace lingtai::desktop
