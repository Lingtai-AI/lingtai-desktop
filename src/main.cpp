#include "ui/rp_widget.h"

#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <iostream>

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    QWidget window;
    window.setObjectName("lingtai_desktop_window");
    window.resize(320, 180);

    auto *control = new Ui::RpWidget(&window);
    control->setObjectName("lingtai_real_lib_ui_rp_widget");
    control->setGeometry(10, 10, 200, 80);
    control->show();
    window.show();

    QTimer::singleShot(120, &app, [&] {
        std::cout
            << "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK"
            << " qt=" << qVersion()
            << " class=" << control->metaObject()->className()
            << " object=" << control->objectName().toStdString()
            << " size=" << control->width() << "x" << control->height()
            << std::endl;
        app.quit();
    });
    QTimer::singleShot(3000, &app, [&] {
        std::cerr << "LINGTAI_LIB_UI_SMOKE_TIMEOUT" << std::endl;
        app.exit(99);
    });

    return app.exec();
}
