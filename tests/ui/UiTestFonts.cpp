#include "UiTestFonts.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>

namespace lingtai::desktop::ui_test {

void applyUiTestFontDefaults() {
#ifdef LINGTAI_UI_TEST_FONTS_DIR
    const QDir fonts_dir(QStringLiteral(LINGTAI_UI_TEST_FONTS_DIR));
    for (const auto *file : {
            "OpenSans-Regular.ttf",
            "OpenSans-SemiBold.ttf",
        }) {
        const auto path = fonts_dir.filePath(QString::fromUtf8(file));
        if (QFile::exists(path)) {
            QFontDatabase::addApplicationFont(path);
        }
    }
#endif

    QFont::insertSubstitutions(QStringLiteral("Open Sans"), {
        QStringLiteral(".AppleSystemUIFont"),
        QStringLiteral("Helvetica Neue"),
        QStringLiteral("Helvetica"),
        QStringLiteral("Arial"),
    });
}

} // namespace lingtai::desktop::ui_test
