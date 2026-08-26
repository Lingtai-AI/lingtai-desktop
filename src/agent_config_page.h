#pragma once

#include "agent_setup_store.h"

#include <QtCore/QString>
#include <QtWidgets/QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QSpinBox;
class QVBoxLayout;

namespace lingtai::desktop {

class AgentConfigPage final : public QWidget {
    Q_OBJECT

public:
    explicit AgentConfigPage(QWidget *parent = nullptr);

    void load(const QString &default_preset, int allowed_count);
    void load_existing(const AgentSetupDraft &draft,
        const QString &folder_name, const QString &default_preset,
        int allowed_count);
    [[nodiscard]] AgentSetupDraft apply_to_draft(
        AgentSetupDraft draft) const;
    void set_existing_mode(bool existing);
    void install_dialog_status(QWidget *status);
    [[nodiscard]] QString agent_name() const;
    [[nodiscard]] QString folder_name() const;
    [[nodiscard]] QString language() const;
    [[nodiscard]] QString create_status() const;
    void apply_chrome();

signals:
    void back_requested();
    void create_requested();

private:
    void update_prompt_paths();
    void update_status(const QString &default_preset, int allowed_count);

    QLineEdit *name_ = nullptr;
    QLineEdit *folder_ = nullptr;
    QComboBox *language_ = nullptr;
    QSpinBox *context_limit_ = nullptr;
    QSpinBox *soul_cadence_ = nullptr;
    QSpinBox *max_rpm_ = nullptr;
    QSpinBox *max_aed_ = nullptr;
    QCheckBox *karma_ = nullptr;
    QCheckBox *nirvana_ = nullptr;
    QCheckBox *soul_flow_ = nullptr;
    QLineEdit *covenant_ = nullptr;
    QLineEdit *soul_path_ = nullptr;
    QPlainTextEdit *comment_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *commit_ = nullptr;
    QVBoxLayout *footer_host_ = nullptr;
    bool folder_dirty_ = false;
    bool covenant_dirty_ = false;
    bool soul_path_dirty_ = false;
};

} // namespace lingtai::desktop
