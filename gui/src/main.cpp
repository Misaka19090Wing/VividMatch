#include <QApplication>

#include "ui/main/languagemanager.h"
#include "ui/main/mainwin.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("VividMatch"));
    app.setApplicationName(QStringLiteral("VividMatchGui"));
    app.setStyle(QStringLiteral("Fusion"));

    // The translation has to be installed before the window is built, so the
    // first paint is already in the right language rather than switching after a
    // visible flash of the other one.
    LanguageManager languages;
    languages.initialise(&app);

    MainWin window;
    window.show();
    return app.exec();
}
