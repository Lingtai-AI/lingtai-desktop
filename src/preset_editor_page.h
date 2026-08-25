#pragma once

#include "preset_editor_model.h"

#include <QtWidgets/QWidget>

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QCheckBox;

namespace lingtai::desktop {

class ChoiceStrip;

class PresetEditorPage final : public QWidget {
    Q_OBJECT

public:
    explicit PresetEditorPage(QWidget *parent = nullptr);

    void load(const PresetEditorLoadRequest &request);
    void refresh_credentials();
    [[nodiscard]] PresetEditorModel &model() noexcept { return model_; }
    [[nodiscard]] const PresetEditorModel &model() const noexcept { return model_; }

signals:
    void cancelled();
    void saved(const QString &preset_name);
    void credentials_requested();

private:
    void rebuild_from_model();
    void sync_conditional_rows();
    void pull_text_fields();
    void on_save();
    void on_manage_credential();
    void changeEvent(QEvent *event) override;

    PresetEditorModel model_;
    QStringList existing_names_;

    QLineEdit *name_ = nullptr;
    QLineEdit *summary_ = nullptr;
    ChoiceStrip *tier_ = nullptr;
    QLineEdit *gains_ = nullptr;
    QLineEdit *losses_ = nullptr;
    QComboBox *provider_ = nullptr;
    QComboBox *model_combo_ = nullptr;
    QLineEdit *model_edit_ = nullptr;
    QWidget *service_tier_row_ = nullptr;
    ChoiceStrip *service_tier_ = nullptr;
    QWidget *thinking_row_ = nullptr;
    ChoiceStrip *thinking_ = nullptr;
    ChoiceStrip *api_compat_ = nullptr;
    QWidget *wire_api_row_ = nullptr;
    ChoiceStrip *wire_api_ = nullptr;
    QWidget *transport_row_ = nullptr;
    ChoiceStrip *transport_ = nullptr;
    QWidget *region_row_ = nullptr;
    ChoiceStrip *regions_ = nullptr;
    QLineEdit *base_url_ = nullptr;
    QWidget *credential_row_ = nullptr;
    QLabel *credential_status_ = nullptr;
    QPushButton *manage_ = nullptr;
    QWidget *api_key_row_ = nullptr;
    QLineEdit *api_key_ = nullptr;
    QLabel *error_ = nullptr;
    QPushButton *save_ = nullptr;
    bool rebuilding_ = false;
};

} // namespace lingtai::desktop
