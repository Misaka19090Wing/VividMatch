#include "languagemanager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace {

constexpr char kSettingsGroup[] = "ui";
constexpr char kLanguageKey[] = "language";

// Where the compiled translations live. Both are tried: next to the executable
// (the portable package copies them there) and in the build tree's i18n folder
// (running from a build directory).
QStringList translationDirectories()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return {
        appDir + QStringLiteral("/translations"),
        appDir + QStringLiteral("/i18n"),
        appDir + QStringLiteral("/../i18n"),
        appDir,
    };
}

// Resolves a preference to a concrete language, using the operating system when
// the preference is "follow the system".
LanguageManager::Language resolveSystemLanguage()
{
    const QString name = QLocale::system().name();  // e.g. "zh_CN", "en_US"
    return name.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive)
               ? LanguageManager::Language::Chinese
               : LanguageManager::Language::English;
}

QString preferenceToString(LanguageManager::Language language)
{
    switch (language) {
        case LanguageManager::Language::Chinese:
            return QStringLiteral("zh_CN");
        case LanguageManager::Language::English:
            return QStringLiteral("en");
        case LanguageManager::Language::System:
            break;
    }
    return QStringLiteral("system");
}

LanguageManager::Language preferenceFromString(const QString& value)
{
    if (value == QStringLiteral("zh_CN")) {
        return LanguageManager::Language::Chinese;
    }
    if (value == QStringLiteral("en")) {
        return LanguageManager::Language::English;
    }
    return LanguageManager::Language::System;
}

}  // namespace

LanguageManager::LanguageManager(QObject* parent)
    : QObject(parent)
    , m_app(nullptr)
    , m_translator(new QTranslator(this))
    , m_preference(Language::System)
    , m_effective(Language::English)
{
}

LanguageManager::~LanguageManager() = default;

QString LanguageManager::translationName(Language language)
{
    switch (language) {
        case Language::Chinese:
            return QStringLiteral("vividmatch_zh_CN");
        case Language::English:
        case Language::System:
            break;
    }
    // English is the source language, so there is nothing to load.
    return QString();
}

void LanguageManager::initialise(QApplication* app)
{
    m_app = app;

    QSettings settings;
    settings.beginGroup(QLatin1String(kSettingsGroup));
    m_preference = preferenceFromString(settings.value(QLatin1String(kLanguageKey)).toString());
    settings.endGroup();

    install();
}

void LanguageManager::install()
{
    if (m_app == nullptr) {
        return;
    }

    // Remove a previously installed translation first, so switching languages
    // cannot end up with two translators stacked.
    m_app->removeTranslator(m_translator);

    m_effective = m_preference == Language::System ? resolveSystemLanguage() : m_preference;

    const QString name = translationName(m_effective);
    if (name.isEmpty()) {
        return;  // source language: the English strings are already in the code
    }

    // A missing .qm is not fatal: the application stays in English. That is the
    // reason English is the source language.
    for (const QString& directory : translationDirectories()) {
        if (m_translator->load(name, directory)) {
            m_app->installTranslator(m_translator);
            return;
        }
    }
}

bool LanguageManager::setLanguage(Language language)
{
    const Language previous = m_effective;
    m_preference = language;

    // Stored so the choice survives a restart. On Windows QSettings writes to
    // HKEY_CURRENT_USER; a policy that denies registry writes makes this a silent
    // no-op (status() reports it), in which case the language still switches for
    // this run and simply is not remembered.
    QSettings settings;
    settings.beginGroup(QLatin1String(kSettingsGroup));
    settings.setValue(QLatin1String(kLanguageKey), preferenceToString(language));
    settings.endGroup();
    settings.sync();

    install();
    // Emitted even when the resolved language did not change, because the
    // preference might have (System -> an explicit pick of the same language),
    // and the UI shows which one is selected.
    emit languageChanged();
    return m_effective != previous;
}
