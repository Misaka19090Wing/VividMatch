#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>

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
