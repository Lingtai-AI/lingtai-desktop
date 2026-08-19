#pragma once

#include <QtWidgets/QCheckBox>
#include <QtCore/QVariantAnimation>

class QPaintEvent;

namespace lingtai::desktop {

class SetupToggle final : public QCheckBox {
public:
    explicit SetupToggle(QWidget *parent = nullptr);

    void set_checked(bool checked, bool animate);

protected:
    void paintEvent(QPaintEvent *event) override;
    [[nodiscard]] bool hitButton(const QPoint &pos) const override;

private:
    void snap_to(bool checked);
    void animate_to(bool checked);

    QVariantAnimation animation_;
    qreal progress_ = 0.0;
};

} // namespace lingtai::desktop
