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

private slots:
    // Reacts to a language switch: rewrites the window's own text, then tells the
    // user that a restart applies the change everywhere.
    void onLanguageChanged();

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
    // Tells the user a restart applies the language everywhere. Shown once per
    // switch unless the previous notice is still on screen.
    void showRestartNotice();
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
    // The restart notice is shown once per switch; a second switch while it is up
    // must not stack another copy.
    bool m_restartNoticeShowing;

#ifdef VIVIDMATCH_TEST_HOOKS
    // Lets a harness reach the language manager to drive a switch without a real
    // menu click, and read the notice it shows.
    friend class MainWinTestHook;
#endif
};

#endif // MAINWIN_H
