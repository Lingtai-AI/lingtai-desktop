#pragma once

#include "kanban_model.h"

#include <QtWidgets/QWidget>

#include <filesystem>
#include <optional>
#include <vector>

class QEvent;
class QGridLayout;
class QKeyEvent;
class QLabel;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QStackedWidget;
class QWidget;

namespace lingtai::desktop {

class KanbanPage final : public QWidget {
    Q_OBJECT

public:
    explicit KanbanPage(QWidget *parent = nullptr);

    void set_board(
        const KanbanBoard &board,
        const std::optional<std::filesystem::path> &selected_key);
    void apply_chrome();
    void choose_agent(const QString &directory_key);

signals:
    void agent_selected(const QString &directory_key);
    void presets_requested();
    void back_requested();
    void reload_requested();

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuild();
    void relayout();
    void show_summary();
    void show_detail();
    bool consume_detail_shortcut(QKeyEvent *event);
    const KanbanAgent *selected_agent() const;

    KanbanBoard board_;
    std::optional<std::filesystem::path> selected_key_;
    bool detail_open_ = false;
    QWidget *hero_host_ = nullptr;
    QWidget *hero_row_ = nullptr;
    QWidget *actions_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    QWidget *summary_body_ = nullptr;
    QWidget *detail_body_ = nullptr;
    QPushButton *detail_button_ = nullptr;
    QGridLayout *metrics_grid_ = nullptr;
    QGridLayout *columns_grid_ = nullptr;
    QGridLayout *model_grid_ = nullptr;
    QGridLayout *legend_grid_ = nullptr;
    QGridLayout *caps_grid_ = nullptr;
    QWidget *left_column_ = nullptr;
    QWidget *right_column_ = nullptr;
    std::vector<QWidget *> metric_cells_;
    std::vector<QWidget *> metric_rules_;
    std::vector<QWidget *> model_cells_;
    std::vector<QWidget *> legend_items_;
    std::vector<QWidget *> cap_pills_;
    bool applying_chrome_ = false;
};

} // namespace lingtai::desktop
