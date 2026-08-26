#pragma once

#include "preset_catalog_presentation.h"

#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

class QComboBox;
class QEvent;
class QLabel;
class QPushButton;

namespace lingtai::desktop {

class AgentPresetsPage final : public QWidget {
    Q_OBJECT

public:
    explicit AgentPresetsPage(QWidget *parent = nullptr);

    void load_from_chooser(QComboBox *chooser, const QString &preferred_default);
    void load_existing(const QString &default_name,
        const QStringList &allowed_names, const QString &active_name,
        const std::vector<PresetCatalogRow> &catalog_rows = {});
    [[nodiscard]] QString default_name() const;
    [[nodiscard]] QStringList allowed_names() const;
    [[nodiscard]] int allowed_count() const;
    [[nodiscard]] bool has_saved_presets() const;
    void apply_chrome();

signals:
    void back_requested();
    void continue_requested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Row {
        QString name;
        QString reference;
        QString summary;
        QString provider;
        QString model;
        QStringList tags;
        bool allowed = false;
        bool active = false;
        QWidget *widget = nullptr;
        QWidget *toggle = nullptr;
        QLabel *default_label = nullptr;
    };

    void rebuild_rows();
    void apply_filter(const QString &query);
    void set_default_row(int index);
    void set_allowed(int index, bool allowed, bool animate);
    void refresh_row_chrome(int index, bool animate_toggle = false);

    QWidget *list_ = nullptr;
    QLabel *empty_ = nullptr;
    QLabel *message_ = nullptr;
    QPushButton *continue_ = nullptr;
    QVector<Row> rows_;
    QStringList loaded_allowed_names_;
    int default_index_ = -1;
    bool existing_mode_ = false;
};

} // namespace lingtai::desktop
