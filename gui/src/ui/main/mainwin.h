#ifndef MAINWIN_H
#define MAINWIN_H

#include <QKeySequence>
#include <QMainWindow>
#include <QString>
#include <QVector>

#include "languagemanager.h"

class QAction;
class QEvent;
class QLabel;
class QMenu;
class QPushButton;
class QStackedWidget;

class BatchComparePage;
class ImageComparePage;
class VideoComparePage;
class VideoBatchPage;

class MainWin : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWin(QWidget* parent = nullptr);
    ~MainWin() override;

protected:
    // Qt sends this when a translator is installed or removed, which is how the
    // window reacts to a language switch without being rebuilt.
    void changeEvent(QEvent* event) override;

private:
    // One comparison mode, as offered both in the File menu and on the home page.
    // Holding them in one list keeps the two entry points in step.
    struct ModeEntry {
        QString menuLabel;   // single line, for the menu
        QString buttonLabel; // two lines: title and explanation, for the button
        QString windowTitle; // window title while this mode is showing
        QString shortcut;
        void (MainWin::*show)();
    };

    QVector<ModeEntry> modeEntries() const;

    void setupMenus();
    QWidget* createHomePage();
    void retranslateUi();
    // Page widget for a mode index, used to re-apply the window title.
    QWidget* pageForMode(int index) const;

    void showHomePage();
    void showImageComparePage();
    void showBatchComparePage();
    void showVideoComparePage();
    void showVideoBatchPage();

private:
    LanguageManager* m_languages;
    QStackedWidget* m_stack;
    QWidget* m_homePage;
    ImageComparePage* m_imagePage;
    BatchComparePage* m_batchPage;
    VideoComparePage* m_videoPage;
    VideoBatchPage* m_videoBatchPage;

    // Widgets and actions whose text depends on the language, kept so a switch can
    // rewrite them in place.
    QLabel* m_titleLabel;
    QLabel* m_subtitleLabel;
    QLabel* m_footerLabel;
    QVector<QPushButton*> m_modeButtons;
    QMenu* m_fileMenu;
    QMenu* m_languageMenu;
    QVector<QAction*> m_modeActions;
    QAction* m_languageSystemAction;
    QAction* m_languageEnglishAction;
    QAction* m_languageChineseAction;
    QAction* m_homeAction;
    QAction* m_exitAction;
};

#endif // MAINWIN_H
