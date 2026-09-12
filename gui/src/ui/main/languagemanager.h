#ifndef LANGUAGEMANAGER_H
#define LANGUAGEMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>

class QApplication;
class QTranslator;

// Loads the UI translation and applies it at run time.
//
// English is the source language: the strings in the code are English, and the
// Chinese text lives in a .qm file compiled from the .ts next to this class. That
// way the application is readable English even when no translation has been built
// or the translation cannot be loaded, instead of falling back to a language the
// reader may not know.
//
// The choice is stored with QSettings, so it survives a restart. By default the
// language follows the operating system's UI language, which is why the stored
// value distinguishes "follow the system" from an explicit pick.
class LanguageManager : public QObject
{
    Q_OBJECT

public:
    enum class Language {
        System,  // follow the operating system
        English,
        Chinese,
    };
    Q_DECLARE_FLAGS(Languages, Language)

    explicit LanguageManager(QObject* parent = nullptr);
    ~LanguageManager() override;

    // Reads the stored preference and installs that translation. Call once,
    // after the QApplication exists.
    void initialise(QApplication* app);

    Language preference() const { return m_preference; }
    // The language actually in use, i.e. the preference with System resolved.
    Language effective() const { return m_effective; }

    // Applies and stores a choice. Returns true when the visible language changed,
    // in which case the caller should expect a LanguageChange event.
    bool setLanguage(Language language);

    // Name of the translation file for a language, without the .qm suffix.
    static QString translationName(Language language);

signals:
    void languageChanged();

private:
    void install();

    QApplication* m_app;
    QTranslator* m_translator;
    Language m_preference;
    Language m_effective;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(LanguageManager::Languages)

#endif // LANGUAGEMANAGER_H
