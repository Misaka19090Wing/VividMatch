#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>

class QStackedWidget;

class BatchComparePage;
class ImageComparePage;

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

private:
    QStackedWidget* m_stack;
    QWidget* m_homePage;
    ImageComparePage* m_imagePage;
    BatchComparePage* m_batchPage;
};

#endif // MAINWIN_H
