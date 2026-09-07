#include <QApplication>

#include "ui/main/mainwin.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("VividMatch"));
    app.setApplicationName(QStringLiteral("VividMatchGui"));
    app.setStyle(QStringLiteral("Fusion"));

    MainWin window;
    window.show();
    return app.exec();
}
