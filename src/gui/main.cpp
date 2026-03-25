#include <QApplication>
#include <QIcon>
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("punto-switcher-config");
    app.setApplicationDisplayName("Punto Switcher Settings");
    app.setOrganizationName("PuntoSwitcher");
    app.setOrganizationDomain("punto-switcher.local");

    // Use a system icon if available, fall back to a built-in placeholder
    app.setWindowIcon(QIcon::fromTheme("input-keyboard",
                      QIcon::fromTheme("preferences-desktop-keyboard")));

    MainWindow w;
    w.show();
    return app.exec();
}
