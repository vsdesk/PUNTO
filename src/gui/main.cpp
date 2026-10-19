#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale(QLocale::Russian));
    QTranslator qtBase;
    if (qtBase.load(QLocale(QLocale::Russian), QStringLiteral("qtbase"), QStringLiteral("_"),
                    QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(&qtBase);
    }
    app.setApplicationName("rocket-switcher-config");
    app.setApplicationDisplayName("Настройки Ракета");
    app.setOrganizationName("Raketa");
    app.setOrganizationDomain("rocket-switcher.local");

    // Use a system icon if available, fall back to a built-in placeholder
    app.setWindowIcon(QIcon::fromTheme("input-keyboard",
                      QIcon::fromTheme("preferences-desktop-keyboard")));

    MainWindow w;
    w.show();
    return app.exec();
}
