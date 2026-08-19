#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;

namespace lingtai::desktop {

class CodexCredentialsStrip final : public QWidget {
    Q_OBJECT

public:
    explicit CodexCredentialsStrip(QWidget *parent = nullptr);

    void refresh();

signals:
    void manage_requested();

protected:
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void apply_chrome();

    QLabel *title_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *action_ = nullptr;
    bool hovered_ = false;
    bool applying_chrome_ = false;
};

} // namespace lingtai::desktop
