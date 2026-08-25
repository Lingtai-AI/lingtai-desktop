#pragma once

#include <QtWidgets/QWidget>

class QKeyEvent;

namespace lingtai::desktop {

// Project creation and existing-Agent setup share this in-window shell.
class ProjectSetupWizard final : public QWidget {
    Q_OBJECT

public:
    explicit ProjectSetupWizard(QWidget *parent = nullptr);

    void reject();

signals:
    void rejected();

protected:
    void keyPressEvent(QKeyEvent *event) override;
};

} // namespace lingtai::desktop
