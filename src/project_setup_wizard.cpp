#include "project_setup_wizard.h"

#include <QtGui/QKeyEvent>

namespace lingtai::desktop {

ProjectSetupWizard::ProjectSetupWizard(QWidget *parent)
: QWidget(parent) {
    setObjectName("lingtai_project_setup_wizard");
    setAccessibleName(QStringLiteral("Set up LingTai project"));
    setMinimumSize(840, 600);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
}

void ProjectSetupWizard::reject() {
    emit rejected();
}

void ProjectSetupWizard::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        reject();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace lingtai::desktop
