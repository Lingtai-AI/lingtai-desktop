#pragma once

#include <QtWidgets/QWidget>

class QKeyEvent;

namespace lingtai::desktop {

// New-project setup lives in the main window content pane, not a separate dialog.
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
