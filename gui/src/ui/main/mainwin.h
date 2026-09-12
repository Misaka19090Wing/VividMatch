#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>
#include <QKeySequence>
#include <QString>
#include <QVector>

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

private:
    // One comparison mode, as offered both in the 文件 menu and on the home page.
    // Holding them in one list keeps the two entry points in step.
    struct ModeEntry {
        QString menuLabel;   // single line, for the menu
        QString buttonLabel; // two lines: title and explanation, for the button
        QKeySequence shortcut;
        void (MainWin::*show)();
    };

    QVector<ModeEntry> modeEntries() const;

    void setupMenus();
    QWidget* createHomePage();
    void showHomePage();
    void showImageComparePage();
    void showBatchComparePage();
    void showVideoComparePage();
    void showVideoBatchPage();

private:
    QStackedWidget* m_stack;
    QWidget* m_homePage;
    ImageComparePage* m_imagePage;
    BatchComparePage* m_batchPage;
    VideoComparePage* m_videoPage;
    VideoBatchPage* m_videoBatchPage;
};

#endif // MAINWIN_H
